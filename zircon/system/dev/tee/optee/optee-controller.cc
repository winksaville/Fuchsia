// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "optee-controller.h"

#include <inttypes.h>
#include <lib/fidl-utils/bind.h>
#include <string.h>

#include <limits>
#include <memory>
#include <utility>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/io-buffer.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <fbl/auto_lock.h>
#include <tee-client-api/tee-client-types.h>

#include "optee-client.h"

namespace optee {

namespace fuchsia_tee = ::llcpp::fuchsia::tee;

enum {
  kComponentPdev,
  kComponentSysmem,
  kComponentCount,
};

constexpr TEEC_UUID kOpteeOsUuid = {
    0x486178E0, 0xE7F8, 0x11E3, {0xBC, 0x5E, 0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};

static bool IsOpteeApi(const tee_smc::TrustedOsCallUidResult& returned_uid) {
  return returned_uid.uid_0_3 == kOpteeApiUid_0 && returned_uid.uid_4_7 == kOpteeApiUid_1 &&
         returned_uid.uid_8_11 == kOpteeApiUid_2 && returned_uid.uid_12_15 == kOpteeApiUid_3;
}

static bool IsOpteeApiRevisionSupported(const tee_smc::TrustedOsCallRevisionResult& returned_rev) {
  // The cast is unfortunately necessary to mute a compiler warning about an unsigned expression
  // always being greater than 0.
  ZX_DEBUG_ASSERT(returned_rev.minor <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
  return returned_rev.major == kOpteeApiRevisionMajor &&
         static_cast<int32_t>(returned_rev.minor) >= static_cast<int32_t>(kOpteeApiRevisionMinor);
}

zx_status_t OpteeController::ValidateApiUid() const {
  static const zx_smc_parameters_t kGetApiFuncCall =
      tee_smc::CreateSmcFunctionCall(tee_smc::kTrustedOsCallUidFuncId);
  union {
    zx_smc_result_t raw;
    tee_smc::TrustedOsCallUidResult uid;
  } result;
  zx_status_t status = zx_smc_call(secure_monitor_.get(), &kGetApiFuncCall, &result.raw);

  return status == ZX_OK ? IsOpteeApi(result.uid) ? ZX_OK : ZX_ERR_NOT_FOUND : status;
}

zx_status_t OpteeController::ValidateApiRevision() const {
  static const zx_smc_parameters_t kGetApiRevisionFuncCall =
      tee_smc::CreateSmcFunctionCall(tee_smc::kTrustedOsCallRevisionFuncId);
  union {
    zx_smc_result_t raw;
    tee_smc::TrustedOsCallRevisionResult revision;
  } result;
  zx_status_t status = zx_smc_call(secure_monitor_.get(), &kGetApiRevisionFuncCall, &result.raw);

  return status == ZX_OK
             ? IsOpteeApiRevisionSupported(result.revision) ? ZX_OK : ZX_ERR_NOT_SUPPORTED
             : status;
}

zx_status_t OpteeController::GetOsRevision() {
  static const zx_smc_parameters_t kGetOsRevisionFuncCall =
      tee_smc::CreateSmcFunctionCall(kGetOsRevisionFuncId);
  union {
    zx_smc_result_t raw;
    GetOsRevisionResult revision;
  } result;
  zx_status_t status = zx_smc_call(secure_monitor_.get(), &kGetOsRevisionFuncCall, &result.raw);

  if (status != ZX_OK) {
    return status;
  }

  os_revision_ = result.revision;

  return ZX_OK;
}

zx_status_t OpteeController::ExchangeCapabilities() {
  uint64_t nonsecure_world_capabilities = 0;
  if (zx_system_get_num_cpus() == 1) {
    nonsecure_world_capabilities |= kNonSecureCapUniprocessor;
  }

  const zx_smc_parameters_t func_call =
      tee_smc::CreateSmcFunctionCall(kExchangeCapabilitiesFuncId, nonsecure_world_capabilities);
  union {
    zx_smc_result_t raw;
    ExchangeCapabilitiesResult response;
  } result;

  zx_status_t status = zx_smc_call(secure_monitor_.get(), &func_call, &result.raw);

  if (status != ZX_OK) {
    return status;
  }

  if (result.response.status != kReturnOk) {
    return ZX_ERR_INTERNAL;
  }

  secure_world_capabilities_ = result.response.secure_world_capabilities;

  return ZX_OK;
}

zx_status_t OpteeController::InitializeSharedMemory() {
  zx_paddr_t shared_mem_start;
  size_t shared_mem_size;
  zx_status_t status = DiscoverSharedMemoryConfig(&shared_mem_start, &shared_mem_size);

  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to discover shared memory configuration\n");
    return status;
  }

  static constexpr uint32_t kTeeBtiIndex = 0;
  zx::bti bti;
  status = pdev_get_bti(&pdev_proto_, kTeeBtiIndex, bti.reset_and_get_address());
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get bti\n");
    return status;
  }

  // The Secure World memory is located at a fixed physical address in RAM, so we have to request
  // the platform device map the physical vmo for us.
  // TODO(rjascani): This currently maps the entire range of the Secure OS memory because pdev
  // doesn't currently have a way of only mapping a portion of it. OP-TEE tells us exactly the
  // physical sub range to use.
  static constexpr uint32_t kSecureWorldMemoryMmioIndex = 0;
  mmio_buffer_t mmio;
  status = pdev_map_mmio_buffer(&pdev_proto_, kSecureWorldMemoryMmioIndex, ZX_CACHE_POLICY_CACHED,
                                &mmio);
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to map secure world memory\n");
    return status;
  }

  status = SharedMemoryManager::Create(shared_mem_start, shared_mem_size, ddk::MmioBuffer(mmio),
                                       std::move(bti), &shared_memory_manager_);

  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to initialize SharedMemoryManager\n");
    return status;
  }

  return status;
}

zx_status_t OpteeController::DiscoverSharedMemoryConfig(zx_paddr_t* out_start_addr,
                                                        size_t* out_size) {
  static const zx_smc_parameters_t func_call =
      tee_smc::CreateSmcFunctionCall(kGetSharedMemConfigFuncId);

  union {
    zx_smc_result_t raw;
    GetSharedMemConfigResult response;
  } result;

  zx_status_t status = zx_smc_call(secure_monitor_.get(), &func_call, &result.raw);

  if (status != ZX_OK) {
    return status;
  }

  if (result.response.status != kReturnOk) {
    return ZX_ERR_INTERNAL;
  }

  *out_start_addr = result.response.start;
  *out_size = result.response.size;

  return status;
}

zx_status_t OpteeController::Create(void* ctx, zx_device_t* parent) {
  auto tee = std::make_unique<OpteeController>(parent);

  auto status = tee->Bind();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for tee
    __UNUSED auto ptr = tee.release();
  }

  return status;
}

zx_status_t OpteeController::Bind() {
  zx_status_t status = ZX_ERR_INTERNAL;

  composite_protocol_t composite;
  status = device_get_protocol(parent(), ZX_PROTOCOL_COMPOSITE, &composite);
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get composite protocol\n");
    return status;
  }

  zx_device_t* components[kComponentCount];
  size_t actual;
  composite_get_components(&composite, components, countof(components), &actual);
  if (actual != countof(components)) {
    zxlogf(ERROR, "optee: Unable to composite_get_components()\n");
    return ZX_ERR_INTERNAL;
  }

  status = device_get_protocol(components[kComponentPdev], ZX_PROTOCOL_PDEV, &pdev_proto_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get pdev protocol\n");
    return status;
  }

  status = device_get_protocol(components[kComponentSysmem], ZX_PROTOCOL_SYSMEM, &sysmem_proto_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get sysmem protocol\n");
    return status;
  }

  static constexpr uint32_t kTrustedOsSmcIndex = 0;
  status = pdev_get_smc(&pdev_proto_, kTrustedOsSmcIndex, secure_monitor_.reset_and_get_address());
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get secure monitor handle\n");
    return status;
  }

  // TODO(MTWN-140): Remove this once we have a tee core driver that will discover the TEE OS
  status = ValidateApiUid();
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: API UID does not match\n");
    return status;
  }

  status = ValidateApiRevision();
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: API revision not supported\n");
    return status;
  }

  status = GetOsRevision();
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Unable to get Trusted OS revision\n");
    return status;
  }

  status = ExchangeCapabilities();
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Could not exchange capabilities\n");
    return status;
  }

  status = InitializeSharedMemory();
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Could not initialize shared memory\n");
    return status;
  }

  status = DdkAdd("optee-tz", DEVICE_ADD_ALLOW_MULTI_COMPOSITE);
  if (status != ZX_OK) {
    zxlogf(ERROR, "optee: Failed to add device\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t OpteeController::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  fuchsia_hardware_tee::DeviceConnector::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

zx_status_t OpteeController::DdkOpen(zx_device_t** out_dev, uint32_t flags) {
  // Do not set out_dev because this Controller will handle the FIDL messages
  return ZX_OK;
}

void OpteeController::DdkUnbindNew(ddk::UnbindTxn txn) {
  // Initiate the removal of this device and all of its children.
  txn.Reply();
}

void OpteeController::DdkRelease() {
  // devmgr has given up ownership, so we must clean ourself up.
  delete this;
}

zx_status_t OpteeController::TeeConnect(zx::channel tee_device_request,
                                        zx::channel service_provider) {
  ZX_DEBUG_ASSERT(tee_device_request.is_valid());

  // Create a new OpteeClient device and hand off client communication to it.
  auto client = std::make_unique<OpteeClient>(this, std::move(service_provider));

  // Add child client device and have it immediately start serving device_request
  //
  // What we really want here is named parameter passing to pass client_remote
  zx_status_t status = client->DdkAdd("optee-client",               // name
                                      DEVICE_ADD_INSTANCE,          // flags
                                      nullptr,                      // props
                                      0,                            // prop_count
                                      0,                            // proto_id
                                      nullptr,                      // proxy_args
                                      tee_device_request.release()  // client_remote
  );
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the memory for the tee client
  __UNUSED OpteeClient* client_ptr = client.release();

  return ZX_OK;
}

void OpteeController::ConnectTee(
    zx::channel service_provider, zx::channel tee_request,
    fuchsia_hardware_tee::DeviceConnector::Interface::ConnectTeeCompleter::Sync completer) {
  TeeConnect(std::move(tee_request), std::move(service_provider));
}

OsInfo OpteeController::GetOsInfo() const {
  fuchsia_tee::Uuid uuid;
  uuid.time_low = kOpteeOsUuid.timeLow;
  uuid.time_mid = kOpteeOsUuid.timeMid;
  uuid.time_hi_and_version = kOpteeOsUuid.timeHiAndVersion;
  std::memcpy(uuid.clock_seq_and_node.data(), kOpteeOsUuid.clockSeqAndNode,
              sizeof(uuid.clock_seq_and_node));

  OsRevision os_revision;
  os_revision.set_major(os_revision_.major);
  os_revision.set_minor(os_revision_.minor);

  OsInfo os_info;
  os_info.set_uuid(uuid);
  os_info.set_revision(std::move(os_revision));
  os_info.set_is_global_platform_compliant(true);
  return os_info;
}

uint32_t OpteeController::CallWithMessage(const optee::Message& message, RpcHandler rpc_handler) {
  uint32_t return_value = tee_smc::kSmc32ReturnUnknownFunction;
  union {
    zx_smc_parameters_t params;
    RpcFunctionResult rpc_result;
  } func_call;
  func_call.params = tee_smc::CreateSmcFunctionCall(optee::kCallWithArgFuncId,
                                                    static_cast<uint32_t>(message.paddr() >> 32),
                                                    static_cast<uint32_t>(message.paddr()));

  while (true) {
    union {
      zx_smc_result_t raw;
      CallWithArgResult response;
      RpcFunctionArgs rpc_args;
    } result;

    zx_status_t status = zx_smc_call(secure_monitor_.get(), &func_call.params, &result.raw);
    if (status != ZX_OK) {
      zxlogf(ERROR, "optee: unable to invoke SMC\n");
      return return_value;
    }

    if (result.response.status == kReturnEThreadLimit) {
      // TODO(rjascani): This should actually block until a thread is available. For now,
      // just quit.
      zxlogf(ERROR, "optee: hit thread limit, need to fix this\n");
      break;
    } else if (optee::IsReturnRpc(result.response.status)) {
      rpc_handler(result.rpc_args, &func_call.rpc_result);
    } else {
      return_value = result.response.status;
      break;
    }
  }

  return return_value;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = OpteeController::Create;
  return ops;
}();

}  // namespace optee

// clang-format off
ZIRCON_DRIVER_BEGIN(optee, optee::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_OPTEE),
ZIRCON_DRIVER_END(optee)
    // clang-format on
