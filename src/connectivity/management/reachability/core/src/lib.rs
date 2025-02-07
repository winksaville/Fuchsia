// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#[macro_use]
extern crate log;
use failure::Error;
use fidl_fuchsia_net_stack as stack;
use fidl_fuchsia_netstack as netstack;
use network_manager_core::error;
use network_manager_core::hal;
use network_manager_core::lifmgr::{subnet_mask_to_prefix_length, to_ip_addr, LifIpAddr};
use std::collections::HashMap;

/// `Stats` keeps the monitoring service statistic counters.
#[derive(Debug, Default, Clone, Copy)]
pub struct Stats {
    /// `events` is the number of events received.
    pub events: u64,
    // TODO(dpradilla): consider keeping this stat per interface or even per network.
    /// `state_updates` is the number of times reachability state has changed.
    pub state_updates: u64,
}

// TODO(dpradilla): consider splitting the state in l2 state and l3 state, as there can be multiple
// L3 networks on the same physical medium.
/// `State` represents the reachability state.
#[derive(Debug, PartialEq, Clone, Copy)]
pub enum State {
    /// Interface no longer present.
    Removed,
    /// Interface is down.
    Down,
    /// Interface is up, no packets seen yet.
    Up,
    /// Interface is up, packets seen.
    LinkLayerUp,
    /// Interface is up, and configured as an L3 interface.
    NetworkLayerUp,
    /// L3 Interface is up, local neighbors seen.
    Local,
    /// L3 Interface is up, local gateway configured and reachable.
    Gateway,
    /// Expected response not seen from reachability test URL.
    WalledGarden,
    /// Expected response seen from reachability test URL.
    Internet,
}

/// `PortType` is the type of port backing the L3 interface.
#[derive(Debug, PartialEq)]
pub enum PortType {
    /// EthernetII or 802.3.
    Ethernet,
    /// Wireless LAN based on 802.11.
    WiFi,
    /// Switch virtual interface.
    SVI,
    /// Loopback.
    Loopback,
}

/// `NetworkInfo` keeps information about an network.
#[derive(Debug, PartialEq)]
struct NetworkInfo {
    /// `is_default` indicates the default route is via this network.
    is_default: bool,
    /// `is_l3` indicates L3 is configured.
    is_l3: bool,
    /// `state` is the current reachability state.
    state: State,
}

/// `ReachabilityInfo` is the information about an interface.
#[derive(Debug, PartialEq)]
pub struct ReachabilityInfo {
    /// `port_type` is the type of port.
    port_type: PortType,
    /// IPv4 reachability information.
    v4: NetworkInfo,
    /// IPv6 reachability information.
    v6: NetworkInfo,
}

type Id = hal::PortId;
type StateInfo = HashMap<Id, ReachabilityInfo>;

/// `Monitor` monitors the reachability state.
pub struct Monitor {
    hal: hal::NetCfg,
    state_info: StateInfo,
    stats: Stats,
}

#[derive(Debug)]
enum Event {
    None,
    Stack(fidl_fuchsia_net_stack::InterfaceStatusChange),
    NetStack(fidl_fuchsia_netstack::NetInterface),
}

impl Monitor {
    /// Create the monitoring service.
    pub fn new() -> Result<Self, Error> {
        let hal = hal::NetCfg::new()?;
        Ok(Monitor { hal, state_info: HashMap::new(), stats: Default::default() })
    }

    /// `stats` returns monitoring service statistic counters.
    pub fn stats(&self) -> &Stats {
        &self.stats
    }
    /// `state` returns reachability state for all interfaces known to the monitoring service.
    pub fn state(&self) -> &StateInfo {
        &self.state_info
    }

    fn dump_state(&self) {
        for (key, value) in &self.state_info {
            debug!("{:?}: {:?}", key, value);
        }
    }

    fn report(&self, id: Id, info: &ReachabilityInfo) {
        warn!("State Change {:?}: {:?}", id, info);
    }

    /// Returns the underlying event streams associated with the open channels to fuchsia.net.stack
    /// and fuchsia.netstack.
    pub fn take_event_streams(
        &mut self,
    ) -> (stack::StackEventStream, netstack::NetstackEventStream) {
        self.hal.take_event_streams()
    }

    /// `update_states` processes an event and updates the reachability state accordingly.
    async fn update_state(&mut self, event: Event, interface_info: &hal::Interface) {
        let port_type = port_type(interface_info);
        if port_type == PortType::Loopback {
            return;
        }

        debug!("update_state ->  event: {:?}, interface_info: {:?}", event, interface_info);
        let routes = self.hal.routes().await;
        if let Some(new_info) = compute_state(&event, interface_info, routes) {
            if let Some(info) = self.state_info.get(&interface_info.id) {
                if info == &new_info {
                    // State has not changed, nothing to do.
                    debug!("update_state ->  no change");
                    return;
                }
            }

            self.report(interface_info.id, &new_info);
            self.stats.state_updates += 1;
            debug!("update_state ->  new state {:?}", new_info);
            self.state_info.insert(interface_info.id, new_info);
        };
    }

    /// Processes an event coming from fuchsia.net.stack containing updates to
    /// properties associated with an interface. `OnInterfaceStatusChange` event is raised when an
    /// interface is enabled/disabled, connected/disconnected, or added/removed.
    pub async fn stack_event(&mut self, event: stack::StackEvent) -> error::Result<()> {
        self.stats.events += 1;
        match event {
            stack::StackEvent::OnInterfaceStatusChange { info } => {
                // This event is not really hooked up (stack does not generate them), code here
                // just for completeness and to be ready then it gets hooked up.
                if let Some(current_info) = self.hal.get_interface(info.id).await {
                    self.update_state(Event::Stack(info), &current_info).await;
                }
                Ok(())
            }
        }
    }

    /// Processes an event coming from fuchsia.netstack containing updates to
    /// properties associated with an interface.
    pub async fn netstack_event(&mut self, event: netstack::NetstackEvent) -> error::Result<()> {
        self.stats.events += 1;
        match event {
            netstack::NetstackEvent::OnInterfacesChanged { interfaces } => {
                // This type of event is useful to know that there has been some change related
                // to the network state, but doesn't give an indication about what that event
                // was. We need to check all interfaces to find out if there has been a state
                // change.
                for i in interfaces {
                    if let Some(current_info) = self.hal.get_interface(u64::from(i.id)).await {
                        self.update_state(Event::NetStack(i), &current_info).await;
                    }
                }
            }
        }
        self.dump_state();
        Ok(())
    }

    /// `populate_state` queries the networks stack to determine current state.
    pub async fn populate_state(&mut self) -> error::Result<()> {
        for info in self.hal.interfaces().await?.iter() {
            self.update_state(Event::None, info).await;
        }
        self.dump_state();
        Ok(())
    }
}

/// `compute_state` processes an event and computes the reachability based on the event and
/// system observations.
fn compute_state(
    event: &Event,
    interface_info: &hal::Interface,
    routes: Option<Vec<hal::Route>>,
) -> Option<ReachabilityInfo> {
    let port_type = port_type(interface_info);
    if port_type == PortType::Loopback {
        return None;
    }

    let i = match event {
        Event::NetStack(i) => i,
        _ => {
            info!("unsupported event type {:?}", event);
            return None;
        }
    };

    let ipv4_address = ipv4_to_cidr(i.addr, i.netmask);

    let mut new_info = ReachabilityInfo {
        port_type,
        v4: NetworkInfo {
            is_default: false,
            is_l3: (i.flags & netstack::NET_INTERFACE_FLAG_DHCP) != 0 || ipv4_address.is_some(),
            state: State::Down,
        },
        v6: NetworkInfo { is_default: false, is_l3: !i.ipv6addrs.is_empty(), state: State::Down },
    };

    let is_up = (i.flags & netstack::NET_INTERFACE_FLAG_UP) != 0;
    if !is_up {
        return Some(new_info);
    }

    new_info.v4.state = State::Up;
    new_info.v6.state = State::Up;

    // packet reception is network layer independent.
    if !packet_count_increases(interface_info.id) {
        // TODO(dpradilla): add active probing here.
        // No packets seen, but interface is up.
        return Some(new_info);
    }

    new_info.v4.state = State::LinkLayerUp;
    new_info.v6.state = State::LinkLayerUp;

    new_info.v4.state = network_layer_state(ipv4_address.into_iter(), &routes, &new_info.v4);

    // TODO(dpradilla): Add support for IPV6

    Some(new_info)
}

fn ipv4_to_cidr(
    addr: fidl_fuchsia_net::IpAddress,
    netmask: fidl_fuchsia_net::IpAddress,
) -> Option<LifIpAddr> {
    let ipv4_address = to_ip_addr(addr);
    if to_ip_addr(addr).is_unspecified() {
        None
    } else {
        Some(LifIpAddr { address: ipv4_address, prefix: subnet_mask_to_prefix_length(netmask) })
    }
}

// `local_routes` traverses `route_table` to find routes that use a gateway local to `address`
// network.
fn local_routes<'a>(address: &LifIpAddr, route_table: &'a [hal::Route]) -> Vec<&'a hal::Route> {
    let local_routes: Vec<&hal::Route> = route_table
        .iter()
        .filter(|r| match r.gateway {
            Some(gateway) => address.is_in_same_subnet(&gateway),
            None => false,
        })
        .collect();
    local_routes
}

// TODO(dpradilla): implement.
// `has_local_neighbors` checks for local neighbors.
fn has_local_neighbors() -> bool {
    true
}

// TODO(dpradilla): implement.
// `packet_count_increases` verifies packet counts are going up.
fn packet_count_increases(_: hal::PortId) -> bool {
    true
}

fn port_type(interface_info: &hal::Interface) -> PortType {
    if interface_info.name.contains("wlan") {
        PortType::WiFi
    } else if interface_info.name.contains("ethernet") {
        PortType::Ethernet
    } else if interface_info.name.contains("loopback") {
        PortType::Loopback
    } else {
        PortType::SVI
    }
}

// `network_layer_state` determines the L3 reachability state.
fn network_layer_state<'a>(
    mut addresses: impl Iterator<Item = LifIpAddr>,
    routes: &Option<Vec<hal::Route>>,
    info: &NetworkInfo,
) -> State {
    // This interface is not configured for L3, Nothing to check.
    if !info.is_l3 {
        return info.state;
    }

    if info.state != State::LinkLayerUp || !has_local_neighbors() {
        return info.state;
    }

    // TODO(dpradilla): add support for multiple addresses.
    let address = addresses.next();
    if address.is_none() {
        return info.state;
    }

    let mut new_state = State::Local;

    let route_table = match routes {
        Some(r) => r,
        _ => return new_state,
    };

    // Has local gateway.
    let gw = local_routes(&address.unwrap(), &route_table);
    if gw.is_empty() {
        return new_state;
    }

    // TODO(dpradilla): verify local gateways are reachable
    new_state = State::Gateway;

    // TODO(dpradilla) Check for internet connectivity and set new_state =State::Internet on
    // success.

    new_state
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_net_ext::IpAddress;

    #[test]
    fn test_has_local_neighbors() {
        assert_eq!(has_local_neighbors(), true);
    }

    #[test]
    fn test_packet_count_increases() {
        assert_eq!(packet_count_increases(hal::PortId::from(1)), true);
    }

    #[test]
    fn test_port_type() {
        assert_eq!(
            port_type(&hal::Interface {
                addr: None,
                enabled: true,
                name: "loopback".to_string(),
                id: hal::PortId::from(1),
                dhcp_client_enabled: false,
            }),
            PortType::Loopback
        );
        assert_eq!(
            port_type(&hal::Interface {
                addr: None,
                enabled: true,
                name: "ethernet/eth0".to_string(),
                id: hal::PortId::from(1),
                dhcp_client_enabled: false,
            }),
            PortType::Ethernet
        );
        assert_eq!(
            port_type(&hal::Interface {
                addr: None,
                enabled: true,
                name: "ethernet/wlan".to_string(),
                id: hal::PortId::from(1),
                dhcp_client_enabled: false,
            }),
            PortType::WiFi
        );
        assert_eq!(
            port_type(&hal::Interface {
                addr: None,
                enabled: true,
                name: "br0".to_string(),
                id: hal::PortId::from(1),
                dhcp_client_enabled: false,
            }),
            PortType::SVI
        );
    }

    #[test]
    fn test_local_routes() {
        let address = &LifIpAddr { address: "1.2.3.4".parse().unwrap(), prefix: 24 };
        let route_table = &vec![
            hal::Route {
                gateway: Some("1.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "1.2.3.0".parse().unwrap(), prefix: 24 },
            },
        ];

        let want_route = &hal::Route {
            gateway: Some("1.2.3.1".parse().unwrap()),
            metric: None,
            port_id: Some(hal::PortId::from(1)),
            target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
        };

        let want = vec![want_route];
        let got = local_routes(address, route_table);
        assert_eq!(got, want, "route via local network found.");

        let address = &LifIpAddr { address: "2.2.3.4".parse().unwrap(), prefix: 24 };
        let route_table = &vec![
            hal::Route {
                gateway: Some("1.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "1.2.3.0".parse().unwrap(), prefix: 24 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "2.2.3.0".parse().unwrap(), prefix: 24 },
            },
        ];

        let want = Vec::<&hal::Route>::new();
        let got = local_routes(address, route_table);
        assert_eq!(got, want, "route via local network not present.");
    }

    #[test]
    fn test_network_layer_state() {
        let address = Some(LifIpAddr { address: "1.2.3.4".parse().unwrap(), prefix: 24 });
        let route_table = vec![
            hal::Route {
                gateway: Some("1.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "1.2.3.0".parse().unwrap(), prefix: 24 },
            },
        ];
        let route_table_2 = vec![
            hal::Route {
                gateway: Some("2.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "1.2.3.0".parse().unwrap(), prefix: 24 },
            },
            hal::Route {
                gateway: None,
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "2.2.3.0".parse().unwrap(), prefix: 24 },
            },
        ];

        assert_eq!(
            network_layer_state(
                address.into_iter(),
                &Some(route_table),
                &NetworkInfo { is_default: false, is_l3: true, state: State::LinkLayerUp },
            ),
            State::Gateway,
            "All is good"
        );

        assert_eq!(
            network_layer_state(
                address.into_iter(),
                &None,
                &NetworkInfo { is_default: false, is_l3: true, state: State::LinkLayerUp }
            ),
            State::Local,
            "No routes"
        );

        assert_eq!(
            network_layer_state(
                None.into_iter(),
                &Some(route_table_2),
                &NetworkInfo { is_default: false, is_l3: true, state: State::NetworkLayerUp }
            ),
            State::NetworkLayerUp,
            "default route is not local"
        );

        // TODO(dpradilla): Add tests that veryfy the rest of the states can be reached when the
        // right conditions are present. To be done when that functionality is implemented.
    }

    #[test]
    fn test_compute_state() {
        let got = compute_state(
            &Event::None,
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ifname".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            None,
        );
        assert_eq!(got, None, "not and ethernet interface");

        let got = compute_state(
            &Event::None,
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            None,
        );
        assert_eq!(got, None, "ethernet interface, but not a valid event");

        let got = compute_state(
            &Event::NetStack(fidl_fuchsia_netstack::NetInterface {
                id: 1,
                flags: 0,
                features: 0,
                configuration: 0,
                name: "eth0".to_string(),
                addr: IpAddress("1.2.3.4".parse().unwrap()).into(),
                netmask: IpAddress("255.255.255.0".parse().unwrap()).into(),
                broadaddr: IpAddress("1.2.3.255".parse().unwrap()).into(),
                ipv6addrs: vec![],
                hwaddr: vec![0, 0, 0, 0, 0, 0],
            }),
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            None,
        );
        let want = Some(ReachabilityInfo {
            port_type: PortType::Ethernet,
            v4: NetworkInfo { is_default: false, is_l3: true, state: State::Down },
            v6: NetworkInfo { is_default: false, is_l3: false, state: State::Down },
        });
        assert_eq!(got, want, "ethernet interface, ipv4 configured, interface down");

        let got = compute_state(
            &Event::NetStack(fidl_fuchsia_netstack::NetInterface {
                id: 1,
                flags: netstack::NET_INTERFACE_FLAG_UP,
                features: 0,
                configuration: 0,
                name: "eth0".to_string(),
                addr: IpAddress("1.2.3.4".parse().unwrap()).into(),
                netmask: IpAddress("255.255.255.0".parse().unwrap()).into(),
                broadaddr: IpAddress("1.2.3.255".parse().unwrap()).into(),
                ipv6addrs: vec![],
                hwaddr: vec![0, 0, 0, 0, 0, 0],
            }),
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            None,
        );
        let want = Some(ReachabilityInfo {
            port_type: PortType::Ethernet,
            v4: NetworkInfo { is_default: false, is_l3: true, state: State::Local },
            v6: NetworkInfo { is_default: false, is_l3: false, state: State::LinkLayerUp },
        });
        assert_eq!(got, want, "ethernet interface, ipv4 configured, interface up");

        let got = compute_state(
            &Event::NetStack(fidl_fuchsia_netstack::NetInterface {
                id: 1,
                flags: netstack::NET_INTERFACE_FLAG_UP,
                features: 0,
                configuration: 0,
                name: "eth0".to_string(),
                addr: IpAddress("1.2.3.4".parse().unwrap()).into(),
                netmask: IpAddress("255.255.255.0".parse().unwrap()).into(),
                broadaddr: IpAddress("1.2.3.255".parse().unwrap()).into(),
                ipv6addrs: vec![],
                hwaddr: vec![0, 0, 0, 0, 0, 0],
            }),
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            Some(vec![hal::Route {
                gateway: Some("2.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            }]),
        );
        let want = Some(ReachabilityInfo {
            port_type: PortType::Ethernet,
            v4: NetworkInfo { is_default: false, is_l3: true, state: State::Local },
            v6: NetworkInfo { is_default: false, is_l3: false, state: State::LinkLayerUp },
        });
        assert_eq!(
            got, want,
            "ethernet interface, ipv4 configured, interface up, no local gateway"
        );

        let got = compute_state(
            &Event::NetStack(fidl_fuchsia_netstack::NetInterface {
                id: 1,
                flags: netstack::NET_INTERFACE_FLAG_UP,
                features: 0,
                configuration: 0,
                name: "eth0".to_string(),
                addr: IpAddress("1.2.3.4".parse().unwrap()).into(),
                netmask: IpAddress("255.255.255.0".parse().unwrap()).into(),
                broadaddr: IpAddress("1.2.3.255".parse().unwrap()).into(),
                ipv6addrs: vec![],
                hwaddr: vec![0, 0, 0, 0, 0, 0],
            }),
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            Some(vec![hal::Route {
                gateway: Some("1.2.3.1".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "0.0.0.0".parse().unwrap(), prefix: 0 },
            }]),
        );
        let want = Some(ReachabilityInfo {
            port_type: PortType::Ethernet,
            v4: NetworkInfo { is_default: false, is_l3: true, state: State::Gateway },
            v6: NetworkInfo { is_default: false, is_l3: false, state: State::LinkLayerUp },
        });
        assert_eq!(
            got, want,
            "ethernet interface, ipv4 configured, interface up, with local gateway"
        );

        let got = compute_state(
            &Event::NetStack(fidl_fuchsia_netstack::NetInterface {
                id: 1,
                flags: netstack::NET_INTERFACE_FLAG_UP,
                features: 0,
                configuration: 0,
                name: "eth0".to_string(),
                addr: IpAddress("1.2.3.4".parse().unwrap()).into(),
                netmask: IpAddress("255.255.255.0".parse().unwrap()).into(),
                broadaddr: IpAddress("1.2.3.255".parse().unwrap()).into(),
                ipv6addrs: vec![],
                hwaddr: vec![0, 0, 0, 0, 0, 0],
            }),
            &hal::Interface {
                id: hal::PortId::from(1),
                name: "ethernet/eth0".to_string(),
                addr: None,
                enabled: false,
                dhcp_client_enabled: false,
            },
            Some(vec![hal::Route {
                gateway: Some("fe80::2aad:3fe0:7436:5677".parse().unwrap()),
                metric: None,
                port_id: Some(hal::PortId::from(1)),
                target: LifIpAddr { address: "::".parse().unwrap(), prefix: 0 },
            }]),
        );
        let want = Some(ReachabilityInfo {
            port_type: PortType::Ethernet,
            v4: NetworkInfo { is_default: false, is_l3: true, state: State::Local },
            v6: NetworkInfo { is_default: false, is_l3: false, state: State::LinkLayerUp },
        });
        assert_eq!(
            got, want,
            "ethernet interface, ipv4 configured, interface up, no local gateway"
        );

        // TODO(dpradilla): Add test cases to cover functionality that is not yet implemented.
    }
}
