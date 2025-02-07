// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <lib/zircon-internal/thread_annotations.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/types.h>

#include <ddk/device.h>

#if __cplusplus

#include <memory>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/mutex.h>

namespace tpm {

typedef uint8_t Locality;

// Abstraction over the hardware access mechanism.  The communication protocol
// relies on accessing certain hardware registers which have the same contents
// regardless of access mechanism.
class HardwareInterface {
 public:
  virtual ~HardwareInterface() {}

  // Return ZX_OK if the device represented by this interface is valid under
  // the interface's constraints.  This may perform IO to determine the
  // answer, and will be called before the device is made visible to the rest
  // of the system.
  virtual zx_status_t Validate() { return ZX_OK; }

  // Read/write the ACCESS register for the given locality
  virtual zx_status_t ReadAccess(Locality loc, uint8_t* access) = 0;
  virtual zx_status_t WriteAccess(Locality loc, uint8_t access) = 0;

  // Read/write the STS register for the given locality
  virtual zx_status_t ReadStatus(Locality loc, uint32_t* sts) = 0;
  virtual zx_status_t WriteStatus(Locality loc, uint32_t sts) = 0;

  // Read the DID_VID register, if present
  virtual zx_status_t ReadDidVid(uint16_t* vid, uint16_t* did) = 0;

  // Read/write from the DATA_FIFO register.  It is up to the caller to respect the
  // protocol's burstCount.
  virtual zx_status_t ReadDataFifo(Locality loc, uint8_t* buf, size_t len) = 0;
  virtual zx_status_t WriteDataFifo(Locality loc, const uint8_t* buf, size_t len) = 0;
};

class Device;
using DeviceType = ddk::Device<Device, ddk::UnbindableNew, ddk::Suspendable>;

class Device : public DeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_TPM> {
 public:
  Device(zx_device_t* parent, std::unique_ptr<HardwareInterface> iface);
  ~Device();

  static zx_status_t Create(void* ctx, zx_device_t* parent, std::unique_ptr<Device>* out);
  static zx_status_t CreateAndBind(void* ctx, zx_device_t* parent);
  static bool RunUnitTests(void* ctx, zx_device_t* parent, zx_handle_t channel);

  // Send the given command packet to the TPM and wait for a response.
  // |actual| is the number of bytes written into |resp|.
  zx_status_t ExecuteCmd(Locality loc, const uint8_t* cmd, size_t len, uint8_t* resp,
                         size_t max_len, size_t* actual);

  // Execute the GetRandom TPM command.
  zx_status_t GetRandom(void* buf, uint16_t count, size_t* actual);

  // DDK methods
  void DdkRelease();
  void DdkUnbindNew(ddk::UnbindTxn txn);
  zx_status_t DdkSuspend(uint32_t flags);

  zx_status_t Init();

 private:
  // Register this instance with devmgr and launch the deferred
  // initialization.
  zx_status_t Bind();

  // Deferred initialization of the device via a thread.  Once complete, this
  // marks the device as visible.
  static zx_status_t InitThread(void* device) {
    return reinterpret_cast<Device*>(device)->InitThread();
  }
  zx_status_t InitThread();

  // Send the given command packet to the TPM and wait for a response.
  // |actual| is the number of bytes written into |resp|.
  zx_status_t ExecuteCmdLocked(Locality loc, const uint8_t* cmd, size_t len, uint8_t* resp,
                               size_t max_len, size_t* actual) TA_REQ(lock_);

  // Request use of the given locality
  zx_status_t RequestLocalityLocked(Locality loc) TA_REQ(lock_);
  // Wait for access to the requested locality
  zx_status_t WaitForLocalityLocked(Locality loc) TA_REQ(lock_);
  // Release the given locality
  zx_status_t ReleaseLocalityLocked(Locality loc) TA_REQ(lock_);

  // Perform the transmit half of a command
  zx_status_t SendCmdLocked(Locality loc, const uint8_t* cmd, size_t len) TA_REQ(lock_);
  // Perform the receive half of a command.  |actual| will contain the total number of bytes in
  // the response, may be less than max_len.
  zx_status_t RecvRespLocked(Locality loc, uint8_t* resp, size_t max_len, size_t* actual)
      TA_REQ(lock_);

  // Issue a TPM_CC_SHUTDOWN with the given type
  zx_status_t ShutdownLocked(uint16_t type) TA_REQ(lock_);

  fbl::Mutex lock_;
  const std::unique_ptr<HardwareInterface> iface_;
};

enum tpm_result {
  TPM_SUCCESS = 0x0,
  TPM_BAD_PARAMETER = 0x3,
  TPM_DEACTIVATED = 0x6,
  TPM_DISABLED = 0x7,
  TPM_DISABLED_CMD = 0x8,
  TPM_FAIL = 0x9,
  TPM_BAD_ORDINAL = 0xa,
  TPM_RETRY = 0x800,
};

}  // namespace tpm

#endif  // __cplusplus
