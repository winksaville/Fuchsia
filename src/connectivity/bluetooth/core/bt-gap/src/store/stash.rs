// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::Error,
    fidl::endpoints::create_proxy,
    fidl_fuchsia_bluetooth_control::HostData,
    fidl_fuchsia_stash::{
        GetIteratorMarker, StoreAccessorMarker, StoreAccessorProxy, StoreMarker, Value,
    },
    fuchsia_bluetooth::{error::Error as BtError, inspect::Inspectable, types::BondingData},
    fuchsia_inspect,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    serde_json,
    std::collections::HashMap,
};

#[cfg(test)]
use {fidl::endpoints::Proxy, fuchsia_async as fasync, fuchsia_zircon as zx};

use crate::store::{
    keys::{
        bonding_data_key, host_data_key, host_id_from_key, BONDING_DATA_PREFIX, HOST_DATA_PREFIX,
    },
    serde::{
        BondingDataDeserializer, BondingDataSerializer, HostDataDeserializer, HostDataSerializer,
    },
};

/// Stash manages persistent data that is stored in bt-gap's component-specific storage. Data is
/// persisted in JSON format using the facilities provided by the serde library (see the
/// declarations in serde.rs for the description of the data format).
///
/// The stash currently stores the following types of data:
///
/// Bonding Data
/// ============
/// Data for all bonded peers are each stored as a unique entry. The key for each bonding data
/// entry has the following format:
///
///     "bonding-data:<device-id>"
///
/// where <device-id> is a unique device identifier generated by the bt-host that has a bond with
/// the peer. The structure of the key allows all bonding data to be fetched from the stash by
/// requesting the "bonding-data:" prefix. Individual entries can be fetched and stored by providing
/// the complete key.
///
/// Each bonding data entry contains the local bt-host identity address that it belongs to.
///
/// Host Data
/// =========
/// Data specific to a local bt-host identity are stored as a unique entry. The key for each host
/// data entry has the following format:
///
///     "host-data:<host-identity-address>"
///
/// where <host-identity-address> is a Bluetooth device address (e.g.
/// "host-data:01:02:03:04:05:06").
#[derive(Debug)]
pub struct Stash {
    /// The proxy to the Fuchsia stash service. This is assumed to have been initialized as a
    /// read/write capable accessor with the identity of the current component.
    proxy: StoreAccessorProxy,

    /// In-memory state of the bonding data stash. Each entry is hierarchically indexed by a
    /// local Bluetooth host identity and a peer device identifier.
    bonding_data: HashMap<String, HashMap<String, Inspectable<BondingData>>>,

    /// Persisted data for a particular local Bluetooth host, indexed by local Bluetooth host
    /// identity.
    // TODO(armansito): Introduce a concrete type for DeviceAddress instead of String.
    host_data: HashMap<String, HostData>,

    /// Handle to inspect data
    inspect: fuchsia_inspect::Node,
}

impl Stash {
    /// Updates the bonding data for a given device. Creates a new entry if one matching this
    /// device does not exist.
    pub fn store_bond(&mut self, data: BondingData) -> Result<(), Error> {
        let node = self.inspect.create_child(format!("bond {}", data.identifier));
        let data = Inspectable::new(data, node);
        fx_log_info!("store_bond (id: {})", data.identifier);

        // Persist the serialized blob.
        let serialized = serde_json::to_string(&BondingDataSerializer(&data.clone().into()))?;
        self.proxy
            .set_value(&bonding_data_key(&data.identifier), &mut Value::Stringval(serialized))?;
        self.proxy.commit()?;

        // Update the in memory cache.
        let local_map =
            self.bonding_data.entry(data.local_address.clone()).or_insert(HashMap::new());
        local_map.insert(data.identifier.clone(), data);
        Ok(())
    }

    /// Returns an iterator over the bonding data entries for the local adapter with the given
    /// `address`. Returns None if no such data exists.
    pub fn list_bonds(&self, local_address: &str) -> Option<impl Iterator<Item = &BondingData>> {
        Some(
            self.bonding_data
                .get(local_address)?
                .values()
                .into_iter()
                .map(|bd| -> &BondingData { &*bd }),
        )
    }

    /// Removes persisted bond for a peer and removes its information from any adapters that have
    /// it. Returns an error for failures but not if the peer isn't found.
    pub fn rm_peer(&mut self, peer_id: &str) -> Result<(), Error> {
        fx_log_info!("rm_peer (id: {})", peer_id);

        // Delete the persisted bond blob.
        self.proxy.delete_value(&bonding_data_key(&peer_id))?;
        self.proxy.commit()?;

        // Delete peer from memory cache of all adapters.
        self.bonding_data.values_mut().for_each(|m| m.retain(|k, _| k != peer_id));
        Ok(())
    }

    /// Returns the local host data for the given local `address`.
    pub fn get_host_data(&self, local_address: &str) -> Option<&HostData> {
        self.host_data.get(local_address)
    }

    /// Updates the host data for the host with the given identity address.
    pub fn store_host_data(&mut self, local_addr: &str, data: HostData) -> Result<(), Error> {
        fx_log_info!("store_host_data (local address: {})", local_addr);

        // Persist the serialized blob.
        let serialized = serde_json::to_string(&HostDataSerializer(&data))?;
        self.proxy.set_value(&host_data_key(local_addr), &mut Value::Stringval(serialized))?;
        self.proxy.commit()?;

        // Update the in memory cache.
        self.host_data.insert(local_addr.to_string(), data);
        Ok(())
    }

    // Initializes the stash using the given `accessor`. This asynchronously loads existing
    // stash data. Returns an error in case of failure.
    async fn new(
        accessor: StoreAccessorProxy,
        inspect: fuchsia_inspect::Node,
    ) -> Result<Stash, Error> {
        let bonding_data = Stash::load_bonds(&accessor, &inspect).await?;
        let host_data = Stash::load_host_data(&accessor).await?;
        Ok(Stash { proxy: accessor, bonding_data, host_data, inspect })
    }

    async fn load_bonds<'a>(
        accessor: &'a StoreAccessorProxy,
        inspect: &'a fuchsia_inspect::Node,
    ) -> Result<HashMap<String, HashMap<String, Inspectable<BondingData>>>, Error> {
        // Obtain a list iterator for all cached bonding data.
        let (iter, server_end) = create_proxy::<GetIteratorMarker>()?;
        accessor.get_prefix(BONDING_DATA_PREFIX, server_end)?;

        let mut bonding_map = HashMap::new();
        loop {
            let next = iter.get_next().await?;
            if next.is_empty() {
                break;
            }
            for key_value in next {
                if let Value::Stringval(json) = key_value.val {
                    let bonding_data: BondingDataDeserializer = serde_json::from_str(&json)?;
                    let bonding_data = BondingData::from(bonding_data.contents());
                    let node = inspect.create_child(format!("bond {}", bonding_data.identifier));
                    let bonding_data = Inspectable::new(bonding_data, node);
                    let local_address_entries = bonding_map
                        .entry(bonding_data.local_address.clone())
                        .or_insert(HashMap::new());
                    local_address_entries.insert(bonding_data.identifier.clone(), bonding_data);
                } else {
                    fx_log_err!("stash malformed: bonding data should be a string");
                    return Err(BtError::new("failed to initialize stash").into());
                }
            }
        }
        Ok(bonding_map)
    }

    async fn load_host_data(
        accessor: &StoreAccessorProxy,
    ) -> Result<HashMap<String, HostData>, Error> {
        // Obtain a list iterator for all cached host data.
        let (iter, server_end) = create_proxy::<GetIteratorMarker>()?;
        accessor.get_prefix(HOST_DATA_PREFIX, server_end)?;

        let mut host_data_map = HashMap::new();
        loop {
            let next = iter.get_next().await?;
            if next.is_empty() {
                break;
            }
            for key_value in next {
                let host_id = host_id_from_key(&key_value.key)?;
                if let Value::Stringval(json) = key_value.val {
                    let host_data: HostDataDeserializer = serde_json::from_str(&json)?;
                    let host_data = host_data.contents();
                    host_data_map.insert(host_id, host_data);
                } else {
                    fx_log_err!("stash malformed: host data should be a string");
                    return Err(BtError::new("failed to initialize stash").into());
                }
            }
        }
        Ok(host_data_map)
    }

    #[cfg(test)]
    pub fn stub() -> Result<Stash, Error> {
        let (proxy, _server) = zx::Channel::create()?;
        let proxy = fasync::Channel::from_channel(proxy)?;
        let proxy = StoreAccessorProxy::from_channel(proxy);
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("stub inspect");
        Ok(Stash { proxy, bonding_data: HashMap::new(), host_data: HashMap::new(), inspect })
    }
}

/// Connects to the stash service and initializes a Stash object. This function obtains
/// read/write capability to the component-specific storage identified by `component_id`.
pub async fn init_stash(
    component_id: &str,
    inspect: fuchsia_inspect::Node,
) -> Result<Stash, Error> {
    let stash_svc = fuchsia_component::client::connect_to_service::<StoreMarker>()?;
    stash_svc.identify(component_id)?;

    let (proxy, server_end) = create_proxy::<StoreAccessorMarker>()?;
    stash_svc.create_accessor(false, server_end)?;

    Stash::new(proxy, inspect).await
}

// These tests access stash in a hermetic envionment and thus it's ok for state to leak between
// test runs, regardless of test failure. Each test clears out the state in stash before performing
// its test logic.
#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_bluetooth_control::LocalKey;
    use {
        fuchsia_async as fasync, fuchsia_component::client::connect_to_service, pin_utils::pin_mut,
    };

    // create_stash_accessor will create a new accessor to stash scoped under the given test name.
    // All preexisting data in stash under this identity is deleted before the accessor is
    // returned.
    fn create_stash_accessor(test_name: &str) -> Result<StoreAccessorProxy, Error> {
        let stashserver = connect_to_service::<StoreMarker>()?;

        // Identify
        stashserver.identify(&(BONDING_DATA_PREFIX.to_owned() + test_name))?;

        // Create an accessor
        let (acc, server_end) = create_proxy()?;
        stashserver.create_accessor(false, server_end)?;

        // Clear all data in stash under our identity
        acc.delete_prefix("")?;
        acc.commit()?;

        Ok(acc)
    }

    #[test]
    fn new_stash_succeeds_with_empty_values() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");

        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");

        // Create a Stash service interface.
        let accessor_proxy = create_stash_accessor("new_stash_succeeds_with_empty_values")
            .expect("failed to create StashAccessor");
        let stash_new_future = Stash::new(accessor_proxy, inspect);
        pin_mut!(stash_new_future);

        // The stash should be initialized with no data.
        assert!(exec
            .run_singlethreaded(stash_new_future)
            .expect("expected Stash to initialize")
            .bonding_data
            .is_empty());
    }

    #[test]
    fn new_stash_fails_with_malformed_key_value_entry() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");

        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");

        // Create a Stash service interface.
        let accessor_proxy =
            create_stash_accessor("new_stash_fails_with_malformed_key_value_entry")
                .expect("failed to create StashAccessor");

        // Set a key/value that contains a non-string value.
        accessor_proxy
            .set_value("bonding-data:test1234", &mut Value::Intval(5))
            .expect("failed to set a bonding data value");
        accessor_proxy.commit().expect("failed to commit a bonding data value");

        // The stash should fail to initialize.
        let stash_new_future = Stash::new(accessor_proxy, inspect);
        assert!(exec.run_singlethreaded(stash_new_future).is_err());
    }

    #[test]
    fn new_stash_fails_with_malformed_json() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");

        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");

        // Create a mock Stash service interface.
        let accessor_proxy = create_stash_accessor("new_stash_fails_with_malformed_json")
            .expect("failed to create StashAccessor");

        // Set a vector that contains a malformed JSON value
        accessor_proxy
            .set_value("bonding-data:test1234", &mut Value::Stringval("{0}".to_string()))
            .expect("failed to set a bonding data value");
        accessor_proxy.commit().expect("failed to commit a bonding data value");

        // The stash should fail to initialize.
        let stash_new_future = Stash::new(accessor_proxy, inspect);
        assert!(exec.run_singlethreaded(stash_new_future).is_err());
    }

    #[test]
    fn new_stash_succeeds_with_values() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");

        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");

        // Create a Stash service interface.
        let accessor_proxy = create_stash_accessor("new_stash_succeeds_with_values")
            .expect("failed to create StashAccessor");

        // Insert values into stash that contain bonding data for several devices.
        accessor_proxy
            .set_value(
                "bonding-data:id-1",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-1",
                       "localAddress": "00:00:00:00:00:01",
                       "name": "Test Device 1",
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy
            .set_value(
                "bonding-data:id-2",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-2",
                       "localAddress": "00:00:00:00:00:01",
                       "name": "Test Device 2",
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy
            .set_value(
                "bonding-data:id-3",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-3",
                       "localAddress": "00:00:00:00:00:02",
                       "name": null,
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy.commit().expect("failed to commit bonding data values");

        // The stash should initialize with bonding data stored in stash
        let stash_new_future = Stash::new(accessor_proxy, inspect);
        let stash = exec.run_singlethreaded(stash_new_future).expect("stash failed to initialize");

        // There should be devices registered for two local addresses.
        assert_eq!(2, stash.bonding_data.len());

        // The first local address should have two devices associated with it.
        let local = stash
            .bonding_data
            .get("00:00:00:00:00:01")
            .expect("could not find local address entries");
        assert_eq!(2, local.len());
        let bond: &BondingData = &*local.get("id-1").expect("could not find device");
        assert_eq!(
            &BondingData {
                identifier: "id-1".to_string(),
                local_address: "00:00:00:00:00:01".to_string(),
                name: Some("Test Device 1".to_string()),
                le: None,
                bredr: None,
            },
            bond
        );
        let bond: &BondingData = &*local.get("id-2").expect("could not find device");
        assert_eq!(
            &BondingData {
                identifier: "id-2".to_string(),
                local_address: "00:00:00:00:00:01".to_string(),
                name: Some("Test Device 2".to_string()),
                le: None,
                bredr: None,
            },
            bond
        );

        // The second local address should have one device associated with it.
        let local = stash
            .bonding_data
            .get("00:00:00:00:00:02")
            .expect("could not find local address entries");
        assert_eq!(1, local.len());
        let bond: &BondingData = &*local.get("id-3").expect("could not find device");
        assert_eq!(
            &BondingData {
                identifier: "id-3".to_string(),
                local_address: "00:00:00:00:00:02".to_string(),
                name: None,
                le: None,
                bredr: None,
            },
            bond
        );
    }

    #[test]
    fn store_bond_commits_entry() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");
        let accessor_proxy = create_stash_accessor("store_bond_commits_entry")
            .expect("failed to create StashAccessor");
        let mut stash = exec
            .run_singlethreaded(Stash::new(accessor_proxy.clone(), inspect))
            .expect("stash failed to initialize");

        let bonding_data = BondingData {
            identifier: "id-1".to_string(),
            local_address: "00:00:00:00:00:01".to_string(),
            name: None,
            le: None,
            bredr: None,
        };
        assert!(stash.store_bond(bonding_data).is_ok());

        // Make sure that the in-memory cache has been updated.
        assert_eq!(1, stash.bonding_data.len());
        let bond: &BondingData =
            &*stash.bonding_data.get("00:00:00:00:00:01").unwrap().get("id-1").unwrap();
        assert_eq!(
            &BondingData {
                identifier: "id-1".to_string(),
                local_address: "00:00:00:00:00:01".to_string(),
                name: None,
                le: None,
                bredr: None,
            },
            bond
        );

        // The new data should be accessible over FIDL.
        assert_eq!(
            exec.run_singlethreaded(accessor_proxy.get_value("bonding-data:id-1"))
                .expect("failed to get value")
                .map(|x| *x),
            Some(Value::Stringval(
                "{\"identifier\":\"id-1\",\"localAddress\":\"00:00:00:00:00:01\",\"name\":null,\
                 \"le\":null,\"bredr\":null}"
                    .to_string()
            ))
        );
    }

    #[test]
    fn list_bonds() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");
        let accessor_proxy =
            create_stash_accessor("list_bonds").expect("failed to create StashAccessor");

        // Insert values into stash that contain bonding data for several devices.
        accessor_proxy
            .set_value(
                "bonding-data:id-1",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-1",
                       "localAddress": "00:00:00:00:00:01",
                       "name": null,
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy
            .set_value(
                "bonding-data:id-2",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-2",
                       "localAddress": "00:00:00:00:00:01",
                       "name": null,
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy.commit().expect("failed to initialize bonding data for testing");

        let stash = exec
            .run_singlethreaded(Stash::new(accessor_proxy, inspect))
            .expect("stash failed to initialize");

        // Should return None for unknown address.
        assert!(stash.list_bonds("00:00:00:00:00:00").is_none());

        let mut iter = stash.list_bonds("00:00:00:00:00:01").expect("expected to find address");
        let next_id = &iter.next().unwrap().identifier.clone();
        assert!("id-1" == next_id.as_str() || "id-2" == next_id.as_str());
        let next_id = &iter.next().unwrap().identifier.clone();
        assert!("id-1" == next_id.as_str() || "id-2" == next_id.as_str());
        assert_eq!(None, iter.next());
    }

    #[test]
    fn get_host_data() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");
        let accessor_proxy =
            create_stash_accessor("list_host_data").expect("failed to create StashAccessor");

        // Insert test data
        accessor_proxy
            .set_value(
                "host-data:00:00:00:00:00:01",
                &mut Value::Stringval(
                    r#"{
                        "irk": {
                            "value":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]
                        }
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy
            .set_value(
                "host-data:00:00:00:00:00:02",
                &mut Value::Stringval(
                    r#"{
                        "irk": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy.commit().expect("failed to initialize host data for testing");

        let stash = exec
            .run_singlethreaded(Stash::new(accessor_proxy, inspect))
            .expect("stash failed to initialize");

        // Should return None for unknown identity address.
        assert!(stash.get_host_data("00:00:00:00:00:00").is_none());

        let host_data =
            stash.get_host_data("00:00:00:00:00:01").expect("expected to find HostData");
        assert_eq!(
            &HostData {
                irk: Some(Box::new(LocalKey {
                    value: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
                })),
            },
            host_data
        );

        let host_data =
            stash.get_host_data("00:00:00:00:00:02").expect("expected to find HostData");
        assert_eq!(&HostData { irk: None }, host_data);
    }

    #[test]
    fn rm_peer() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");
        let accessor_proxy =
            create_stash_accessor("rm_peer").expect("failed to create StashAccessor");

        // Insert values into stash that contain bonding data for several devices.
        accessor_proxy
            .set_value(
                "bonding-data:id-1",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-1",
                       "localAddress": "00:00:00:00:00:01",
                       "name": null,
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy
            .set_value(
                "bonding-data:id-2",
                &mut Value::Stringval(
                    r#"
                    {
                       "identifier": "id-2",
                       "localAddress": "00:00:00:00:00:01",
                       "name": null,
                       "le": null,
                       "bredr": null
                    }"#
                    .to_string(),
                ),
            )
            .expect("failed to set value");
        accessor_proxy.commit().expect("failed to initialize bonding data for testing");

        let mut stash = exec
            .run_singlethreaded(Stash::new(accessor_proxy, inspect))
            .expect("stash failed to initialize");

        // OK to remove some unknown peer...
        assert!(stash.rm_peer("id-0").is_ok());

        // ...or known peer.
        assert!(stash.rm_peer("id-1").is_ok());

        let local = stash
            .bonding_data
            .get("00:00:00:00:00:01")
            .expect("could not find local address entries");
        assert_eq!(1, local.len());
        assert!(local.get("id-1").is_none());
        let bond: &BondingData = &*(local.get("id-2").expect("could not find device"));
        assert_eq!(
            &BondingData {
                identifier: "id-2".to_string(),
                local_address: "00:00:00:00:00:01".to_string(),
                name: None,
                le: None,
                bredr: None,
            },
            bond,
        );
    }

    #[test]
    fn store_host_data() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let inspect = fuchsia_inspect::Inspector::new().root().create_child("test");
        let accessor_proxy =
            create_stash_accessor("store_local_irk").expect("failed to create StashAccessor");
        let mut stash = exec
            .run_singlethreaded(Stash::new(accessor_proxy.clone(), inspect))
            .expect("stash failed to initialize");

        let host_data = HostData {
            irk: Some(Box::new(LocalKey {
                value: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
            })),
        };
        assert!(stash.store_host_data("00:00:00:00:00:01", host_data).is_ok());

        // Make sure the in-memory cache has been updated.
        assert_eq!(1, stash.host_data.len());
        assert_eq!(
            &HostData {
                irk: Some(Box::new(LocalKey {
                    value: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
                })),
            },
            stash.host_data.get("00:00:00:00:00:01").unwrap()
        );

        // The new data should be accessible over FIDL.
        assert_eq!(
            exec.run_singlethreaded(accessor_proxy.get_value("host-data:00:00:00:00:00:01"))
                .expect("failed to get value")
                .map(|x| *x),
            Some(Value::Stringval(
                "{\"irk\":{\"value\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]}}".to_string()
            ))
        );

        // It should be possible to overwrite the IRK.
        let host_data = HostData {
            irk: Some(Box::new(LocalKey {
                value: [16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1],
            })),
        };
        assert!(stash.store_host_data("00:00:00:00:00:01", host_data).is_ok());

        // Make sure the in-memory cache has been updated.
        assert_eq!(1, stash.host_data.len());
        assert_eq!(
            &HostData {
                irk: Some(Box::new(LocalKey {
                    value: [16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1]
                })),
            },
            stash.host_data.get("00:00:00:00:00:01").unwrap()
        );

        // The new data should be accessible over FIDL.
        assert_eq!(
            exec.run_singlethreaded(accessor_proxy.get_value("host-data:00:00:00:00:00:01"))
                .expect("failed to get value")
                .map(|x| *x),
            Some(Value::Stringval(
                "{\"irk\":{\"value\":[16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1]}}".to_string()
            ))
        );
    }
}
