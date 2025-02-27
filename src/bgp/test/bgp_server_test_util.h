/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __BGP_SERVER_TEST_UTIL_H__
#define __BGP_SERVER_TEST_UTIL_H__

#include <boost/any.hpp>
#include <boost/shared_ptr.hpp>

#include "base/util.h"
#include "base/test/task_test_util.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_peer.h"
#include "bgp/bgp_peer_internal_types.h"
#include "bgp/bgp_peer_types.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_peer_close.h"
#include "bgp/state_machine.h"
#include "bgp/routing-instance/peer_manager.h"
#include "bgp/routing-instance/routing_instance.h"
#include "db/db.h"
#include "db/db_graph.h"
#include "xmpp/xmpp_lifetime.h"
#include "xmpp/xmpp_server.h"

class BgpPeerTest;

class BgpInstanceConfigTest : public BgpInstanceConfig {
public:
    BgpInstanceConfigTest(const std::string &name) : BgpInstanceConfig(name) {
    }
    RouteTargetList *mutable_import_list() { return &import_list_; }
    RouteTargetList *mutable_export_list() { return &export_list_; }
    void set_virtual_network(std::string virtual_network) {
        virtual_network_ = virtual_network;
    }
    void set_virtual_network_index(int virtual_network_index) {
        virtual_network_index_ = virtual_network_index;
    }
};

class BgpTestUtil {
public:
    static BgpInstanceConfigTest *CreateBgpInstanceConfig(
            const std::string &name,
            const std::string import_targets = "",
            const std::string export_targets = "",
            const std::string virtual_network = "",
            int virtual_network_index = 0);
    static void UpdateBgpInstanceConfig(BgpInstanceConfigTest *inst,
            const std::string import_targets,
            const std::string export_targets);
    static void UpdateBgpInstanceConfig(BgpInstanceConfigTest *inst,
        const std::string virtual_network, int virtual_network_index);

    void SetUserData(std::string key, boost::any &value);
    boost::any GetUserData(std::string key);

private:
    std::map<std::string, boost::any> user_data_;
};

class XmppServerTest : public XmppServer {
public:

    XmppServerTest(EventManager *evm) : XmppServer(evm) {
        GetIsPeerCloseGraceful_fnc_ =
            boost::bind(&XmppServerTest::XmppServerIsPeerCloseGraceful, this);
    }
    XmppServerTest(EventManager *evm, const std::string &server_addr) :
            XmppServer(evm, server_addr) {
        GetIsPeerCloseGraceful_fnc_ =
            boost::bind(&XmppServerTest::XmppServerIsPeerCloseGraceful, this);
    }
    XmppServerTest(EventManager *evm, const std::string &server_addr,
                   const XmppChannelConfig *config) :
        XmppServer(evm, server_addr, config) {
        GetIsPeerCloseGraceful_fnc_ =
            boost::bind(&XmppServerTest::XmppServerIsPeerCloseGraceful, this);
    }
    virtual ~XmppServerTest() { }

    virtual bool IsPeerCloseGraceful() {
        return GetIsPeerCloseGraceful_fnc_();
    }

    bool XmppServerIsPeerCloseGraceful() {
        return XmppServer::IsPeerCloseGraceful();
    }

    // Protect connection db with mutex as it is queried from main thread which
    // does not adhere to control-node scheduler policy.
    XmppServerConnection *FindConnection(const std::string &peer_addr) {
        tbb::mutex::scoped_lock lock(mutex_);
        return XmppServer::FindConnection(peer_addr);
    }

    void InsertConnection(XmppServerConnection *connection) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppServer::InsertConnection(connection);
    }

    void RemoveConnection(XmppServerConnection *connection) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppServer::RemoveConnection(connection);
    }

    void InsertDeletedConnection(XmppServerConnection *connection) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppServer::InsertDeletedConnection(connection);
    }

    void RemoveDeletedConnection(XmppServerConnection *connection) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppServer::RemoveDeletedConnection(connection);
    }

    boost::function<bool()> GetIsPeerCloseGraceful_fnc_;

private:
    tbb::mutex mutex_;
};

class StateMachineTest : public StateMachine {
public:
    explicit StateMachineTest(BgpPeer *peer)
        : StateMachine(peer) {
    }
    virtual ~StateMachineTest() { }

    void StartConnectTimer(int seconds) {
        connect_timer_->Start(10,
            boost::bind(&StateMachine::ConnectTimerExpired, this),
            boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
    }

    void StartOpenTimer(int seconds) {
        open_timer_->Start(1000,
            boost::bind(&StateMachine::OpenTimerExpired, this),
            boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
    }

    void StartIdleHoldTimer() {
        if (idle_hold_time_ <= 0)
            return;
        idle_hold_timer_->Start(10,
            boost::bind(&StateMachine::IdleHoldTimerExpired, this),
            boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
    }

    void StartHoldTimer() {
        if (hold_time_msecs_ <= 0) {
            StateMachine::StartHoldTimer();
            return;
        }
        hold_timer_->Start(30,
            boost::bind(&StateMachine::HoldTimerExpired, this),
            boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
    }

    static void set_hold_time_msecs(int hold_time_msecs) {
        hold_time_msecs_ = hold_time_msecs;
    }

    virtual int keepalive_time_msecs() const {
        if (keepalive_time_msecs_)
            return keepalive_time_msecs_;
        return StateMachine::keepalive_time_msecs();
    }

    static void set_keepalive_time_msecs(int keepalive_time_msecs) {
        keepalive_time_msecs_ = keepalive_time_msecs;
    }

private:
    static int hold_time_msecs_;
    static int keepalive_time_msecs_;
};

class BgpServerTest : public BgpServer {
public:
    BgpServerTest(EventManager *evm, const std::string &localname,
                  DB *config_db, DBGraph *config_graph);
    BgpServerTest(EventManager *evm, const std::string &localname);
    ~BgpServerTest();
    bool Configure(const std::string &config);
    BgpPeerTest *FindPeerByUuid(const char *routing_instance,
                                const std::string &uuid);
    BgpPeer *FindPeer(const char *routing_instance,
                      const std::string &peername);
    const BgpPeer *FindMatchingPeer(const char *routing_instance,
                                    const std::string &name);
    void Shutdown(bool verify = true);
    void VerifyShutdown() const;

    DB *config_db() { return config_db_.get(); }
    DBGraph *config_graph() { return config_graph_.get(); }

    static void GlobalSetUp();
    void set_autonomous_system(as_t as) { autonomous_system_ = as; }
    void set_local_autonomous_system(as_t local_as) {
        local_autonomous_system_ = local_as;
    }
    void set_bgp_identifier(uint32_t bgp_id) {
        Ip4Address addr(bgp_id);
        bgp_identifier_ = addr;
    }

    virtual std::string ToString() const;
    virtual bool IsPeerCloseGraceful() {
        return GetIsPeerCloseGraceful_fnc_();
    }

    bool BgpServerIsPeerCloseGraceful() {
        return BgpServer::IsPeerCloseGraceful();
    }

    boost::function<bool()> GetIsPeerCloseGraceful_fnc_;

private:
    void PostShutdown();

    std::string name_;
    boost::scoped_ptr<DB> config_db_;
    boost::scoped_ptr<DBGraph> config_graph_;
    bool cleanup_config_;
};

typedef boost::shared_ptr<BgpServerTest> BgpServerTestPtr;

class BgpPeerTest : public BgpPeer {
public:
    BgpPeerTest(BgpServer *server, RoutingInstance *rtinst,
                const BgpNeighborConfig *config);
    ~BgpPeerTest();

    void BindLocalEndpoint(BgpSession *session);

    static void verbose_name(bool verbose) { verbose_name_ = verbose; }
    std::string ToString() const;

    bool BgpPeerSendUpdate(const uint8_t *msg, size_t msgsize);
    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize) {
        return SendUpdate_fnc_(msg, msgsize);
    }

    bool BgpPeerMpNlriAllowed(uint16_t afi, uint8_t safi);
    virtual bool MpNlriAllowed(uint16_t afi, uint8_t safi) {
        return MpNlriAllowed_fnc_(afi, safi);
    }
  
    bool BgpPeerIsReady();
    void SetDataCollectionKey(BgpPeerInfo *peer_info) const;

    virtual bool IsReady() const {
        return IsReady_fnc_();
    }

    void set_vpn_tables_registered(bool registered) {
        vpn_tables_registered_ = registered;
    }

    boost::function<bool(const uint8_t *, size_t)> SendUpdate_fnc_;
    boost::function<bool(uint16_t, uint8_t)> MpNlriAllowed_fnc_;
    boost::function<bool()> IsReady_fnc_;

    BgpTestUtil util_;

private:
    static bool verbose_name_;
};

class PeerManagerTest : public PeerManager {
public:
    PeerManagerTest(RoutingInstance *instance);
    virtual BgpPeer *PeerLocate(BgpServer *server,
                                const BgpNeighborConfig *config);
    virtual BgpPeer *PeerLookup(boost::asio::ip::tcp::endpoint remote_endpoint);
    virtual void DestroyIPeer(IPeer *ipeer);

private:
    typedef std::map<boost::uuids::uuid, BgpPeer *> PeerByUuidMap;
    PeerByUuidMap peers_by_uuid_;
};

class XmppStateMachineTest : public XmppStateMachine {
public:
    explicit XmppStateMachineTest(XmppConnection *connection, bool active,
                                  bool auth_enabled = false)
        : XmppStateMachine(connection, active, auth_enabled) {
    }
    ~XmppStateMachineTest() { }

    void StartConnectTimer(int seconds) {
        connect_timer_->Start(10,
            boost::bind(&XmppStateMachine::ConnectTimerExpired, this),
            boost::bind(&XmppStateMachine::TimerErrorHandler, this, _1, _2));
    }

    void StartOpenTimer(int seconds) {
        open_timer_->Start(1000,
            boost::bind(&XmppStateMachine::OpenTimerExpired, this),
            boost::bind(&XmppStateMachine::TimerErrorHandler, this, _1, _2));
    }
};

class XmppLifetimeManagerTest : public XmppLifetimeManager {
public:
    explicit XmppLifetimeManagerTest(int task_id)
        : XmppLifetimeManager(task_id), destroy_not_ok_(false) {
    }
    virtual ~XmppLifetimeManagerTest() {
    }

    virtual bool MayDestroy() { return !destroy_not_ok_; }
    virtual void SetQueueDisable(bool disabled) {
        LifetimeManager::SetQueueDisable(disabled);
    }

    void set_destroy_not_ok(bool destroy_not_ok) {
        destroy_not_ok_ = destroy_not_ok;
    }

private:
    bool destroy_not_ok_;
};

#define BGP_WAIT_FOR_PEER_STATE(peer, state)                                   \
    TASK_UTIL_WAIT_EQ(state, (peer)->GetState(), task_util_wait_time(),        \
                      task_util_retry_count(), "Peer State")

#define BGP_WAIT_FOR_PEER_DELETION(peer)  \
    TASK_UTIL_EXPECT_EQ_MSG(NULL, peer, "Peer Deletion")

#define BGP_VERIFY_ROUTE_COUNT(table, count)                                   \
    TASK_UTIL_EXPECT_EQ_MSG(count, static_cast<int>((table)->Size()),          \
                            "Wait for route count")

#define BGP_VERIFY_ROUTE_PRESENCE(table, route) \
    TASK_UTIL_EXPECT_NE_MSG(static_cast<BgpRoute *>(NULL),                     \
                            (table)->Find(route), "Route Presence")

#define BGP_VERIFY_ROUTE_ABSENCE(table, route) \
    TASK_UTIL_EXPECT_EQ_MSG(static_cast<BgpRoute *>(NULL),                     \
                            (table)->Find(route), "Route Absence")

#endif // __BGP_SERVER_TEST_UTIL_H__
