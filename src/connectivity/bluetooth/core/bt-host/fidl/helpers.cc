// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "helpers.h"

#include <endian.h>

#include <unordered_set>

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/discovery_filter.h"
#include "src/lib/fxl/strings/split_string.h"
#include "src/lib/fxl/strings/string_number_conversions.h"

using fuchsia::bluetooth::Bool;
using fuchsia::bluetooth::Error;
using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Int8;
using fuchsia::bluetooth::Status;

namespace fble = fuchsia::bluetooth::le;
namespace fbt = fuchsia::bluetooth;
namespace fctrl = fuchsia::bluetooth::control;
namespace fhost = fuchsia::bluetooth::host;

namespace bthost {
namespace fidl_helpers {
namespace {

fctrl::TechnologyType TechnologyTypeToFidl(bt::gap::TechnologyType type) {
  switch (type) {
    case bt::gap::TechnologyType::kLowEnergy:
      return fctrl::TechnologyType::LOW_ENERGY;
    case bt::gap::TechnologyType::kClassic:
      return fctrl::TechnologyType::CLASSIC;
    case bt::gap::TechnologyType::kDualMode:
      return fctrl::TechnologyType::DUAL_MODE;
    default:
      ZX_PANIC("invalid technology type: %u", static_cast<unsigned int>(type));
      break;
  }

  // This should never execute.
  return fctrl::TechnologyType::DUAL_MODE;
}

bt::sm::SecurityProperties SecurityPropsFromFidl(const fctrl::SecurityProperties& sec_prop) {
  auto level = bt::sm::SecurityLevel::kEncrypted;
  if (sec_prop.authenticated) {
    level = bt::sm::SecurityLevel::kAuthenticated;
  }
  return bt::sm::SecurityProperties(level, sec_prop.encryption_key_size,
                                    sec_prop.secure_connections);
}

fctrl::SecurityProperties SecurityPropsToFidl(const bt::sm::SecurityProperties& sec_prop) {
  fctrl::SecurityProperties result;
  result.authenticated = sec_prop.authenticated();
  result.secure_connections = sec_prop.secure_connections();
  result.encryption_key_size = sec_prop.enc_key_size();
  return result;
}

bt::DeviceAddress::Type BondingAddrTypeFromFidl(const fctrl::AddressType& type) {
  switch (type) {
    case fctrl::AddressType::LE_RANDOM:
      return bt::DeviceAddress::Type::kLERandom;
    case fctrl::AddressType::LE_PUBLIC:
      return bt::DeviceAddress::Type::kLEPublic;
    case fctrl::AddressType::BREDR:
      return bt::DeviceAddress::Type::kBREDR;
    default:
      ZX_PANIC("invalid address type: %u", static_cast<unsigned int>(type));
      break;
  }
  return bt::DeviceAddress::Type::kBREDR;
}

fctrl::AddressType BondingAddrTypeToFidl(bt::DeviceAddress::Type type) {
  switch (type) {
    case bt::DeviceAddress::Type::kLERandom:
      return fctrl::AddressType::LE_RANDOM;
    case bt::DeviceAddress::Type::kLEPublic:
      return fctrl::AddressType::LE_PUBLIC;
    case bt::DeviceAddress::Type::kBREDR:
      return fctrl::AddressType::BREDR;
    default:
      // Anonymous is not a valid address type to use for bonding, so we treat
      // that as a programming error.
      ZX_PANIC("invalid address type for bonding: %u", static_cast<unsigned int>(type));
      break;
  }
  return fctrl::AddressType::BREDR;
}

bt::sm::LTK LtkFromFidl(const fctrl::LTK& ltk) {
  return bt::sm::LTK(SecurityPropsFromFidl(ltk.key.security_properties),
                     bt::hci::LinkKey(ltk.key.value, ltk.rand, ltk.ediv));
}

fctrl::LTK LtkToFidl(const bt::sm::LTK& ltk) {
  fctrl::LTK result;
  result.key.security_properties = SecurityPropsToFidl(ltk.security());
  result.key.value = ltk.key().value();

  // TODO(armansito): Remove this field since its already captured in security
  // properties.
  result.key_size = ltk.security().enc_key_size();
  result.rand = ltk.key().rand();
  result.ediv = ltk.key().ediv();
  return result;
}

bt::sm::Key KeyFromFidl(const fctrl::RemoteKey& key) {
  return bt::sm::Key(SecurityPropsFromFidl(key.security_properties), key.value);
}

fctrl::RemoteKey KeyToFidl(const bt::sm::Key& key) {
  fctrl::RemoteKey result;
  result.security_properties = SecurityPropsToFidl(key.security());
  result.value = key.value();
  return result;
}

}  // namespace

std::optional<bt::PeerId> PeerIdFromString(const std::string& id) {
  uint64_t value;
  if (!fxl::StringToNumberWithError<decltype(value)>(id, &value, fxl::Base::k16)) {
    return std::nullopt;
  }
  return bt::PeerId(value);
}

std::optional<bt::DeviceAddressBytes> AddressBytesFromString(const std::string& addr) {
  if (addr.size() != 17)
    return std::nullopt;

  auto split = fxl::SplitString(fxl::StringView(addr.data(), addr.size()), ":",
                                fxl::kKeepWhitespace, fxl::kSplitWantAll);
  if (split.size() != 6)
    return std::nullopt;

  std::array<uint8_t, 6> bytes;
  size_t index = 5;
  for (const auto& octet_str : split) {
    uint8_t octet;
    if (!fxl::StringToNumberWithError<uint8_t>(octet_str, &octet, fxl::Base::k16))
      return std::nullopt;
    bytes[index--] = octet;
  }

  return bt::DeviceAddressBytes(bytes);
}

ErrorCode HostErrorToFidl(bt::HostError host_error) {
  switch (host_error) {
    case bt::HostError::kFailed:
      return ErrorCode::FAILED;
    case bt::HostError::kTimedOut:
      return ErrorCode::TIMED_OUT;
    case bt::HostError::kInvalidParameters:
      return ErrorCode::INVALID_ARGUMENTS;
    case bt::HostError::kCanceled:
      return ErrorCode::CANCELED;
    case bt::HostError::kInProgress:
      return ErrorCode::IN_PROGRESS;
    case bt::HostError::kNotSupported:
      return ErrorCode::NOT_SUPPORTED;
    case bt::HostError::kNotFound:
      return ErrorCode::NOT_FOUND;
    case bt::HostError::kProtocolError:
      return ErrorCode::PROTOCOL_ERROR;
    default:
      break;
  }

  return ErrorCode::FAILED;
}

Status NewFidlError(ErrorCode error_code, std::string description) {
  Status status;
  status.error = Error::New();
  status.error->error_code = error_code;
  status.error->description = description;
  return status;
}

bt::UUID UuidFromFidl(const fuchsia::bluetooth::Uuid& input) {
  bt::UUID output;
  // Conversion must always succeed given the defined size of |input|.
  static_assert(sizeof(input.value) == 16, "FIDL UUID definition malformed!");
  bool status =
      bt::UUID::FromBytes(bt::BufferView(input.value.data(), input.value.size()), &output);
  ZX_ASSERT_MSG(status, "expected UUID conversion from FIDL to succeed!");
  return output;
}

bt::sm::IOCapability IoCapabilityFromFidl(fctrl::InputCapabilityType input,
                                          fctrl::OutputCapabilityType output) {
  if (input == fctrl::InputCapabilityType::NONE && output == fctrl::OutputCapabilityType::NONE) {
    return bt::sm::IOCapability::kNoInputNoOutput;
  } else if (input == fctrl::InputCapabilityType::KEYBOARD &&
             output == fctrl::OutputCapabilityType::DISPLAY) {
    return bt::sm::IOCapability::kKeyboardDisplay;
  } else if (input == fctrl::InputCapabilityType::KEYBOARD &&
             output == fctrl::OutputCapabilityType::NONE) {
    return bt::sm::IOCapability::kKeyboardOnly;
  } else if (input == fctrl::InputCapabilityType::NONE &&
             output == fctrl::OutputCapabilityType::DISPLAY) {
    return bt::sm::IOCapability::kDisplayOnly;
  } else if (input == fctrl::InputCapabilityType::CONFIRMATION &&
             output == fctrl::OutputCapabilityType::DISPLAY) {
    return bt::sm::IOCapability::kDisplayYesNo;
  }
  return bt::sm::IOCapability::kNoInputNoOutput;
}

bt::sm::PairingData PairingDataFromFidl(const fctrl::LEData& data) {
  bt::sm::PairingData result;

  auto addr = AddressBytesFromString(data.address);
  ZX_ASSERT(addr);
  result.identity_address = bt::DeviceAddress(BondingAddrTypeFromFidl(data.address_type), *addr);

  if (data.ltk) {
    result.ltk = LtkFromFidl(*data.ltk);
  }
  if (data.irk) {
    result.irk = KeyFromFidl(*data.irk);
  }
  if (data.csrk) {
    result.csrk = KeyFromFidl(*data.csrk);
  }
  return result;
}

bt::UInt128 LocalKeyFromFidl(const fctrl::LocalKey& key) { return key.value; }

std::optional<bt::sm::LTK> BrEdrKeyFromFidl(const fctrl::BREDRData& data) {
  if (data.link_key) {
    return LtkFromFidl(*data.link_key);
  }
  return std::nullopt;
}

fctrl::AdapterInfo NewAdapterInfo(const bt::gap::Adapter& adapter) {
  fctrl::AdapterInfo adapter_info;
  adapter_info.identifier = adapter.identifier().ToString();
  adapter_info.technology = TechnologyTypeToFidl(adapter.state().type());
  adapter_info.address = adapter.state().controller_address().ToString();

  adapter_info.state = fctrl::AdapterState::New();
  adapter_info.state->local_name = adapter.state().local_name();
  adapter_info.state->discoverable = Bool::New();
  adapter_info.state->discoverable->value = false;
  adapter_info.state->discovering = Bool::New();
  adapter_info.state->discovering->value = adapter.IsDiscovering();

  // TODO(armansito): Populate |local_service_uuids| as well.

  return adapter_info;
}

fctrl::RemoteDevice NewRemoteDevice(const bt::gap::Peer& peer) {
  fctrl::RemoteDevice fidl_device;
  fidl_device.identifier = peer.identifier().ToString();
  fidl_device.address = peer.address().value().ToString();
  fidl_device.technology = TechnologyTypeToFidl(peer.technology());
  fidl_device.connected = peer.connected();
  fidl_device.bonded = peer.bonded();

  // Set default value for device appearance.
  fidl_device.appearance = fctrl::Appearance::UNKNOWN;

  // |service_uuids| is not a nullable field, so we need to assign something
  // to it.
  fidl_device.service_uuids.resize(0);

  if (peer.rssi() != bt::hci::kRSSIInvalid) {
    fidl_device.rssi = Int8::New();
    fidl_device.rssi->value = peer.rssi();
  }

  if (peer.name()) {
    fidl_device.name = *peer.name();
  }

  if (peer.le()) {
    bt::gap::AdvertisingData adv_data;

    if (!bt::gap::AdvertisingData::FromBytes(peer.le()->advertising_data(), &adv_data)) {
      return fidl_device;
    }

    for (const auto& uuid : adv_data.service_uuids()) {
      fidl_device.service_uuids.push_back(uuid.ToString());
    }
    if (adv_data.appearance()) {
      fidl_device.appearance = static_cast<fctrl::Appearance>(le16toh(*adv_data.appearance()));
    }
    if (adv_data.tx_power()) {
      auto fidl_tx_power = Int8::New();
      fidl_tx_power->value = *adv_data.tx_power();
      fidl_device.tx_power = std::move(fidl_tx_power);
    }
  }

  return fidl_device;
}

fctrl::RemoteDevicePtr NewRemoteDevicePtr(const bt::gap::Peer& peer) {
  auto fidl_device = fctrl::RemoteDevice::New();
  *fidl_device = NewRemoteDevice(peer);
  return fidl_device;
}

fctrl::BondingData NewBondingData(const bt::gap::Adapter& adapter, const bt::gap::Peer& peer) {
  fctrl::BondingData out_data;
  out_data.identifier = peer.identifier().ToString();
  out_data.local_address = adapter.state().controller_address().ToString();

  if (peer.name()) {
    out_data.name = *peer.name();
  }

  // Store LE data.
  if (peer.le() && peer.le()->bond_data()) {
    out_data.le = fctrl::LEData::New();

    const auto& le_data = *peer.le()->bond_data();
    const auto& identity = le_data.identity_address ? *le_data.identity_address : peer.address();
    out_data.le->address = identity.value().ToString();
    out_data.le->address_type = BondingAddrTypeToFidl(identity.type());

    // TODO(armansito): Populate the preferred connection parameters here.

    // TODO(armansito): Populate with discovered GATT services. We initialize
    // this as empty as |services| is not nullable.
    out_data.le->services.resize(0);

    if (le_data.ltk) {
      out_data.le->ltk = fctrl::LTK::New();
      *out_data.le->ltk = LtkToFidl(*le_data.ltk);
    }
    if (le_data.irk) {
      out_data.le->irk = fctrl::RemoteKey::New();
      *out_data.le->irk = KeyToFidl(*le_data.irk);
    }
    if (le_data.csrk) {
      out_data.le->csrk = fctrl::RemoteKey::New();
      *out_data.le->csrk = KeyToFidl(*le_data.csrk);
    }
  }

  // Store BR/EDR data.
  if (peer.bredr() && peer.bredr()->link_key()) {
    out_data.bredr = fctrl::BREDRData::New();

    out_data.bredr->address = peer.bredr()->address().value().ToString();

    // TODO(BT-669): Populate with history of role switches.
    out_data.bredr->piconet_leader = false;

    // TODO(BT-670): Populate with discovered SDP services.
    out_data.bredr->services.resize(0);

    if (peer.bredr()->link_key()) {
      out_data.bredr->link_key = fctrl::LTK::New();
      *out_data.bredr->link_key = LtkToFidl(*peer.bredr()->link_key());
    }
  }

  return out_data;
}

fble::RemoteDevicePtr NewLERemoteDevice(const bt::gap::Peer& peer) {
  bt::gap::AdvertisingData ad;
  if (!peer.le()) {
    return nullptr;
  }

  const auto& le = *peer.le();
  auto fidl_device = fble::RemoteDevice::New();
  fidl_device->identifier = peer.identifier().ToString();
  fidl_device->connectable = peer.connectable();

  // Initialize advertising data only if its non-empty.
  if (le.advertising_data().size() != 0u) {
    bt::gap::AdvertisingData ad;
    if (!bt::gap::AdvertisingData::FromBytes(le.advertising_data(), &ad)) {
      return nullptr;
    }
    fidl_device->advertising_data = ad.AsLEAdvertisingData();
  }

  if (peer.rssi() != bt::hci::kRSSIInvalid) {
    fidl_device->rssi = Int8::New();
    fidl_device->rssi->value = peer.rssi();
  }

  return fidl_device;
}

bool IsScanFilterValid(const fble::ScanFilter& fidl_filter) {
  // |service_uuids| is the only field that can potentially contain invalid
  // data, since they are represented as strings.
  if (!fidl_filter.service_uuids)
    return true;

  for (const auto& uuid_str : *fidl_filter.service_uuids) {
    if (!bt::IsStringValidUuid(uuid_str))
      return false;
  }

  return true;
}

bool PopulateDiscoveryFilter(const fble::ScanFilter& fidl_filter,
                             bt::gap::DiscoveryFilter* out_filter) {
  ZX_DEBUG_ASSERT(out_filter);

  if (fidl_filter.service_uuids) {
    std::vector<bt::UUID> uuids;
    for (const auto& uuid_str : *fidl_filter.service_uuids) {
      bt::UUID uuid;
      if (!bt::StringToUuid(uuid_str, &uuid)) {
        bt_log(TRACE, "bt-host", "invalid parameters given to scan filter");
        return false;
      }
      uuids.push_back(uuid);
    }

    if (!uuids.empty())
      out_filter->set_service_uuids(uuids);
  }

  if (fidl_filter.connectable) {
    out_filter->set_connectable(fidl_filter.connectable->value);
  }

  if (fidl_filter.manufacturer_identifier) {
    out_filter->set_manufacturer_code(fidl_filter.manufacturer_identifier->value);
  }

  if (fidl_filter.name_substring && !fidl_filter.name_substring.value_or("").empty()) {
    out_filter->set_name_substring(fidl_filter.name_substring.value_or(""));
  }

  if (fidl_filter.max_path_loss) {
    out_filter->set_pathloss(fidl_filter.max_path_loss->value);
  }

  return true;
}

bt::gap::AdvertisingInterval AdvertisingIntervalFromFidl(fble::AdvertisingModeHint mode_hint) {
  switch (mode_hint) {
    case fble::AdvertisingModeHint::VERY_FAST:
      return bt::gap::AdvertisingInterval::FAST1;
    case fble::AdvertisingModeHint::FAST:
      return bt::gap::AdvertisingInterval::FAST2;
    case fble::AdvertisingModeHint::SLOW:
      return bt::gap::AdvertisingInterval::SLOW;
  }
  return bt::gap::AdvertisingInterval::SLOW;
}

bt::gap::AdvertisingData AdvertisingDataFromFidl(const fble::AdvertisingData& input) {
  bt::gap::AdvertisingData output;

  if (input.has_name()) {
    output.SetLocalName(input.name());
  }
  if (input.has_appearance()) {
    output.SetAppearance(static_cast<uint16_t>(input.appearance()));
  }
  if (input.has_tx_power_level()) {
    output.SetTxPower(input.tx_power_level());
  }
  if (input.has_service_uuids()) {
    for (const auto& uuid : input.service_uuids()) {
      output.AddServiceUuid(UuidFromFidl(uuid));
    }
  }
  if (input.has_service_data()) {
    for (const auto& entry : input.service_data()) {
      bt::UUID uuid = UuidFromFidl(entry.uuid);
      bt::BufferView data(entry.data);
      output.SetServiceData(uuid, data);
    }
  }
  if (input.has_manufacturer_data()) {
    for (const auto& entry : input.manufacturer_data()) {
      bt::BufferView data(entry.data);
      output.SetManufacturerData(entry.company_id, data);
    }
  }
  if (input.has_uris()) {
    for (const auto& uri : input.uris()) {
      output.AddURI(uri);
    }
  }

  return output;
}

fble::AdvertisingData AdvertisingDataToFidl(const bt::gap::AdvertisingData& input) {
  fble::AdvertisingData output;

  if (input.local_name()) {
    output.set_name(*input.local_name());
  }
  if (input.appearance()) {
    output.set_appearance(static_cast<fbt::Appearance>(*input.appearance()));
  }
  if (input.tx_power()) {
    output.set_tx_power_level(*input.tx_power());
  }
  if (!input.service_uuids().empty()) {
    std::vector<fbt::Uuid> uuids;
    for (const auto& uuid : input.service_uuids()) {
      uuids.push_back(fbt::Uuid{uuid.value()});
    }
    output.set_service_uuids(std::move(uuids));
  }
  if (!input.service_data_uuids().empty()) {
    std::vector<fble::ServiceData> entries;
    for (const auto& uuid : input.service_data_uuids()) {
      auto data = input.service_data(uuid);
      fble::ServiceData entry{fbt::Uuid{uuid.value()}, data.ToVector()};
      entries.push_back(std::move(entry));
    }
    output.set_service_data(std::move(entries));
  }
  if (!input.manufacturer_data_ids().empty()) {
    std::vector<fble::ManufacturerData> entries;
    for (const auto& id : input.manufacturer_data_ids()) {
      auto data = input.manufacturer_data(id);
      fble::ManufacturerData entry{id, data.ToVector()};
      entries.push_back(std::move(entry));
    }
    output.set_manufacturer_data(std::move(entries));
  }
  if (!input.uris().empty()) {
    std::vector<std::string> uris;
    for (const auto& uri : input.uris()) {
      uris.push_back(uri);
    }
    output.set_uris(std::move(uris));
  }

  return output;
}

fble::Peer PeerToFidlLe(const bt::gap::Peer& peer) {
  ZX_ASSERT(peer.le());

  fble::Peer output;
  output.set_id(fbt::PeerId{peer.identifier().value()});
  output.set_connectable(peer.connectable());

  if (peer.rssi() != bt::hci::kRSSIInvalid) {
    output.set_rssi(peer.rssi());
  }

  if (peer.le()->advertising_data().size() != 0u) {
    // We populate |output|'s AdvertisingData field if we can parse the payload. We leave it blank
    // otherwise.
    bt::gap::AdvertisingData unpacked;
    if (bt::gap::AdvertisingData::FromBytes(peer.le()->advertising_data(), &unpacked)) {
      output.set_advertising_data(fidl_helpers::AdvertisingDataToFidl(unpacked));
    }
  }

  return output;
}

}  // namespace fidl_helpers
}  // namespace bthost

// static
std::vector<uint8_t> fidl::TypeConverter<std::vector<uint8_t>, bt::ByteBuffer>::Convert(
    const bt::ByteBuffer& from) {
  std::vector<uint8_t> to(from.size());
  bt::MutableBufferView view(to.data(), to.size());
  view.Write(from);
  return to;
}
