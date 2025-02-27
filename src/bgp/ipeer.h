/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_IPEER_H_
#define SRC_BGP_IPEER_H_

#include <string>

#include "bgp/bgp_proto.h"
#include "tbb/atomic.h"

class BgpServer;
class PeerCloseManager;

class IPeerUpdate {
public:
    virtual ~IPeerUpdate() { }
    // Printable name
    virtual std::string ToString() const = 0;

    // Send an update. Returns true if the peer can send additional messages,
    // false if it is send blocked.
    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize) = 0;
};

class IPeerDebugStats {
public:
    struct ProtoStats {
        ProtoStats() : total(0), open(0), keepalive(0), notification(0),
        update(0), close(0) {
        }
        uint32_t total;
        uint32_t open;
        uint32_t keepalive;
        uint32_t notification;
        uint32_t update;
        uint32_t close;
    };

    struct ErrorStats {
        ErrorStats()
            : connect_error(0),
              connect_timer(0),
              hold_timer(0),
              open_error(0),
              update_error(0) {
        }
        uint32_t connect_error;
        uint32_t connect_timer;
        uint32_t hold_timer;
        uint32_t open_error;
        uint32_t update_error;
    };

    struct RxErrorStats {
        RxErrorStats() : inet6_bad_xml_token_count(0),
            inet6_bad_prefix_count(0), inet6_bad_nexthop_count(0),
            inet6_bad_afi_safi_count(0) {
        }
        uint32_t inet6_bad_xml_token_count;
        uint32_t inet6_bad_prefix_count;
        uint32_t inet6_bad_nexthop_count;
        uint32_t inet6_bad_afi_safi_count;
    };

    struct UpdateStats {
        UpdateStats() : end_of_rib(0), total(0), reach(0), unreach(0) {
        }
        uint32_t end_of_rib;
        uint32_t total;
        uint32_t reach;
        uint32_t unreach;
    };

    struct SocketStats {
        SocketStats() : calls(0), bytes(0), blocked_count(0),
                        blocked_duration_usecs(0) {
        }
        uint64_t calls;
        uint64_t bytes;
        uint64_t blocked_count;
        uint64_t blocked_duration_usecs;
    };

    virtual ~IPeerDebugStats() { }

    // Reset all counters
    virtual void Clear() = 0;
    // Printable name
    virtual std::string ToString() const = 0;
    // Previous State of the peer
    virtual std::string last_state() const = 0;
    // Last state change At
    virtual std::string last_state_change_at() const = 0;
    // Last error on this peer
    virtual std::string last_error() const = 0;
    // Last Event on this peer
    virtual std::string last_event() const = 0;
    // When was the Last
    virtual std::string last_flap() const = 0;
    // Total number of flaps
    virtual uint32_t num_flaps() const = 0;

    virtual void GetRxProtoStats(ProtoStats *stats) const = 0;
    virtual void GetRxRouteUpdateStats(UpdateStats *stats) const = 0;
    virtual void GetRxSocketStats(SocketStats *stats) const = 0;
    virtual void GetRxErrorStats(RxErrorStats *stats) const = 0;

    virtual void GetTxProtoStats(ProtoStats *stats) const = 0;
    virtual void GetTxRouteUpdateStats(UpdateStats *stats) const = 0;
    virtual void GetTxSocketStats(SocketStats *stats) const = 0;

    virtual void UpdateTxReachRoute(uint32_t count) = 0;
    virtual void UpdateTxUnreachRoute(uint32_t count) = 0;
};

class IPeerClose {
public:
    virtual ~IPeerClose() { }
    // Printable name
    virtual std::string ToString() const = 0;
    virtual PeerCloseManager *close_manager() = 0;
    virtual bool IsCloseGraceful() = 0;
    virtual void CustomClose() = 0;
    virtual bool CloseComplete(bool from_timer, bool gr_cancelled) = 0;
};

class IPeer : public IPeerUpdate {
public:
    virtual ~IPeer() { }
    // Printable name
    virtual std::string ToString() const = 0;
    virtual std::string ToUVEKey() const = 0;
    virtual BgpServer *server() = 0;
    virtual IPeerClose *peer_close() = 0;
    virtual IPeerDebugStats *peer_stats() = 0;
    virtual const IPeerDebugStats *peer_stats() const = 0;
    virtual bool IsReady() const = 0;
    virtual bool IsXmppPeer() const = 0;
    virtual void Close() = 0;
    virtual BgpProto::BgpPeerType PeerType() const = 0;
    virtual uint32_t bgp_identifier() const = 0;
    virtual const std::string GetStateName() const = 0;
    virtual void UpdateRefCount(int count) const = 0;
    virtual tbb::atomic<int> GetRefCount() const = 0;
};

#endif  // SRC_BGP_IPEER_H_
