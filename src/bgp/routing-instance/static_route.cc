/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/static_route.h"

#include <boost/assign/list_of.hpp>
#include <boost/bind.hpp>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/request_pipeline.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/queue_task.h"
#include "base/task.h"
#include "base/task_annotations.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_path.h"
#include "bgp/bgp_peer.h"
#include "bgp/bgp_route.h"
#include "bgp/bgp_sandesh.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_table.h"
#include "bgp/l3vpn/inetvpn_route.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/routing-instance/static_route_types.h"
#include "db/db_table_partition.h"
#include "net/address.h"

using boost::assign::list_of;
using boost::system::error_code;
using std::make_pair;
using std::set;
using std::string;
using std::vector;

class StaticRouteState : public ConditionMatchState {
public:
    explicit StaticRouteState(StaticRoutePtr info) : info_(info) {
    }
    StaticRoutePtr info() {
        return info_;
    }

private:
    StaticRoutePtr info_;
    DISALLOW_COPY_AND_ASSIGN(StaticRouteState);
};

class StaticRoute : public ConditionMatch {
public:
    // List of Route targets
    typedef set<RouteTarget> RouteTargetList;

    // List of path ids for the Nexthop
    typedef set<uint32_t> NexthopPathIdList;

    enum CompareResult {
        NoChange = 0,
        PrefixChange = -1,
        NexthopChange = 1,
        RTargetChange = 2
    };

    StaticRoute(RoutingInstance *rtinstance, const Ip4Prefix &static_route,
                const vector<string> &rtargets, IpAddress nexthop);

    // Compare config and return whether cfg has updated
    CompareResult CompareStaticRouteCfg(const StaticRouteConfig &cfg);

    const Ip4Prefix &static_route_prefix() const {
        return static_route_prefix_;
    }

    RoutingInstance *routing_instance() const {
        return routing_instance_;
    }

    BgpTable *bgp_table() const {
        return routing_instance_->GetTable(Address::INET);
    }

    const IpAddress &nexthop() const {
        return nexthop_;
    }

    BgpRoute *nexthop_route() const {
        return nexthop_route_;
    }

    NexthopPathIdList *NexthopPathIds() { return &nexthop_path_ids_; }

    void set_nexthop_route(BgpRoute *nexthop_route) {
        nexthop_route_ = nexthop_route;

        if (!nexthop_route_) {
            nexthop_path_ids_.clear();
            return;
        }

        assert(nexthop_path_ids_.empty());

        for (Route::PathList::iterator it =
             nexthop_route->GetPathList().begin();
             it != nexthop_route->GetPathList().end(); it++) {
            BgpPath *path = static_cast<BgpPath *>(it.operator->());

            // Infeasible paths are not considered
            if (!path->IsFeasible()) break;

            // take snapshot of all ECMP paths
            if (nexthop_route_->BestPath()->PathCompare(*path, true)) break;

            // Use the nexthop attribute of the nexthop path as the path id.
            uint32_t path_id = path->GetAttr()->nexthop().to_v4().to_ulong();
            nexthop_path_ids_.insert(path_id);
        }
    }

    const RouteTargetList &rtarget_list() const {
        return rtarget_list_;
    }

    void UpdateRtargetList(const vector<string> &rtargets) {
        rtarget_list_.clear();
        for (vector<string>::const_iterator it = rtargets.begin();
             it != rtargets.end(); ++it) {
            error_code ec;
            RouteTarget rtarget = RouteTarget::FromString(*it, &ec);
            if (ec != 0)
                continue;
            rtarget_list_.insert(rtarget);
        }

        // Update static route if added already
        UpdateStaticRoute();
    }

    void AddStaticRoute(NexthopPathIdList *list);
    void UpdateStaticRoute();
    void RemoveStaticRoute();
    void NotifyRoute();

    virtual bool Match(BgpServer *server, BgpTable *table,
                       BgpRoute *route, bool deleted);

    virtual string ToString() const {
        return (string("StaticRoute ") + nexthop_.to_string());
    }

    void set_unregistered() {
        unregistered_ = true;
    }

    bool unregistered() const {
        return unregistered_;
    }

private:
    // Helper function to match
    bool is_nexthop_route(BgpRoute *route) {
        InetRoute *inet_route = dynamic_cast<InetRoute *>(route);
        if (nexthop() == inet_route->GetPrefix().ip4_addr())
            return true;
        return false;
    }

    ExtCommunityPtr ExtCommunityRouteTargetList(const BgpAttr *attr) const {
        ExtCommunity::ExtCommunityList export_list;
        for (RouteTargetList::const_iterator it = rtarget_list().begin();
             it != rtarget_list().end(); it++) {
            export_list.push_back(it->GetExtCommunity());
        }

        ExtCommunityPtr new_ext_community(NULL);
        ExtCommunityDB *comm_db = routing_instance()->server()->extcomm_db();
        if (!export_list.empty()) {
            new_ext_community =
                comm_db->ReplaceRTargetAndLocate(attr->ext_community(),
                                                 export_list);
        }

        return new_ext_community;
    }

    RoutingInstance *routing_instance_;
    Ip4Prefix static_route_prefix_;
    IpAddress nexthop_;
    BgpRoute *nexthop_route_;
    NexthopPathIdList nexthop_path_ids_;
    RouteTargetList rtarget_list_;
    bool unregistered_;

    DISALLOW_COPY_AND_ASSIGN(StaticRoute);
};


StaticRoute::StaticRoute(RoutingInstance *rtinst, const Ip4Prefix &static_route,
    const vector<string> &rtargets, IpAddress nexthop)
    : routing_instance_(rtinst),
      static_route_prefix_(static_route),
      nexthop_(nexthop),
      nexthop_route_(NULL),
      unregistered_(false) {
    for (vector<string>::const_iterator it = rtargets.begin();
        it != rtargets.end(); it++) {
        error_code ec;
        RouteTarget rtarget = RouteTarget::FromString(*it, &ec);
        if (ec != 0)
            continue;
        rtarget_list_.insert(rtarget);
    }
}

// Compare config and return whether cfg has updated
StaticRoute::CompareResult 
StaticRoute::CompareStaticRouteCfg(const StaticRouteConfig &cfg) {
    Ip4Prefix prefix(cfg.address.to_v4(), cfg.prefix_length);
    if (static_route_prefix_ != prefix) {
        return PrefixChange;
    }
    if (nexthop_ != cfg.nexthop) {
        return NexthopChange;
    }
    if (rtarget_list_.size() != cfg.route_target.size()) {
        return RTargetChange;
    }
    for (vector<string>::const_iterator it = cfg.route_target.begin();
         it != cfg.route_target.end(); it++) {
        error_code ec;
        RouteTarget rtarget = RouteTarget::FromString(*it, &ec);
        if (rtarget_list_.find(rtarget) == rtarget_list_.end()) {
            return RTargetChange;
        }
    }
    return NoChange;
}

// Match function called from BgpConditionListener
// Concurrency : db::DBTable
bool 
StaticRoute::Match(BgpServer *server, BgpTable *table, 
                   BgpRoute *route, bool deleted) {
    CHECK_CONCURRENCY("db::DBTable");
    StaticRouteRequest::RequestType type;

    if (is_nexthop_route(route) && !unregistered()) {
        if (deleted)
            type = StaticRouteRequest::NEXTHOP_DELETE;
        else
            type = StaticRouteRequest::NEXTHOP_ADD_CHG;
    } else {
        return false;
    }

    BgpConditionListener *listener = server->condition_listener(Address::INET);
    StaticRouteState *state = static_cast<StaticRouteState *>
        (listener->GetMatchState(table, route, this));
    if (!deleted) {
        // MatchState is added to the Route to ensure that DBEntry is not
        // deleted before the module processes the WorkQueue request.
        if (!state) {
            state = new StaticRouteState(StaticRoutePtr(this));
            listener->SetMatchState(table, route, this, state);
        }
    } else {
        // MatchState is set on all the Routes that matches the conditions
        // Retrieve to check and ignore delete of unseen Add Match
        if (state == NULL) {
            // Not seen ADD ignore DELETE
            return false;
        }
    }

    // The MatchState reference is taken to ensure that the route is not
    // deleted when request is still present in the queue
    // This is to handle the case where MatchState already exists and
    // deleted entry gets reused or reused entry gets deleted.
    state->IncrementRefCnt();

    // Post the Match result to StaticRoute processing task to take Action
    // Nexthop route found in NAT instance ==> Add Static Route
    // and stitch the Path Attribute from nexthop route
    StaticRouteRequest *req =
        new StaticRouteRequest(type, table, route, StaticRoutePtr(this));
    routing_instance()->static_route_mgr()->EnqueueStaticRouteReq(req);

    return true;
}

// RemoveStaticRoute
void StaticRoute::RemoveStaticRoute() {
    CHECK_CONCURRENCY("bgp::StaticRoute");
    InetRoute rt_key(static_route_prefix());
    DBTablePartition *partition =
       static_cast<DBTablePartition *>(bgp_table()->GetTablePartition(&rt_key));
    BgpRoute *static_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));
    if (!static_route || static_route->IsDeleted()) return;

    for (NexthopPathIdList::iterator it = NexthopPathIds()->begin();
         it != NexthopPathIds()->end(); it++) {
        static_route->RemovePath(BgpPath::StaticRoute, NULL, *it);
        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Removed Static route path " << static_route->ToString() <<
            " path_id " << BgpPath::PathIdString(*it) <<
            " in table " << bgp_table()->name());
    }

    if (!static_route->BestPath()) {
        partition->Delete(static_route);
    } else {
        partition->Notify(static_route);
    }
}

// UpdateStaticRoute
void
StaticRoute::UpdateStaticRoute() {
    CHECK_CONCURRENCY("bgp::Config");
    InetRoute rt_key(static_route_prefix());
    DBTablePartition *partition =
       static_cast<DBTablePartition *>(bgp_table()->GetTablePartition(&rt_key));
    BgpRoute *static_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));
    if (static_route == NULL) return;

    static_route->ClearDelete();

    BgpAttrDB *attr_db = routing_instance()->server()->attr_db();
    for (NexthopPathIdList::iterator it = NexthopPathIds()->begin();
         it != NexthopPathIds()->end(); it++) {
        BgpPath *existing_path =
            static_route->FindPath(BgpPath::StaticRoute, NULL, *it);
        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Update the RTarget list of Static route path "
            << static_route->ToString() << " path_id "
            << BgpPath::PathIdString(*it) << " in table "
            << bgp_table()->name());

        ExtCommunityPtr ptr =
            ExtCommunityRouteTargetList(existing_path->GetAttr());
        // Add the route target in the ExtCommunity attribute
        BgpAttrPtr new_attr = attr_db->ReplaceExtCommunityAndLocate(
            existing_path->GetAttr(), ptr);

        BgpPath *new_path =
            new BgpPath(*it, BgpPath::StaticRoute, new_attr.get(),
                        existing_path->GetFlags(), existing_path->GetLabel());

        static_route->RemovePath(BgpPath::StaticRoute, NULL, *it);

        static_route->InsertPath(new_path);
    }
    partition->Notify(static_route);
}

// AddStaticRoute
void
StaticRoute::AddStaticRoute(NexthopPathIdList *old_path_ids) {
    CHECK_CONCURRENCY("bgp::StaticRoute");

    InetRoute rt_key(static_route_prefix());
    DBTablePartition *partition =
       static_cast<DBTablePartition *>(bgp_table()->GetTablePartition(&rt_key));
    BgpRoute *static_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));

    if (static_route == NULL) {
        static_route = new InetRoute(static_route_prefix());
        partition->Add(static_route);
    } else {
        static_route->ClearDelete();
    }

    BgpAttrDB *attr_db = routing_instance()->server()->attr_db();
    for (Route::PathList::iterator it = nexthop_route()->GetPathList().begin();
         it != nexthop_route()->GetPathList().end(); it++) {
        BgpPath *nexthop_route_path = static_cast<BgpPath *>(it.operator->());

        // Infeasible paths are not considered
        if (!nexthop_route_path->IsFeasible()) break;

        // take snapshot of all ECMP paths
        if (nexthop_route()->BestPath()->PathCompare(*nexthop_route_path, true))
            break;

        // Skip paths with duplicate forwarding information.  This ensures
        // that we generate only one path with any given next hop and label
        // when there are multiple nexthop paths from the original source
        // received via different peers e.g. directly via XMPP and via BGP.
        if (nexthop_route()->DuplicateForwardingPath(nexthop_route_path))
            continue;

        // Add the route target in the ExtCommunity attribute
        ExtCommunityPtr ptr =
            ExtCommunityRouteTargetList(nexthop_route_path->GetAttr());
        BgpAttrPtr new_attr = attr_db->ReplaceExtCommunityAndLocate(
            nexthop_route_path->GetAttr(), ptr);

        // Replace the source rd if the nexthop path is a secondary path
        // of a primary path in the l3vpn table. Use the RD of the primary.
        if (nexthop_route_path->IsReplicated()) {
            const BgpSecondaryPath *spath =
                static_cast<const BgpSecondaryPath *>(nexthop_route_path);
            const RoutingInstance *ri = spath->src_table()->routing_instance();
            if (ri->IsDefaultRoutingInstance()) {
                const InetVpnRoute *vpn_route =
                    static_cast<const InetVpnRoute *>(spath->src_rt());
                new_attr = attr_db->ReplaceSourceRdAndLocate(new_attr.get(),
                    vpn_route->GetPrefix().route_distinguisher());
            }
        }

        // Check whether we already have a path with the associated path id.
        uint32_t path_id =
            nexthop_route_path->GetAttr()->nexthop().to_v4().to_ulong();
        BgpPath *existing_path = static_route->FindPath(BgpPath::StaticRoute,
                                                        NULL, path_id);
        bool is_stale = false;
        if (existing_path != NULL) {
            if ((new_attr.get() != existing_path->GetAttr()) ||
                (nexthop_route_path->GetLabel() != existing_path->GetLabel())) {
                // Update Attributes and notify (if needed)
                is_stale = existing_path->IsStale();
                static_route->RemovePath(BgpPath::StaticRoute, NULL, path_id);
            } else {
                continue;
            }
        }

        BgpPath *new_path =
            new BgpPath(path_id, BgpPath::StaticRoute, new_attr.get(),
                nexthop_route_path->GetFlags(), nexthop_route_path->GetLabel());
        if (is_stale)
            new_path->SetStale();

        static_route->InsertPath(new_path);
        partition->Notify(static_route);

        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Added Static Route path " << static_route->ToString() <<
            " path_id " << BgpPath::PathIdString(path_id) <<
            " in table " << bgp_table()->name());
    }

    if (!old_path_ids) return;

    for (NexthopPathIdList::iterator it = old_path_ids->begin();
         it != old_path_ids->end(); it++) {
        if (NexthopPathIds()->find(*it) != NexthopPathIds()->end())
            continue;
        static_route->RemovePath(BgpPath::StaticRoute, NULL, *it);
        partition->Notify(static_route);

        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Removed StaticRoute path " << static_route->ToString() <<
            " path_id " << BgpPath::PathIdString(*it) <<
            " in table " << bgp_table()->name());
    }
}

void StaticRoute::NotifyRoute() {
    InetRoute rt_key(static_route_prefix());
    DBTablePartition *partition =
       static_cast<DBTablePartition *>(bgp_table()->GetTablePartition(&rt_key));
    BgpRoute *static_route = static_cast<BgpRoute *>(partition->Find(&rt_key));
    if (!static_route)
        return;
    partition->Notify(static_route);
}

int StaticRouteMgr::static_route_task_id_ = -1;

StaticRouteMgr::StaticRouteMgr(RoutingInstance *instance)
    : instance_(instance),
      listener_(instance_->server()->condition_listener(Address::INET)),
      resolve_trigger_(new TaskTrigger(
          boost::bind(&StaticRouteMgr::ResolvePendingStaticRouteConfig, this),
          TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0)) {
    if (static_route_task_id_ == -1) {
        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        static_route_task_id_ = scheduler->GetTaskId("bgp::StaticRoute");
    }

    static_route_queue_ = new WorkQueue<StaticRouteRequest *>
        (static_route_task_id_, routing_instance()->index(),
         boost::bind(&StaticRouteMgr::StaticRouteEventCallback, this, _1));
}

void StaticRouteMgr::EnqueueStaticRouteReq(StaticRouteRequest *req) {
    static_route_queue_->Enqueue(req);
}

bool StaticRouteMgr::StaticRouteEventCallback(StaticRouteRequest *req) {
    CHECK_CONCURRENCY("bgp::StaticRoute");
    BgpTable *table = req->table_;
    BgpRoute *route = req->rt_;
    StaticRoute *info = static_cast<StaticRoute *>(req->info_.get());

    StaticRouteState *state = NULL;
    if (route) {
        state = static_cast<StaticRouteState *>
            (listener_->GetMatchState(table, route, info));
    }

    switch (req->type_) {
        case StaticRouteRequest::NEXTHOP_ADD_CHG: {
            assert(state);
            state->reset_deleted();
            if (route->IsDeleted() || !route->BestPath() ||
                !route->BestPath()->IsFeasible())  {
                break;
            }

            // Store the old path list
            StaticRoute::NexthopPathIdList path_ids;
            path_ids.swap(*(info->NexthopPathIds()));

            // Populate the Nexthop PathID
            info->set_nexthop_route(route);

            info->AddStaticRoute(&path_ids);
            break;
        }
        case StaticRouteRequest::NEXTHOP_DELETE: {
            assert(state);
            info->RemoveStaticRoute();
            if (info->deleted() || route->IsDeleted()) {
                state->set_deleted();
            }
            info->set_nexthop_route(NULL);
            break;
        }
        case StaticRouteRequest::DELETE_STATIC_ROUTE_DONE: {
            info->set_unregistered();
            if (!info->num_matchstate()) {
                listener_->UnregisterCondition(table, info);
                static_route_map_.erase(info->static_route_prefix());
                if (static_route_map_.empty())
                    instance_->server()->RemoveStaticRouteMgr(this);
                if (!routing_instance()->deleted() && 
                    routing_instance()->config()) {
                    resolve_trigger_->Set();
                }
            }
            break;
        }
        default: {
            assert(0);
            break;
        }
    }

    if (state) {
        state->DecrementRefCnt();
        if (state->refcnt() == 0 && state->deleted()) {
            listener_->RemoveMatchState(table, route, info);
            delete state;
            if (!info->num_matchstate() && info->unregistered()) {
                listener_->UnregisterCondition(table, info);
                static_route_map_.erase(info->static_route_prefix());
                if (static_route_map_.empty())
                    instance_->server()->RemoveStaticRouteMgr(this);
                if (!routing_instance()->deleted() && 
                    routing_instance()->config()) {
                    resolve_trigger_->Set();
                }
            }
        }
    }

    delete req;
    return true;
}

void 
StaticRouteMgr::LocateStaticRoutePrefix(const StaticRouteConfig &cfg) {
    CHECK_CONCURRENCY("bgp::Config");
    Ip4Prefix prefix(cfg.address.to_v4(), cfg.prefix_length);

    // Verify whether the entry already exists
    StaticRouteMap::iterator it = static_route_map_.find(prefix);
    if (it != static_route_map_.end()) {
        // Wait for the delete complete cb
        if (it->second->deleted()) return;

        StaticRoute *match =
            static_cast<StaticRoute *>(it->second.get());
        // Check whether the config has got updated
        StaticRoute::CompareResult change = match->CompareStaticRouteCfg(cfg);

        // StaticRoutePrefix is the key,, it can't change
        assert(change != StaticRoute::PrefixChange);

        // No change..
        if (change == StaticRoute::NoChange) return;

        if (change == StaticRoute::RTargetChange) {
            // Update the route target in ExtCommunity attribute if the
            // route is already added
            match->UpdateRtargetList(cfg.route_target);
            return;
        }

        // If the nexthop changes, remove the static route, if already added.
        // To do this, remove the match condition and wait for remove completion
        BgpConditionListener::RequestDoneCb callback =
            boost::bind(&StaticRouteMgr::StopStaticRouteDone, this,
                        _1, _2);

        listener_->RemoveMatchCondition(match->bgp_table(), it->second.get(),
                                       callback);
        return;
    }

    StaticRoute *match =  
        new StaticRoute(routing_instance(), prefix, cfg.route_target,
                        cfg.nexthop);
    StaticRoutePtr static_route_match = StaticRoutePtr(match);

    if (static_route_map_.empty())
        instance_->server()->InsertStaticRouteMgr(this);
    static_route_map_.insert(make_pair(prefix, static_route_match));

    listener_->AddMatchCondition(match->bgp_table(), static_route_match.get(),
                                BgpConditionListener::RequestDoneCb());
}

void StaticRouteMgr::StopStaticRouteDone(BgpTable *table,
                                             ConditionMatch *info) {
    // Post the RequestDone event to StaticRoute task to take Action
    StaticRouteRequest *req =
        new StaticRouteRequest(StaticRouteRequest::DELETE_STATIC_ROUTE_DONE,
                           table, NULL, StaticRoutePtr(info));

    EnqueueStaticRouteReq(req);
    return;
}

void StaticRouteMgr::RemoveStaticRoutePrefix(const Ip4Prefix &static_route) {
    CHECK_CONCURRENCY("bgp::Config");
    StaticRouteMap::iterator it = static_route_map_.find(static_route);
    if (it == static_route_map_.end()) return;

    if (it->second->deleted()) return;

    BgpConditionListener::RequestDoneCb callback =
        boost::bind(&StaticRouteMgr::StopStaticRouteDone, this, _1, _2);

    StaticRoute *match = static_cast<StaticRoute *>(it->second.get());
    listener_->RemoveMatchCondition(match->bgp_table(), match, callback);
}

void StaticRouteMgr::ProcessStaticRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    const BgpInstanceConfig::StaticRouteList &list =
        routing_instance()->config()->static_routes();
    typedef BgpInstanceConfig::StaticRouteList::const_iterator iterator_t;
    for (iterator_t iter = list.begin(); iter != list.end(); ++iter) {
        LocateStaticRoutePrefix(*iter);
    }
}

bool StaticRouteMgr::ResolvePendingStaticRouteConfig() {
    ProcessStaticRouteConfig();
    return true;
}

StaticRouteMgr::~StaticRouteMgr() {
    if (static_route_queue_)
        delete static_route_queue_;
}

bool CompareStaticRouteConfig(const StaticRouteConfig &lhs,
                              const StaticRouteConfig &rhs) {
    Ip4Prefix lhs_static_route_prefix(lhs.address.to_v4(), lhs.prefix_length);
    Ip4Prefix rhs_static_route_prefix(rhs.address.to_v4(), rhs.prefix_length);
    return (lhs_static_route_prefix < rhs_static_route_prefix); 
}


void StaticRouteMgr::UpdateStaticRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    typedef BgpInstanceConfig::StaticRouteList StaticRouteList;
    StaticRouteList static_route_list =
        routing_instance()->config()->static_routes();

    sort(static_route_list.begin(), static_route_list.end(),
              CompareStaticRouteConfig);

    StaticRouteList::const_iterator static_route_cfg_it = 
            static_route_list.begin();
    StaticRouteMap::iterator oper_it = static_route_map_.begin();

    while ((static_route_cfg_it != static_route_list.end()) &&
           (oper_it != static_route_map_.end())) {
        Ip4Prefix static_route_prefix(static_route_cfg_it->address.to_v4(),
                                      static_route_cfg_it->prefix_length);
        if (static_route_prefix < oper_it->first) {
            LocateStaticRoutePrefix(*static_route_cfg_it);
            static_route_cfg_it++;
        } else if (static_route_prefix > oper_it->first) {
            RemoveStaticRoutePrefix(oper_it->first);
            oper_it++;
        } else {
            LocateStaticRoutePrefix(*static_route_cfg_it);
            static_route_cfg_it++;
            oper_it++;
        }
    }

    for (; oper_it != static_route_map_.end(); oper_it++) {
        RemoveStaticRoutePrefix(oper_it->first);
    }
    for (; static_route_cfg_it != static_route_list.end();
         static_route_cfg_it++) {
        LocateStaticRoutePrefix(*static_route_cfg_it);
    }
}

void StaticRouteMgr::FlushStaticRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    for (StaticRouteMap::iterator it = static_route_map_.begin();
         it != static_route_map_.end(); it++) {
        RemoveStaticRoutePrefix(it->first);
    }
}

void StaticRouteMgr::NotifyAllRoutes() {
    CHECK_CONCURRENCY("bgp::Config");
    for (StaticRouteMap::iterator it = static_route_map_.begin();
         it != static_route_map_.end(); ++it) {
        StaticRoute *static_route =
             static_cast<StaticRoute *>(it->second.get());
        static_route->NotifyRoute();
    }
}

class ShowStaticRouteHandler {
public:
    static void FillStaticRoutesInfo(vector<StaticRouteEntriesInfo> *list,
                                     RoutingInstance *ri) {
        if (!ri->static_route_mgr())
            return;
        if (ri->static_route_mgr()->static_route_map().empty())
            return;

        StaticRouteEntriesInfo info;
        info.set_ri_name(ri->name());
        for (StaticRouteMgr::StaticRouteMap::const_iterator it =
             ri->static_route_mgr()->static_route_map().begin();
             it != ri->static_route_mgr()->static_route_map().end(); it++) {
            StaticRoute *match =
                static_cast<StaticRoute *>(it->second.get());
            StaticRouteInfo static_info;
            static_info.set_prefix(match->static_route_prefix().ToString());
            BgpTable *bgptable = match->bgp_table();
            InetRoute rt_key(match->static_route_prefix());
            BgpRoute *static_rt =
                static_cast<BgpRoute *>(bgptable->Find(&rt_key));
            if (static_rt) {
                static_info.set_static_rt(true);
            } else {
                static_info.set_static_rt(false);
            }

            static_info.set_nexthop(match->nexthop().to_string());
            if (match->nexthop_route()) {
                ShowRouteBrief show_route;
                match->nexthop_route()->FillRouteInfo(bgptable, &show_route);
                static_info.set_nexthop_rt(show_route);
            }

            vector<string> route_target_list;
            for (StaticRoute::RouteTargetList::const_iterator rtit =
                 match->rtarget_list().begin();
                 rtit != match->rtarget_list().end(); ++rtit) {
                route_target_list.push_back(rtit->ToString());
            }
            static_info.set_route_target_list(route_target_list);

            info.static_route_list.push_back(static_info);
        }
        list->push_back(info);
    }

    static bool CallbackS1(const Sandesh *sr,
            const RequestPipeline::PipeSpec ps,
            int stage, int instNum,
            RequestPipeline::InstData *data) {
        const ShowStaticRouteReq *req =
                static_cast<const ShowStaticRouteReq *>(ps.snhRequest_.get());
        BgpSandeshContext *bsc =
            static_cast<BgpSandeshContext *>(req->client_context());
        RoutingInstanceMgr *rim = bsc->bgp_server->routing_instance_mgr();
        vector<StaticRouteEntriesInfo> static_route_entries;
        if (req->get_ri_name() != "") {
            RoutingInstance *ri = rim->GetRoutingInstance(req->get_ri_name());
            if (ri)
                FillStaticRoutesInfo(&static_route_entries, ri);
        } else {
            RoutingInstanceMgr::NameIterator i = rim->name_begin();
            for (; i != rim->name_end(); i++) {
                FillStaticRoutesInfo(&static_route_entries, i->second);
            }
        }
        ShowStaticRouteResp *resp = new ShowStaticRouteResp;
        resp->set_static_route_entries(static_route_entries);
        resp->set_context(req->context());
        resp->Response();
        return true;
    }
};

void ShowStaticRouteReq::HandleRequest() const {
    RequestPipeline::PipeSpec ps(this);
    // Request pipeline has 1 stage:
    RequestPipeline::StageSpec s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    s1.taskId_ = scheduler->GetTaskId("bgp::StaticRoute");
    s1.cbFn_ = ShowStaticRouteHandler::CallbackS1;
    s1.instances_.push_back(0);
    ps.stages_ = list_of(s1);
    RequestPipeline rp(ps);
}
