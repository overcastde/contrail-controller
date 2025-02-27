/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>

#include <cmn/agent_cmn.h>
#include <route/route.h>

#include <cmn/agent_cmn.h>
#include <oper/route_common.h>
#include <oper/vrf.h>
#include <oper/tunnel_nh.h>
#include <oper/mpls.h>
#include <oper/mirror_table.h>
#include <oper/peer.h>
#include <oper/agent_route.h>
#include <controller/controller_route_path.h>
#include <controller/controller_peer.h>
#include <controller/controller_init.h>
#include <controller/controller_export.h>
#include <oper/agent_sandesh.h>

using namespace std;
using namespace boost::asio;

/*
 * Common routine for all controller route data types
 * to verify peer validity.
 */
bool CheckPeerValidity(const AgentXmppChannel *channel,
                       uint64_t sequence_number) {
    //TODO reaching here as test case route add called with NULL peer
    if (sequence_number == ControllerPeerPath::kInvalidPeerIdentifier)
        return true;

    assert(channel);
    if (channel->bgp_peer_id() == NULL)
       return false;

    if (sequence_number == channel->unicast_sequence_number())
        return true;

    return false;
}

ControllerPeerPath::ControllerPeerPath(const Peer *peer) :
    AgentRouteData(false), peer_(peer) {
    if ((peer != NULL) && (peer->GetType() == Peer::BGP_PEER) ) {
        const BgpPeer *bgp_peer = static_cast<const BgpPeer *>(peer);
        channel_ = bgp_peer->GetBgpXmppPeerConst();
        sequence_number_ = channel_->unicast_sequence_number();
    } else {
        channel_ = NULL;
        sequence_number_ = kInvalidPeerIdentifier;
    }
}

bool ControllerEcmpRoute::IsPeerValid() const {
    return CheckPeerValidity(channel(), sequence_number());
}

bool ControllerEcmpRoute::AddChangePath(Agent *agent, AgentPath *path,
                                        const AgentRoute *rt) {
    CompositeNHKey *comp_key = static_cast<CompositeNHKey *>(nh_req_.key.get());
    //Reorder the component NH list, and add a reference to local composite mpls
    //label if any
    path->ReorderCompositeNH(agent, comp_key);
    return InetUnicastRouteEntry::ModifyEcmpPath(dest_addr_, plen_, vn_name_,
                                                 label_, local_ecmp_nh_,
                                                 vrf_name_, sg_list_,
                                                 path_preference_,
                                                 tunnel_bmap_,
                                                 nh_req_, agent, path);
}

ControllerVmRoute *ControllerVmRoute::MakeControllerVmRoute(const Peer *peer,
                                         const string &default_vrf,
                                         const Ip4Address &router_id,
                                         const string &vrf_name,
                                         const Ip4Address &tunnel_dest,
                                         TunnelType::TypeBmap bmap,
                                         uint32_t label,
                                         const string &dest_vn_name,
                                         const SecurityGroupList &sg_list,
                                         const PathPreference &path_preference) {
    // Make Tunnel-NH request
    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    nh_req.key.reset(new TunnelNHKey(default_vrf, router_id, tunnel_dest, false,
                                     TunnelType::ComputeType(bmap)));
    nh_req.data.reset(new TunnelNHData());

    // Make route request pointing to Tunnel-NH created above
    ControllerVmRoute *data =
        new ControllerVmRoute(peer, default_vrf, tunnel_dest, label, dest_vn_name,
                              bmap, sg_list, path_preference, nh_req);
    return data;
}

bool ControllerVmRoute::IsPeerValid() const {
    return CheckPeerValidity(channel(), sequence_number());
}

bool ControllerVmRoute::UpdateRoute(AgentRoute *rt) {
    bool ret = false;
    //For IP subnet routes with Tunnel NH, check if arp flood
    //needs to be enabled for the path.
    if ((rt->GetTableType() == Agent::INET4_UNICAST) ||
        (rt->GetTableType() == Agent::INET6_UNICAST)) {
        InetUnicastRouteEntry *inet_rt =
            static_cast<InetUnicastRouteEntry *>(rt);
        //If its the IPAM route then no change required neither
        //super net needs to be searched.
        if (inet_rt->ipam_subnet_route())
            return ret;

        bool ipam_subnet_route = inet_rt->IpamSubnetRouteAvailable();
        if (inet_rt->ipam_subnet_route() != ipam_subnet_route) {
            inet_rt->set_ipam_subnet_route(ipam_subnet_route);
            ret = true;
        }

        if (ipam_subnet_route) { 
            if (inet_rt->proxy_arp() == true) {
                inet_rt->set_proxy_arp(false);
                ret = true;
            }
        } else {
            if (inet_rt->proxy_arp() == false) {
                inet_rt->set_proxy_arp(true);
                ret = true;
            }
        }

    }
    return ret;
}

bool ControllerVmRoute::AddChangePath(Agent *agent, AgentPath *path,
                                      const AgentRoute *rt) {
    bool ret = false;
    NextHop *nh = NULL;
    SecurityGroupList path_sg_list;

    if (path->tunnel_bmap() != tunnel_bmap_) {
        path->set_tunnel_bmap(tunnel_bmap_);
        ret = true;
    }

    TunnelType::Type new_tunnel_type = TunnelType::ComputeType(tunnel_bmap_);
    if ((tunnel_bmap_ == (1 << TunnelType::VXLAN) && 
         (new_tunnel_type != TunnelType::VXLAN)) ||
        (tunnel_bmap_ != (1 << TunnelType::VXLAN) &&
         (new_tunnel_type == TunnelType::VXLAN))) {
        new_tunnel_type = TunnelType::INVALID;
        nh_req_.key.reset(new TunnelNHKey(agent->fabric_vrf_name(),
                                          agent->router_id(), tunnel_dest_,
                                          false, new_tunnel_type));
    }
    agent->nexthop_table()->Process(nh_req_);
    TunnelNHKey key(agent->fabric_vrf_name(), agent->router_id(), tunnel_dest_,
                    false, new_tunnel_type);
    nh = static_cast<NextHop *>(agent->nexthop_table()->FindActiveEntry(&key));
    path->set_tunnel_dest(tunnel_dest_);

    if (path->tunnel_type() != new_tunnel_type) {
        path->set_tunnel_type(new_tunnel_type);
        ret = true;
    }

    //Interpret label sent by control node
    if (tunnel_bmap_ == TunnelType::VxlanType()) {
        //Only VXLAN encap is sent, so label is VXLAN
        path->set_vxlan_id(label_);
        path->set_label(MplsTable::kInvalidLabel);
    } else if (tunnel_bmap_ == TunnelType::MplsType()) {
        //MPLS (GRE/UDP) is the only encap sent,
        //so label is MPLS.
        path->set_label(label_);
        path->set_vxlan_id(VxLanTable::kInvalidvxlan_id);
    } else {
        //Got a mix of Vxlan and Mpls, so interpret label
        //as per the computed tunnel type.
        if (new_tunnel_type == TunnelType::VXLAN) {
            if (path->vxlan_id() != label_) {
                path->set_vxlan_id(label_);
                path->set_label(MplsTable::kInvalidLabel);
                ret = true;
            }
        } else {
            if (path->label() != label_) {
                path->set_label(label_);
                path->set_vxlan_id(VxLanTable::kInvalidvxlan_id);
                ret = true;
            }
        }
    }

    if (path->dest_vn_name() != dest_vn_name_) {
        path->set_dest_vn_name(dest_vn_name_);
        ret = true;
    }

    path->set_unresolved(false);
    if (path->ChangeNH(agent, nh) == true)
        ret = true;

    path_sg_list = path->sg_list();
    if (path_sg_list != sg_list_) {
        path->set_sg_list(sg_list_);
        ret = true;
    }

    if (path->path_preference() != path_preference_) {
        path->set_path_preference(path_preference_);
        ret = true;
    }

    //If a transition of path happens from ECMP to non ECMP
    //reset local mpls label reference and composite nh key
    if (path->composite_nh_key()) {
        path->set_composite_nh_key(NULL);
        path->set_local_ecmp_mpls_label(NULL);
    }

    return ret;
}

ControllerLocalVmRoute::ControllerLocalVmRoute(const VmInterfaceKey &intf,
                                               uint32_t mpls_label,
                                               uint32_t vxlan_id,
                                               bool force_policy,
                                               const string &vn_name,
                                               uint8_t flags,
                                               const SecurityGroupList &sg_list,
                                               const PathPreference &path_preference,
                                               uint64_t sequence_number,
                                               const AgentXmppChannel *channel) :
    LocalVmRoute(intf, mpls_label, vxlan_id, force_policy, vn_name, flags, sg_list,
                 path_preference, Ip4Address(0)),
    sequence_number_(sequence_number), channel_(channel) { }

bool ControllerLocalVmRoute::IsPeerValid() const {
    return CheckPeerValidity(channel_, sequence_number_);
}

ControllerVlanNhRoute::ControllerVlanNhRoute(const VmInterfaceKey &intf,
                                             uint32_t tag,
                                             uint32_t label,
                                             const string &dest_vn_name,
                                             const SecurityGroupList &sg_list,
                                             const PathPreference &path_preference,
                                             uint64_t sequence_number,
                                             const AgentXmppChannel *channel) :
    VlanNhRoute(intf, tag, label, dest_vn_name, sg_list, path_preference),
    sequence_number_(sequence_number), channel_(channel) { }

bool ControllerVlanNhRoute::IsPeerValid() const {
    return CheckPeerValidity(channel_, sequence_number_);
}

ControllerInetInterfaceRoute::ControllerInetInterfaceRoute(const InetInterfaceKey &intf,
                                                           uint32_t label,
                                                           int tunnel_bmap,
                                                           const string &dest_vn_name,
                                                           uint64_t sequence_number,
                                                           const AgentXmppChannel *channel) :
    InetInterfaceRoute(intf, label, tunnel_bmap, dest_vn_name),
    sequence_number_(sequence_number), channel_(channel) { }

bool ControllerInetInterfaceRoute::IsPeerValid() const {
    return CheckPeerValidity(channel_, sequence_number_);
}

bool ClonedLocalPath::AddChangePath(Agent *agent, AgentPath *path,
                                    const AgentRoute *rt) {
    bool ret = false;

    MplsLabel *mpls = agent->mpls_table()->FindMplsLabel(mpls_label_);
    if (!mpls) {
        return ret;
    }

    //Do a route lookup in native VRF
    assert(mpls->nexthop()->GetType() == NextHop::VRF);
    const VrfNH *vrf_nh = static_cast<const VrfNH *>(mpls->nexthop());
    const InetUnicastRouteEntry *uc_rt =
        static_cast<const InetUnicastRouteEntry *>(rt);
    const AgentRoute *mpls_vrf_uc_rt =
        vrf_nh->GetVrf()->GetUcRoute(uc_rt->addr());
    if (mpls_vrf_uc_rt == NULL) {
        return ret;
    }
    AgentPath *local_path = mpls_vrf_uc_rt->FindLocalVmPortPath();
    if (!local_path) {
        return ret;
    }

    if (path->dest_vn_name() != vn_) {
        path->set_dest_vn_name(vn_);
        ret = true;
    }
    path->set_unresolved(false);

    if (path->sg_list() != sg_list_) {
        path->set_sg_list(sg_list_);
        ret = true;
    }

    path->set_tunnel_bmap(local_path->tunnel_bmap());
    TunnelType::Type new_tunnel_type =
        TunnelType::ComputeType(path->tunnel_bmap());
    if (new_tunnel_type == TunnelType::VXLAN &&
        local_path->vxlan_id() == VxLanTable::kInvalidvxlan_id) {
        new_tunnel_type = TunnelType::ComputeType(TunnelType::MplsType());
    }

    if (path->tunnel_type() != new_tunnel_type) {
        path->set_tunnel_type(new_tunnel_type);
        ret = true;
    }

    // If policy force-enabled in request, enable policy
    path->set_force_policy(local_path->force_policy());

    if (path->label() != local_path->label()) {
        path->set_label(local_path->label());
        ret = true;
    }

    if (path->vxlan_id() != local_path->vxlan_id()) {
        path->set_vxlan_id(local_path->vxlan_id());
        ret = true;
    }

    NextHop *nh = const_cast<NextHop *>(local_path->ComputeNextHop(agent));
    if (path->ChangeNH(agent, nh) == true) {
        ret = true;
    }
    return ret;
}

bool ClonedLocalPath::IsPeerValid() const {
    return CheckPeerValidity(channel_, sequence_number_);
}

ControllerMulticastRoute::ControllerMulticastRoute(const string &vn_name,
                                                   uint32_t label,
                                                   int vxlan_id,
                                                   uint32_t tunnel_type,
                                                   DBRequest &nh_req,
                                                   COMPOSITETYPE comp_nh_type,
                                                   uint64_t sequence_number,
                                                   const AgentXmppChannel *channel) :
    MulticastRoute(vn_name, label, vxlan_id, tunnel_type, nh_req, comp_nh_type),
    sequence_number_(sequence_number), channel_(channel) { }

bool ControllerMulticastRoute::IsPeerValid() const {
    return CheckPeerValidity(channel_, sequence_number_);
}
