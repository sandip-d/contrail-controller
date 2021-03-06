/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_xmpp_channel.h"

#include <sstream>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <pugixml/pugixml.hpp>

#include "base/label_block.h"
#include "base/logging.h"
#include "base/task_annotations.h"
#include "base/util.h"

#include "bgp/bgp_factory.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_peer_membership.h"
#include "bgp/bgp_ribout.h"
#include "bgp/bgp_server.h"
#include "bgp/inet/inet_table.h"
#include "bgp/inetmcast/inetmcast_table.h"
#include "bgp/enet/enet_table.h"
#include "bgp/ipeer.h"
#include "bgp/origin-vn/origin_vn.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/scheduling_group.h"
#include "bgp/security_group/security_group.h"
#include "bgp/tunnel_encap/tunnel_encap.h"

#include "net/bgp_af.h"
#include "net/mac_address.h"

#include "schema/xmpp_unicast_types.h"
#include "schema/xmpp_multicast_types.h"
#include "schema/xmpp_enet_types.h"

#include "xml/xml_pugi.h"
#include "xmpp/xmpp_connection.h"
#include "xmpp/xmpp_server.h"
#include "xmpp/sandesh/xmpp_peer_info_types.h"

using pugi::xml_node;
using std::auto_ptr;
using std::string;
using std::vector;
using boost::system::error_code;

BgpXmppChannel::Stats::Stats()
    : rt_updates(0), reach(0), unreach(0) {
}

class BgpXmppChannel::PeerClose : public IPeerClose {
public:
    PeerClose(BgpXmppChannel *channel)
       : parent_(channel),
         manager_(BgpObjectFactory::Create<PeerCloseManager>(channel->Peer())) {
    }
    virtual ~PeerClose() {
    }
    virtual std::string ToString() const {
        return parent_ ? parent_->ToString() : "";
    }

    virtual PeerCloseManager *close_manager() {
        return manager_.get();
    }

    virtual bool IsCloseGraceful() {
        if (!parent_ || !parent_->channel_) return false;

        XmppConnection *connection =
            const_cast<XmppConnection *>(parent_->channel_->connection());

        if (!connection || connection->IsActiveChannel()) return false;

        // Check from the server, if GR is enabled or not.
        return static_cast<XmppServer *>(connection->server())->IsPeerCloseGraceful();
    }

    virtual void CustomClose() {
    }

    virtual bool CloseComplete(bool from_timer, bool gr_cancelled) {
        if (!parent_) return true;

        if (!from_timer) {

            // If graceful restart is enabled, do not delete this peer yet
            // However, if a gr is already aborted, do not trigger another gr
            if (!gr_cancelled && IsCloseGraceful()) {
                return false;
            }
        } else {

            // Close is complete off graceful restart timer. Delete this peer
            // if the session has not come back up
            if (parent_->Peer()->IsReady()) return false;
        }

        XmppConnection *connection =
            const_cast<XmppConnection *>(parent_->channel_->connection());

        // TODO: This needs to be cleaned up properly by clearly separting GR
        // entry and exit steps. Avoid duplicate channel deletions.
        if (connection && !connection->IsActiveChannel()) {
            parent_->manager_->Enqueue(parent_);
            parent_ = NULL;
        }
        return true;
    }

    void Close() {
        manager_->Close();
    }

private:
    BgpXmppChannel *parent_;
    std::auto_ptr<PeerCloseManager> manager_;
};

class BgpXmppChannel::PeerStats : public IPeerDebugStats {
public:
    explicit PeerStats(BgpXmppChannel *peer)
        : peer_(peer) {
    }

    // Printable name
    virtual std::string ToString() const {
        return peer_->ToString();
    }

    // Previous State of the peer
    virtual std::string last_state() const {
        return (peer_->channel_->LastStateName());
    }
    // Last state change occurred at
    virtual std::string last_state_change_at() const {
        return (peer_->channel_->LastStateChangeAt());
    }

    // Last error on this peer
    virtual std::string last_error() const {
        return "";
    }

    // Last Event on this peer
    virtual std::string last_event() const {
        return (peer_->channel_->LastEvent());
    }

    // When was the Last
    virtual std::string last_flap() const {
        return (peer_->channel_->LastFlap());
    }

    // Total number of flaps
    virtual uint32_t num_flaps() const {
        return (peer_->channel_->FlapCount());
    }

    virtual void GetRxProtoStats(ProtoStats &stats) const {
        stats.open = peer_->channel_->rx_open();
        stats.close = peer_->channel_->rx_close();
        stats.keepalive = peer_->channel_->rx_keepalive();
        stats.update = peer_->channel_->rx_update();
    }

    virtual void GetTxProtoStats(ProtoStats &stats) const {
        stats.open = peer_->channel_->tx_open();
        stats.close = peer_->channel_->tx_close();
        stats.keepalive = peer_->channel_->tx_keepalive();
        stats.update = peer_->channel_->tx_update();
    }

    virtual void GetRxRouteUpdateStats(UpdateStats &stats)  const {
        stats.total = peer_->stats_[0].rt_updates;
        stats.reach = peer_->stats_[0].reach;
        stats.unreach = peer_->stats_[0].unreach;
    }

    virtual void GetTxRouteUpdateStats(UpdateStats &stats)  const {
        stats.total = peer_->stats_[1].rt_updates;
        stats.reach = peer_->stats_[1].reach;
        stats.unreach = peer_->stats_[1].unreach;
    }

    virtual void GetRxSocketStats(SocketStats &stats)  const {
        TcpServer::SocketStats socket_stats;
        const XmppSession *session = peer_->GetSession();

        if (session) {
            socket_stats = session->GetSocketStats();
            stats.calls = socket_stats.read_calls;
            stats.bytes = socket_stats.read_bytes;
        }
    }

    virtual void GetTxSocketStats(SocketStats &stats)  const {
        TcpServer::SocketStats socket_stats;
        const XmppSession *session = peer_->GetSession();

        if (session) {
            socket_stats = session->GetSocketStats();
            stats.calls = socket_stats.write_calls;
            stats.bytes = socket_stats.write_bytes;
            stats.blocked_count = socket_stats.write_blocked;
            stats.blocked_duration_usecs =
                socket_stats.write_blocked_duration_usecs;
        }
    }

    virtual void UpdateTxUnreachRoute(uint32_t count) {
        peer_->stats_[1].unreach += count;
    }

    virtual void UpdateTxReachRoute(uint32_t count) {
        peer_->stats_[1].reach += count;
    }

private:
    BgpXmppChannel *peer_;
};


class BgpXmppChannel::XmppPeer : public IPeer {
public:
    XmppPeer(BgpServer *server, BgpXmppChannel *channel)
        : server_(server),
          parent_(channel),
          is_deleted_(false) {
        refcount_ = 0;
    }

    virtual ~XmppPeer() {
        assert(GetRefCount() == 0);
    }

    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize);
    virtual std::string ToString() const {
        return parent_->ToString();
    }

    virtual std::string ToUVEKey() const {
        std::ostringstream out;
        if (!parent_->channel_->connection()) return "";
        return parent_->channel_->connection()->ToUVEKey();
    }

    virtual BgpServer *server() {
        return server_;
    }
    virtual IPeerClose *peer_close() {
        return parent_->peer_close_.get();
    }

    virtual IPeerDebugStats *peer_stats() {
        return parent_->peer_stats_.get();
    }
    virtual bool IsReady() const {
        return (parent_->channel_->GetPeerState() == xmps::READY);
    }
    virtual const string GetStateName() const {
        switch (parent_->channel_->GetPeerState()) {
            case xmps::UNKNOWN: return "UNKNOWN";
            case xmps::READY: return "READY";
            case xmps::NOT_READY: return "NOT_READY";
        }
        return "UNKNOWN";
    }
    virtual bool IsXmppPeer() const {
        return true;
    }
    virtual void Close();

    const bool IsDeleted() const { return is_deleted_; }
    void SetDeleted(bool deleted) { is_deleted_ = deleted; }

    virtual BgpProto::BgpPeerType PeerType() const {
        return BgpProto::XMPP;
    }

    virtual uint32_t bgp_identifier() const {
        const XmppConnection *connection = parent_->channel_->connection();
        const boost::asio::ip::tcp::endpoint &remote = connection->endpoint();
        if (remote.address().is_v4()) {
            return remote.address().to_v4().to_ulong();
        }
        return 0;
    }

    virtual void UpdateRefCount(int count) { refcount_ += count; }
    virtual tbb::atomic<int> GetRefCount() const { return refcount_; }

private:
    void WriteReadyCb(const boost::system::error_code &ec) {
        if (!server_) return;
        SchedulingGroupManager *sg_mgr = server_->scheduling_group_manager();
        BGP_LOG_XMPP_PEER(this, SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                          "Sender is ready");
        sg_mgr->SendReady(this);
        send_ready_ = true;
        XmppPeerInfoData peer_info;
        peer_info.set_name(ToUVEKey());
        peer_info.set_send_state("in sync");
        XMPPPeerInfo::Send(peer_info);
    }

    BgpServer *server_;
    BgpXmppChannel *parent_;
    tbb::atomic<int> refcount_;
    bool is_deleted_;
    bool send_ready_;
};

static bool SkipUpdateSend() {
    static bool init_;
    static bool skip_;

    if (init_) return skip_;

    skip_ = getenv("XMPP_SKIP_UPDATE_SEND") != NULL;
    init_ = true;

    return skip_;
}

bool BgpXmppChannel::XmppPeer::SendUpdate(const uint8_t *msg, size_t msgsize) {
    XmppChannel *channel = parent_->channel_;
    if (channel->GetPeerState() == xmps::READY) {
        parent_->stats_[1].rt_updates ++;
        if (SkipUpdateSend()) return true;
        send_ready_ = channel->Send(msg, msgsize, xmps::BGP,
                boost::bind(&BgpXmppChannel::XmppPeer::WriteReadyCb, this, _1));
        if (!send_ready_) {
            XmppPeerInfoData peer_info;
            peer_info.set_name(ToUVEKey());
            peer_info.set_send_state("not in sync");
            XMPPPeerInfo::Send(peer_info);
        }
        return send_ready_;
    } else {
        return false;
    }
}

void BgpXmppChannel::XmppPeer::Close() {
    SetDeleted(true);
    if (server_ == NULL) {
        return;
    }
    parent_->peer_close_->Close();
}

BgpXmppChannel::BgpXmppChannel(XmppChannel *channel, BgpServer *bgp_server,
        BgpXmppChannelManager *manager)
    : peer_id_(xmps::BGP), channel_(channel), bgp_server_(bgp_server),
      peer_(new XmppPeer(bgp_server, this)),
      peer_close_(new PeerClose(this)),
      peer_stats_(new PeerStats(this)),
      bgp_policy_(peer_->PeerType(), RibExportPolicy::XMPP, 0, -1, 0),
      manager_(manager),
      deleted_(false),
      defer_close_(false),
      membership_response_worker_(
            TaskScheduler::GetInstance()->GetTaskId("xmpp::StateMachine"),
            channel->connection()->GetIndex(),
            boost::bind(&BgpXmppChannel::MembershipResponseHandler, this, _1)),
      lb_mgr_(new LabelBlockManager()) {

    channel_->RegisterReceive(peer_id_,
         boost::bind(&BgpXmppChannel::ReceiveUpdate, this, _1));
}

BgpXmppChannel::~BgpXmppChannel() {
    if (channel_->connection() && !channel_->connection()->IsActiveChannel()) {
        CHECK_CONCURRENCY("bgp::Config");
    }

    if (manager_)
        manager_->RemoveChannel(channel_);
    STLDeleteElements(&defer_q_);
    assert(peer_->IsDeleted());
    channel_->UnRegisterReceive(peer_id_);
}

const XmppSession *BgpXmppChannel::GetSession() const {
    if (channel_ && channel_->connection()) {
        return channel_->connection()->session();
    }
    return NULL;
}

std::string BgpXmppChannel::ToString() const {
    return channel_->ToString();
}

std::string BgpXmppChannel::StateName() const {
    return channel_->StateName();
}

void BgpXmppChannel::RoutingInstanceCreateCallback(std::string vrf_name) {
    VrfMembershipRequestMap::iterator it = vrf_membership_request_map_.find(vrf_name);
    if (it == vrf_membership_request_map_.end())
        return;
    if (!defer_close_)
        ProcessDeferredSubscribeRequest(vrf_name, it->second);
    vrf_membership_request_map_.erase(it);
}

IPeer *BgpXmppChannel::Peer() {
    return peer_.get();
}

bool BgpXmppChannel::XmppDecodeAddress(int af, const string &address,
                                       IpAddress *addrp) {
    switch (af) {
    case BgpAf::IPv4:
        break;
    default:
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                          "Unsupported address family:" << af);
        return false;
    }

    error_code error;
    *addrp = IpAddress::from_string(address, error);
    if (error) {
        return false;
    }
    return true;
}

void BgpXmppChannel::ProcessMcastItem(std::string vrf_name,
                                      const pugi::xml_node &node,
                                      bool add_change) {
    autogen::McastItemType item;
    item.Clear();

    if (!item.XmlParse(node)) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                   BGP_LOG_FLAG_ALL, "Invalid multicast message received");
        return;
    }

    // NLRI ipaddress/mask
    if (item.entry.nlri.af != BgpAf::IPv4) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
           BGP_LOG_FLAG_ALL, "Unsupported address family:" << item.entry.nlri.af
           << " for multicast route");
        return;
    }

    if (item.entry.nlri.safi != BgpAf::Mcast) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
            BGP_LOG_FLAG_ALL, "Unsupported safi:" << item.entry.nlri.safi <<
            " for multicast route");
        return;
    }

    error_code error;
    IpAddress grp_address = IpAddress::from_string("0.0.0.0", error);
    if (!item.entry.nlri.group.empty()) {
        if (!(XmppDecodeAddress(item.entry.nlri.af,
                                item.entry.nlri.group, &grp_address))) {
            BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                "Error parsing group address:" << item.entry.nlri.group <<
                " for family:" << item.entry.nlri.af);
            return;
        }
    }

    IpAddress src_address = IpAddress::from_string("0.0.0.0", error);
    if (!item.entry.nlri.source.empty()) {
        if (!(XmppDecodeAddress(item.entry.nlri.af,
                                item.entry.nlri.source, &src_address))) {
            BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                "Error parsing source address:" << item.entry.nlri.source <<
                " for family:" << item.entry.nlri.af);
            return;
        }
    }

    RoutingInstanceMgr *instance_mgr = bgp_server_->routing_instance_mgr();
    if (!instance_mgr) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
              BGP_LOG_FLAG_ALL,
              " ProcessMcastItem: Routing Instance Manager not found");
        return;
    }
    RoutingInstance *rt_instance = instance_mgr->GetRoutingInstance(vrf_name);
    bool subscribe_pending = false;
    int instance_id = -1;
    BgpTable *table = NULL;
    //Build the key to the Multicast DBTable
    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    if (rt_instance != NULL) {
        table = rt_instance->GetTable(Address::INETMCAST);
        if (table == NULL) {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                       BGP_LOG_FLAG_ALL,
                                       "Inet Multicast table not found");
            return;
        }


        //check if Registration is pending
        RoutingTableMembershipRequestMap::iterator loc =
            routingtable_membership_request_map_.find(table->name());
        if (loc != routingtable_membership_request_map_.end()) {
            if (loc->second.pending_req == SUBSCRIBE) {
                instance_id = loc->second.instance_id;
                subscribe_pending = true;
            } else {
                BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                   SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                   "Inet Multicast Route not processed as no subscription pending");
                return;
            }
        } else {
            if (IPeerRib *rib = mgr->IPeerRibFind(peer_.get(), table)) {
                instance_id = rib->instance_id();
            } else {
                BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                   SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                   "Inet Multicast Route not processed as peer is not registered");
                return;
            }
        }
    } else {
        //check if Registration is pending before routing instance create
        VrfMembershipRequestMap::iterator loc =
            vrf_membership_request_map_.find(vrf_name);
        if (loc != vrf_membership_request_map_.end()) {
            subscribe_pending = true;
            instance_id = loc->second;
        } else {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
               SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
               "Inet Multicast Route not processed as no subscription pending");
            return;
        }
    }

    RouteDistinguisher mc_rd(peer_->bgp_identifier(), instance_id);
    InetMcastPrefix mc_prefix(mc_rd, grp_address.to_v4(), src_address.to_v4());

    //Build and enqueue a DB request for route-addition
    DBRequest req;
    req.key.reset(new InetMcastTable::RequestKey(mc_prefix, peer_.get()));

    uint32_t flags = 0;
    ExtCommunitySpec ext;

    if (add_change) {
        req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        vector<uint32_t> labels;

        // Agents should send only one next-hop in the item
        if (item.entry.next_hops.next_hop.size() != 1) {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                    SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                    "More than one nexthop received for the group:"
                    << item.entry.nlri.group);
                return;
        }

        // Label Allocation item.entry.label by parsing the range
        if (!stringToIntegerList(item.entry.next_hops.next_hop[0].label, "-", labels) ||
            labels.size() != 2) {
            BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                "Bad label block range:" << item.entry.next_hops.next_hop[0].label);
            return;
        }

        BgpAttrSpec attrs;
        LabelBlockPtr lbptr = lb_mgr_->LocateBlock(labels[0], labels[1]);

        BgpAttrLabelBlock attr_label(lbptr);
        attrs.push_back(&attr_label);

        //Next-hop ipaddress
        IpAddress nh_address;
        if (!(XmppDecodeAddress(item.entry.next_hops.next_hop[0].af,
                                item.entry.next_hops.next_hop[0].address, &nh_address))) {
            BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                "Error parsing nexthop address:" <<
                 item.entry.next_hops.next_hop[0].address <<
                " family:" << item.entry.next_hops.next_hop[0].af <<
                " for multicast route");
            return;
        }
        BgpAttrNextHop nexthop(nh_address.to_v4().to_ulong());
        attrs.push_back(&nexthop);

        // Tunnel Encap list
        bool no_valid_tunnel_encap = true;
        for (std::vector<std::string>::const_iterator it =
             item.entry.next_hops.next_hop[0].tunnel_encapsulation_list.begin();
             it !=
             item.entry.next_hops.next_hop[0].tunnel_encapsulation_list.end();
             it++) {
             TunnelEncap tun_encap(*it);
             if (tun_encap.tunnel_encap() != TunnelEncapType::UNSPEC) {
                 no_valid_tunnel_encap = false;
                 ext.communities.push_back(tun_encap.GetExtCommunityValue());
             }
        }

        // If all of the tunnel encaps published by the agent is invalid,
        // mark the path as infeasible
        // If agent has not published any tunnel encap, default the tunnel
        // encap to "gre"
        //
        if (!item.entry.next_hops.next_hop[0].tunnel_encapsulation_list.tunnel_encapsulation.empty() &&
            no_valid_tunnel_encap) {
            flags = BgpPath::NoTunnelEncap;
        }

        // We may have extended communities for tunnel encapsulation.
        if (!ext.communities.empty())
            attrs.push_back(&ext);

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attrs);
        req.data.reset(new InetMcastTable::RequestData(attr, flags, 0));
        stats_[0].reach++;
    } else {
        req.oper = DBRequest::DB_ENTRY_DELETE;
        stats_[0].unreach++;
    }


    if (subscribe_pending) {
        //
        // We will Q all route request till register request is processed
        //
        DBRequest *request_entry = new DBRequest();
        request_entry->Swap(&req);
        std::string table_name =
            RoutingInstance::GetTableNameFromVrf(vrf_name, Address::INETMCAST);
        defer_q_.insert(std::make_pair(std::make_pair(vrf_name, table_name),
                                       request_entry));
        return;
    }

    assert(table);
    if (mgr && !mgr->PeerRegistered(peer_.get(), table)) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
            BGP_LOG_FLAG_ALL,
            "Peer:" << peer_.get() << " not subscribed to table " <<
            table->name());
        return;
    }

    BGP_TRACE_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                               "Inet Multicast Group" 
                               << item.entry.nlri.group <<
                               " Source " << item.entry.nlri.source <<
                               " Label Range: " <<
                               ((item.entry.next_hops.next_hop.size() == 1) ?
                                 item.entry.next_hops.next_hop[0].label.c_str():
                                 "Invalid Label Range")
                               << " from peer:" << peer_->ToString() <<
                               " is enqueued for " <<
                               (add_change ? "add/change" : "delete"));
    table->Enqueue(&req);
}

void BgpXmppChannel::ProcessItem(string vrf_name,
                                 const pugi::xml_node &node, bool add_change) {
    autogen::ItemType item;
    item.Clear();

    if (!item.XmlParse(node)) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Invalid message received");
        return;
    }

    // NLRI ipaddress/mask
    if (item.entry.nlri.af != BgpAf::IPv4) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Unsupported address family");
        return;
    }

    error_code error;
    Ip4Prefix rt_prefix = Ip4Prefix::FromString(item.entry.nlri.address,
                                                &error);
    if (error) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Bad address string: " <<
                                   item.entry.nlri.address);
        return;
    }

    RoutingInstanceMgr *instance_mgr = bgp_server_->routing_instance_mgr();
    if (!instance_mgr) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
              BGP_LOG_FLAG_ALL,
              " ProcessItem: Routing Instance Manager not found");
        return;
    }

    RoutingInstance *rt_instance = instance_mgr->GetRoutingInstance(vrf_name);
    bool subscribe_pending = false;
    int instance_id = -1;
    BgpTable *table = NULL;
    if (rt_instance != NULL) {
        table = rt_instance->GetTable(Address::INET);
        if (table == NULL) {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                       BGP_LOG_FLAG_ALL, "Inet table not found");
            return;
        }

        RoutingTableMembershipRequestMap::iterator loc =
            routingtable_membership_request_map_.find(table->name());
        if (loc != routingtable_membership_request_map_.end()) {
            // We have rxed unregister request for a table and
            // receiving route update for the same table
            if (loc->second.pending_req != SUBSCRIBE) {
                BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                  BGP_LOG_FLAG_ALL,
                                  "Received route update after unregister req : "
                                  << table->name());
                return;
            }
            subscribe_pending = true;
            instance_id = loc->second.instance_id;
        } else {
            // Bail if we are not subscribed to the table
            PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
            const IPeerRib *peer_rib = mgr->IPeerRibFind(peer_.get(), table);
            if (!peer_rib) {
                BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                   SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                   "Peer:" << peer_.get() << " not subscribed to table " <<
                   table->name());
                return;
            }
            instance_id = peer_rib->instance_id();
        }
    } else {
        //check if Registration is pending before routing instance create
        VrfMembershipRequestMap::iterator loc =
            vrf_membership_request_map_.find(vrf_name);
        if (loc != vrf_membership_request_map_.end()) {
            subscribe_pending = true;
            instance_id = loc->second;
        } else {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
               SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
               "Inet Route not processed as no subscription pending");
            return;
        }
    }

    if (instance_id == -1)
        instance_id = rt_instance->index();

    InetTable::RequestData::NextHops nexthops;
    DBRequest req;
    req.key.reset(new InetTable::RequestKey(rt_prefix, peer_.get()));

    IpAddress nh_address(Ip4Address(0));
    uint32_t label = 0;
    uint32_t flags = 0;
    ExtCommunitySpec ext;

    if (add_change) {
        req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        BgpAttrSpec attrs;

        if (!item.entry.next_hops.next_hop.empty()) {
            for (size_t i = 0; i < item.entry.next_hops.next_hop.size(); i++) {
                InetTable::RequestData::NextHop nexthop;

                IpAddress nhop_address(Ip4Address(0));
                if (!(XmppDecodeAddress(
                          item.entry.next_hops.next_hop[i].af,
                          item.entry.next_hops.next_hop[i].address,
                          &nhop_address))) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                        BGP_LOG_FLAG_ALL, "Error parsing nexthop address:" <<
                        item.entry.next_hops.next_hop[i].address <<
                        " family:" << item.entry.next_hops.next_hop[i].af <<
                        " for unicast route");
                    return;
                }

                if (i == 0) {
                    nh_address = nhop_address;
                    label = item.entry.next_hops.next_hop[0].label;
                }

                bool no_valid_tunnel_encap = true;

                // Tunnel Encap list
                for (std::vector<std::string>::const_iterator it =
                     item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.begin();
                     it !=
                     item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.end();
                     it++) {
                    TunnelEncap tun_encap(*it);
                    if (tun_encap.tunnel_encap() != TunnelEncapType::UNSPEC) {
                        no_valid_tunnel_encap = false;
                        if (i == 0) {
                            ext.communities.push_back(tun_encap.GetExtCommunityValue());
                        }
                        nexthop.tunnel_encapsulations_.push_back(tun_encap.GetExtCommunity());
                    }
                }

                //
                // If all of the tunnel encaps published by the agent is invalid,
                // mark the path as infeasible
                // If agent has not published any tunnel encap, default the tunnel
                // encap to "gre"
                //
                if (!item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.tunnel_encapsulation.empty() &&
                    no_valid_tunnel_encap) {
                    flags = BgpPath::NoTunnelEncap;
                }

                nexthop.flags_ = flags;
                nexthop.address_ = nhop_address;
                nexthop.label_ = item.entry.next_hops.next_hop[i].label;
                nexthop.source_rd_ = RouteDistinguisher(
                                         nhop_address.to_v4().to_ulong(),
                                         instance_id);
                nexthops.push_back(nexthop);
            }
        }

        BgpAttrNextHop nexthop(nh_address.to_v4().to_ulong());
        attrs.push_back(&nexthop);

        BgpAttrSourceRd source_rd(
            RouteDistinguisher(nh_address.to_v4().to_ulong(), instance_id));
        attrs.push_back(&source_rd);

        // SGID list
        for (std::vector<int>::iterator it =
             item.entry.security_group_list.security_group.begin();
             it != item.entry.security_group_list.security_group.end();
             it++) {
            SecurityGroup sg(bgp_server_->autonomous_system(), *it);
            ext.communities.push_back(sg.GetExtCommunityValue());
        }

        if (rt_instance) {
            OriginVn origin_vn(bgp_server_->autonomous_system(),
                rt_instance->virtual_network_index());
            ext.communities.push_back(origin_vn.GetExtCommunityValue());
        }

        // We may have extended communities for tunnel encapsulation, security
        // groups and origin vn.
        if (!ext.communities.empty())
            attrs.push_back(&ext);

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attrs);

        req.data.reset(new InetTable::RequestData(attr, nexthops));
        stats_[0].reach++;
    } else {
        req.oper = DBRequest::DB_ENTRY_DELETE;
        stats_[0].unreach++;
    }

    // Defer all route requests till register request is processed
    if (subscribe_pending) {
        DBRequest *request_entry = new DBRequest();
        request_entry->Swap(&req);
        std::string table_name =
            RoutingInstance::GetTableNameFromVrf(vrf_name, Address::INET);
        defer_q_.insert(std::make_pair(std::make_pair(vrf_name, table_name),
                                       request_entry));
        return;
    }

    assert(table);

    BGP_TRACE_XMPP_PEER_INSTANCE(Peer(), vrf_name, 
                               "Inet route " << item.entry.nlri.address <<
                               " with next-hop " << nh_address
                               << " and label " << label
                               <<  " is enqueued for "
                               << (add_change ? "add/change" : "delete"));
    table->Enqueue(&req);
}

void BgpXmppChannel::ProcessEnetItem(string vrf_name,
                                     const pugi::xml_node &node,
                                     bool add_change) {
    autogen::EnetItemType item;
    item.Clear();

    if (!item.XmlParse(node)) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Invalid message received");
        return;
    }

    // NLRI ipaddress/mask
    if (item.entry.nlri.af != BgpAf::L2Vpn) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Unsupported address family");
        return;
    }

    error_code error;
    MacAddress mac_addr = MacAddress::FromString(item.entry.nlri.mac, &error);

    if (error) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Bad mac address string: " <<
                                   item.entry.nlri.mac);
        return;
    }

    Ip4Prefix ip_prefix =
        Ip4Prefix::FromString(item.entry.nlri.address, &error);
    if (error) {
        BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                   BGP_LOG_FLAG_ALL,
                                   "Bad address string: " <<
                                   item.entry.nlri.address);
        return;
    }
    EnetPrefix enet_prefix(mac_addr, ip_prefix);

    RoutingInstanceMgr *instance_mgr = bgp_server_->routing_instance_mgr();
    if (!instance_mgr) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
              BGP_LOG_FLAG_ALL,
              " ProcessEnetItem: Routing Instance Manager not found");
        return;
    }

    RoutingInstance *rt_instance = instance_mgr->GetRoutingInstance(vrf_name);
    bool subscribe_pending = false;
    int instance_id = -1;
    BgpTable *table = NULL;
    if (rt_instance != NULL) {
        table = rt_instance->GetTable(Address::ENET);
        if (table == NULL) {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name, SandeshLevel::SYS_WARN,
                                       BGP_LOG_FLAG_ALL, "Enet table not found");
            return;
        }

        RoutingTableMembershipRequestMap::iterator loc =
            routingtable_membership_request_map_.find(table->name());
        if (loc != routingtable_membership_request_map_.end()) {
            // We have rxed unregister request for a table and
            // receiving route update for the same table
            if (loc->second.pending_req != SUBSCRIBE) {
                BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                  BGP_LOG_FLAG_ALL,
                                  "Received route update after unregister req : "
                                  << table->name());
                return;
            }
            subscribe_pending = true;
            instance_id = loc->second.instance_id;
        } else {
            // Bail if we are not subscribed to the table
            PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
            const IPeerRib *peer_rib = mgr->IPeerRibFind(peer_.get(), table);
            if (!peer_rib) {
                BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                   SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                   "Peer:" << peer_.get() << " not subscribed to table " <<
                   table->name());
                return;
            }
            instance_id = peer_rib->instance_id();
        }
    } else {
        //check if Registration is pending before routing instance create
        VrfMembershipRequestMap::iterator loc =
            vrf_membership_request_map_.find(vrf_name);
        if (loc != vrf_membership_request_map_.end()) {
            subscribe_pending = true;
            instance_id = loc->second;
        } else {
            BGP_LOG_XMPP_PEER_INSTANCE(Peer(), vrf_name,
               SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
               "Enet Route not processed as no subscription pending");
            return;
        }
    }

    if (instance_id == -1)
        instance_id = rt_instance->index();

    EnetTable::RequestData::NextHops nexthops;
    DBRequest req;
    ExtCommunitySpec ext;
    req.key.reset(new EnetTable::RequestKey(enet_prefix, peer_.get()));

    IpAddress nh_address(Ip4Address(0));
    uint32_t label = 0;
    uint32_t flags = 0;

    if (add_change) {
        req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        BgpAttrSpec attrs;

        if (!item.entry.next_hops.next_hop.empty()) {
            for (size_t i = 0; i < item.entry.next_hops.next_hop.size(); i++) {
                EnetTable::RequestData::NextHop nexthop;
                IpAddress nhop_address(Ip4Address(0));

                if (!(XmppDecodeAddress(
                          item.entry.next_hops.next_hop[i].af,
                          item.entry.next_hops.next_hop[i].address,
                          &nhop_address))) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                        "Error parsing nexthop address:" <<
                        item.entry.next_hops.next_hop[i].address <<
                        " family:" << item.entry.next_hops.next_hop[i].af <<
                        " for enet route");
                    return;
                }
                if (i == 0) {
                    nh_address = nhop_address;
                    label = item.entry.next_hops.next_hop[0].label;
                }

                // Tunnel Encap list
                bool no_valid_tunnel_encap = true;
                for (std::vector<std::string>::const_iterator it =
                     item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.begin();
                     it !=
                     item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.end();
                     it++) {
                    TunnelEncap tun_encap(*it);
                    if (tun_encap.tunnel_encap() != TunnelEncapType::UNSPEC) {
                        no_valid_tunnel_encap = false;
                        if (i == 0) {
                            ext.communities.push_back(tun_encap.GetExtCommunityValue());
                        }
                        nexthop.tunnel_encapsulations_.push_back(tun_encap.GetExtCommunity());
                    }
                }
                //
                // If all of the tunnel encaps published by the agent is invalid,
                // mark the path as infeasible
                // If agent has not published any tunnel encap, default the tunnel
                // encap to "gre"
                //
                if (!item.entry.next_hops.next_hop[i].tunnel_encapsulation_list.tunnel_encapsulation.empty() &&
                    no_valid_tunnel_encap)
                    flags = BgpPath::NoTunnelEncap;

                nexthop.flags_ = flags;
                nexthop.address_ = nhop_address;
                nexthop.label_ = item.entry.next_hops.next_hop[i].label;
                nexthop.source_rd_ = RouteDistinguisher(
                                         nhop_address.to_v4().to_ulong(),
                                         instance_id);
                nexthops.push_back(nexthop);
            }
        }

        BgpAttrNextHop nexthop(nh_address.to_v4().to_ulong());
        attrs.push_back(&nexthop);

        BgpAttrSourceRd source_rd(
            RouteDistinguisher(nh_address.to_v4().to_ulong(), instance_id));
        attrs.push_back(&source_rd);

        if (!ext.communities.empty())
            attrs.push_back(&ext);

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attrs);

        req.data.reset(new EnetTable::RequestData(attr, nexthops));
        stats_[0].reach++;
    } else {
        req.oper = DBRequest::DB_ENTRY_DELETE;
        stats_[0].unreach++;
    }

    // Defer all route requests till register request is processed
    if (subscribe_pending) {
        DBRequest *request_entry = new DBRequest();
        request_entry->Swap(&req);
        std::string table_name =
            RoutingInstance::GetTableNameFromVrf(vrf_name, Address::ENET);
        defer_q_.insert(std::make_pair(std::make_pair(vrf_name, table_name),
                                       request_entry));
        return;
    }

    assert(table);

    BGP_TRACE_XMPP_PEER_INSTANCE(Peer(), vrf_name,
                               "Enet route " << item.entry.nlri.mac << ","
                               << item.entry.nlri.address
                               << " with next-hop " << nh_address
                               << " and label " << label
                               <<  " is enqueued for "
                               << (add_change ? "add/change" : "delete"));
    table->Enqueue(&req);
}

void BgpXmppChannel::DequeueRequest(const string &table_name,
                                    DBRequest *request) {
    BgpTable *table = static_cast<BgpTable *>
        (bgp_server_->database()->FindTable(table_name));
    if (table == NULL) {
        return;
    }
    auto_ptr<DBRequest> ptr(request);
    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    if (mgr && !mgr->PeerRegistered(peer_.get(), table)) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                          "Peer:" << peer_->ToString()
                          << " not subscribed to instance " << table->name());
        return;
    }

    if (request->oper == DBRequest::DB_ENTRY_ADD_CHANGE) {
        // In cases where RoutingInstance is not yet created when RouteAdd
        // request is received from agent, the origin_vn is not set in the
        // DBRequest. Fill the origin_vn info in the route attribute.
        BgpTable::RequestData *data =
            static_cast<BgpTable::RequestData *>(request->data.get());
        BgpAttrPtr attr =  data->attrs();
        RoutingInstance *rt_instance = table->routing_instance();
        assert(rt_instance);
        ExtCommunity::ExtCommunityList origin_vn_list;
        OriginVn origin_vn(bgp_server_->autonomous_system(),
            rt_instance->virtual_network_index());
        origin_vn_list.push_back(origin_vn.GetExtCommunity());
        ExtCommunityPtr ext_community =
            bgp_server_->extcomm_db()->ReplaceOriginVnAndLocate(
                                attr->ext_community(), origin_vn_list);
        BgpAttrPtr new_attr =
            bgp_server_->attr_db()->ReplaceExtCommunityAndLocate(
                attr.get(), ext_community);
        data->set_attrs(new_attr);
    }

    table->Enqueue(ptr.get());
}

bool BgpXmppChannel::ResumeClose() {
    peer_->Close();
    return true;
}

void BgpXmppChannel::RegisterTable(BgpTable *table, int instance_id) {
    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    mgr->Register(peer_.get(), table, bgp_policy_, instance_id,
            boost::bind(&BgpXmppChannel::MembershipRequestCallback,
                    this, _1, _2));
}

void BgpXmppChannel::UnregisterTable(BgpTable *table) {
    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    mgr->Unregister(peer_.get(), table,
            boost::bind(&BgpXmppChannel::MembershipRequestCallback,
                    this, _1, _2));
}

bool BgpXmppChannel::MembershipResponseHandler(std::string table_name) {
    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_ALL,
                      "MembershipResponseHandler for table " << table_name);

    RoutingTableMembershipRequestMap::iterator loc =
        routingtable_membership_request_map_.find(table_name);
    if (loc == routingtable_membership_request_map_.end()) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                          "table " << table_name << " not in request queue");
        assert(0);
    }

    if (defer_close_) {
        routingtable_membership_request_map_.erase(loc);
        if (routingtable_membership_request_map_.size()) {
            return true;
        }
        defer_close_ = false;
        ResumeClose();
        return true;
    }

    BgpTable *table = static_cast<BgpTable *>
        (bgp_server_->database()->FindTable(table_name));
    if (table == NULL) {
        routingtable_membership_request_map_.erase(loc);
        return true;
    }

    MembershipRequestState state = loc->second;
    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_ALL,
            "MembershipResponseHandler for table " << table_name <<
            " current req = " <<
            ((state.current_req == SUBSCRIBE) ? "subscribe" : "unsubscribe") <<
            " pending req = " <<
            ((state.pending_req == SUBSCRIBE) ? "subscribe" : "unsubscribe"));

    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    if ((state.current_req == UNSUBSCRIBE) &&
        (state.pending_req == SUBSCRIBE)) {
        //
        // Rxed Register while processing unregister
        //
        RegisterTable(table, state.instance_id);
        loc->second.current_req = SUBSCRIBE;
        return true;
    } else if ((state.current_req == SUBSCRIBE) &&
               (state.pending_req == UNSUBSCRIBE)) {
        //
        // Rxed UnRegister while processing register
        //
        UnregisterTable(table);
        loc->second.current_req = UNSUBSCRIBE;
        return true;
    }

    routingtable_membership_request_map_.erase(loc);

    VrfTableName vrf_n_table =
        std::make_pair(table->routing_instance()->name(), table_name);

    if (state.pending_req == UNSUBSCRIBE) {
        assert(!defer_q_.count(vrf_n_table));
        return true;
    } else if (state.pending_req == SUBSCRIBE) {
        IPeerRib *rib = mgr->IPeerRibFind(peer_.get(), table);
        rib->set_instance_id(state.instance_id);
    }

    for(DeferQ::iterator it = defer_q_.find(vrf_n_table);
        (it != defer_q_.end() && it->first.second == table_name); it++) {
        DequeueRequest(table_name, it->second);
    }
    // Erase all elements for the table
    defer_q_.erase(vrf_n_table);

    std::vector<std::string> registered_tables;
    mgr->FillRegisteredTable(peer_.get(), registered_tables);

    if (registered_tables.empty()) return true;

    XmppPeerInfoData peer_info;
    peer_info.set_name(peer_->ToUVEKey());
    peer_info.set_routing_tables(registered_tables);
    peer_info.set_send_state("in sync");
    XMPPPeerInfo::Send(peer_info);

    return true;
}

void BgpXmppChannel::MembershipRequestCallback(IPeer *ipeer, BgpTable *table) {
    membership_response_worker_.Enqueue(table->name());
}


void BgpXmppChannel::FlushDeferRegisterRequest() {
    // Erase all elements for the table
    vrf_membership_request_map_.clear();
}

void BgpXmppChannel::FlushDeferQ(std::string vrf_name, std::string table_name) {
    // Erase all elements for the table
    for(DeferQ::iterator it =
        defer_q_.find(std::make_pair(vrf_name, table_name)), itnext;
        (it != defer_q_.end() && it->first.second == table_name);
        it = itnext) {
        itnext = it;
        itnext++;
        delete it->second;
        defer_q_.erase(it);
    }
}

void BgpXmppChannel::FlushDeferQ(std::string vrf_name) {
    // Erase all elements for the table
    for(DeferQ::iterator it = defer_q_.begin(), itnext;
        (it != defer_q_.end() && it->first.first == vrf_name);
        it = itnext) {
        itnext = it;
        itnext++;
        delete it->second;
        defer_q_.erase(it);
    }
}

void BgpXmppChannel::ProcessDeferredSubscribeRequest(std::string vrf_name,
                                                     int instance_id) {
    RoutingInstanceMgr *instance_mgr = bgp_server_->routing_instance_mgr();
    if (!instance_mgr) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
              BGP_LOG_FLAG_ALL,
              " ProcessDeferredSubscribeRequest: RIM not found");
        return;
    }
    RoutingInstance *rt_instance = instance_mgr->GetRoutingInstance(vrf_name);
    assert(rt_instance);

    RoutingInstance::RouteTableList const rt_list = rt_instance->GetTables();
    for (RoutingInstance::RouteTableList::const_iterator it = rt_list.begin();
         it != rt_list.end(); ++it) {

        BgpTable *table = it->second;
        if (table->family() == Address::INETVPN) {
            //Do not register to inetvpn table
            continue;
        }

        RegisterTable(table, instance_id);

        MembershipRequestState state(SUBSCRIBE, instance_id);
        routingtable_membership_request_map_.insert(make_pair(table->name(), state));
    }
}

void BgpXmppChannel::ProcessSubscriptionRequest(
        std::string vrf_name, const XmppStanza::XmppMessageIq *iq,
        bool add_change) {
    PeerRibMembershipManager *mgr = bgp_server_->membership_mgr();
    int instance_id = -1;

    if (add_change) {
        XmlPugi *pugi = reinterpret_cast<XmlPugi *>(iq->dom.get());
        xml_node options = pugi->FindNode("options");
        for (xml_node node = options.first_child(); node;
             node = node.next_sibling()) {
            if (strcmp(node.name(), "instance-id") == 0) {
                instance_id = node.text().as_int();
            }
        }
    }

    RoutingInstanceMgr *instance_mgr = bgp_server_->routing_instance_mgr();
    if (!instance_mgr) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
              BGP_LOG_FLAG_ALL,
              " ProcessSubscriptionRequest: Routing Instance Manager not found");
        return;
    }
    RoutingInstance *rt_instance = instance_mgr->GetRoutingInstance(vrf_name);
    if (rt_instance == NULL) {
        BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                          BGP_LOG_FLAG_ALL,
                          " ReceiveUpdate: Routing Instance " <<
                          vrf_name << " not found");
        if (add_change) {
            vrf_membership_request_map_[vrf_name] = instance_id;
        } else {
            if (vrf_membership_request_map_.erase(vrf_name)) {
                FlushDeferQ(vrf_name);
            }
        }

        return;
    }

    // TODO: handle missing inet/inetmcast etc tables??
    RoutingInstance::RouteTableList const rt_list = rt_instance->GetTables();
    for (RoutingInstance::RouteTableList::const_iterator it = rt_list.begin();
         it != rt_list.end(); ++it) {

        BgpTable *table = it->second;
        if (table->family() == Address::INETVPN)
            continue;
        if (table->family() == Address::EVPN)
            continue;

        if (add_change) {
            RoutingTableMembershipRequestMap::iterator loc =
                routingtable_membership_request_map_.find(table->name());
            if (loc == routingtable_membership_request_map_.end()) {
                IPeerRib *rib = mgr->IPeerRibFind(peer_.get(), table);
                if (rib && !rib->IsStale()) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                      BGP_LOG_FLAG_ALL,
                                      "Received registration req without " <<
                                      "unregister : " << table->name());
                    continue;
                }
                MembershipRequestState state(SUBSCRIBE, instance_id);
                routingtable_membership_request_map_.insert(
                                        make_pair(table->name(), state));
            } else {
                if (loc->second.pending_req == SUBSCRIBE) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                      BGP_LOG_FLAG_ALL,
                                      "Received registration req without " <<
                                      "unregister : " << table->name());
                }
                loc->second.instance_id = instance_id;
                loc->second.pending_req = SUBSCRIBE;
                continue;
            }

            RegisterTable(table, instance_id);
        } else {
            if (defer_q_.count(std::make_pair(vrf_name, table->name()))) {
                BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                  BGP_LOG_FLAG_ALL,
                                  "Flush the DBRoute request on unregister :" <<
                                  table->name());
            }

            // Erase all elements for the table
            FlushDeferQ(vrf_name, table->name());

            RoutingTableMembershipRequestMap::iterator loc =
                routingtable_membership_request_map_.find(table->name());
            if (loc == routingtable_membership_request_map_.end()) {
                IPeerRib *rib = mgr->IPeerRibFind(peer_.get(), table);
                if (!rib) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                      BGP_LOG_FLAG_ALL,
                                      "Received back to back unregister req:" <<
                                      table->name());
                    continue;
                } else {
                    MembershipRequestState state(UNSUBSCRIBE, instance_id);
                    routingtable_membership_request_map_.insert(
                                            make_pair(table->name(), state));
                }
            } else {
                if (loc->second.pending_req == UNSUBSCRIBE) {
                    BGP_LOG_XMPP_PEER(Peer(), SandeshLevel::SYS_WARN,
                                      BGP_LOG_FLAG_ALL,
                                      "Received back to back unregister req:" <<
                                      table->name());
                    continue;
                }
                loc->second.instance_id = -1;
                loc->second.pending_req = UNSUBSCRIBE;
                continue;
            }
            UnregisterTable(table);
        }
    }
}

void BgpXmppChannel::ReceiveUpdate(const XmppStanza::XmppMessage *msg) {
    CHECK_CONCURRENCY("xmpp::StateMachine");

    //
    // Make sure that peer is not set for closure already
    //
    assert(!defer_close_);
    assert(!peer_->IsDeleted());
    assert(!channel_->connection() || !channel_->connection()->ShutdownPending());
    if (msg->type == XmppStanza::IQ_STANZA) {
        const XmppStanza::XmppMessageIq *iq =
                   static_cast<const XmppStanza::XmppMessageIq *>(msg);
        if (iq->iq_type.compare("set") == 0) {

            if (iq->action.compare("subscribe") == 0) {
                ProcessSubscriptionRequest(iq->node, iq, true);
            } else if (iq->action.compare("unsubscribe") == 0) {
                ProcessSubscriptionRequest(iq->node, iq, false);
            } else if (iq->action.compare("publish") == 0) {
                XmlBase *impl = msg->dom.get();
                stats_[0].rt_updates++;
                XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl);
                for (xml_node item = pugi->FindNode("item"); item;
                    item = item.next_sibling()) {
                    if (strcmp(item.name(), "item") != 0) continue;

                        std::string id(iq->as_node.c_str());
                        char *str = const_cast<char *>(id.c_str());
                        char *saveptr;
                        char *af = strtok_r(str, "/", &saveptr);
                        char *safi = strtok_r(NULL, "/", &saveptr);

                        if (atoi(af) == BgpAf::IPv4 &&
                            atoi(safi) == BgpAf::Unicast) {
                            ProcessItem(iq->node, item, iq->is_as_node);
                        } else if (atoi(af) == BgpAf::IPv4 &&
                            atoi(safi) == BgpAf::Mcast) {
                            ProcessMcastItem(iq->node, item, iq->is_as_node);
                        } else if (atoi(af) == BgpAf::L2Vpn &&
                                   atoi(safi) == BgpAf::Enet) {
                            ProcessEnetItem(iq->node, item, iq->is_as_node);
                        }
                }
            }
        }
    }
}

bool BgpXmppChannelManager::DeleteExecutor(BgpXmppChannel *channel) {
    if (channel->deleted()) return true;
    channel->set_deleted(true);

    //
    // TODO: Enqueue an event to the deleter() and deleted this peer and the
    // channel from a different thread to solve concurrency issues
    //
    delete channel;
    return true;
}


// BgpXmppChannelManager routines.

BgpXmppChannelManager::BgpXmppChannelManager(XmppServer *xmpp_server,
                                             BgpServer *server)
    : xmpp_server_(xmpp_server),
      bgp_server_(server),
      queue_(TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0,
             boost::bind(&BgpXmppChannelManager::DeleteExecutor, this, _1)) {
    queue_.SetEntryCallback(
            boost::bind(&BgpXmppChannelManager::IsReadyForDeletion, this));
    if (xmpp_server) {
        xmpp_server->RegisterConnectionEvent(xmps::BGP,
               boost::bind(&BgpXmppChannelManager::XmppHandleChannelEvent,
                           this, _1, _2));
    }
    id_ = server->routing_instance_mgr()->RegisterCreateCallback(
        boost::bind(&BgpXmppChannelManager::RoutingInstanceCreateCallback, this, _1));
}

BgpXmppChannelManager::~BgpXmppChannelManager() {
    assert(channel_map_.empty());
    if (xmpp_server_) {
        xmpp_server_->UnRegisterConnectionEvent(xmps::BGP);
    }

    queue_.Shutdown();
    channel_map_.clear();
    bgp_server_->routing_instance_mgr()->UnregisterCreateCallback(id_);
}

bool BgpXmppChannelManager::IsReadyForDeletion() {
    return bgp_server_->IsReadyForDeletion();
}


void BgpXmppChannelManager::RoutingInstanceCreateCallback(std::string vrf_name) {
    BOOST_FOREACH(XmppChannelMap::value_type &i, channel_map_) {
        i.second->RoutingInstanceCreateCallback(vrf_name);
    }
}


void BgpXmppChannelManager::VisitChannels(BgpXmppChannelManager::VisitorFn fn) {
    BOOST_FOREACH(XmppChannelMap::value_type &i, channel_map_) {
        fn(i.second);
    }
}

BgpXmppChannel *BgpXmppChannelManager::FindChannel(std::string client) {
    BOOST_FOREACH(XmppChannelMap::value_type &i, channel_map_) {
        if (i.second->ToString() == client) {
            return i.second;
        }
    }
    return NULL;
}


BgpXmppChannel *BgpXmppChannelManager::FindChannel(
        const XmppChannel *ch) {
    XmppChannelMap::iterator it = channel_map_.find(ch);
    if (it == channel_map_.end())
        return NULL;
    return it->second;
}


void BgpXmppChannelManager::RemoveChannel(XmppChannel *ch) {
    channel_map_.erase(ch);
}

BgpXmppChannel *BgpXmppChannelManager::CreateChannel(XmppChannel *channel) {
    BgpXmppChannel *ch = new BgpXmppChannel(channel, bgp_server_, this);

    return ch;
}

void BgpXmppChannelManager::XmppHandleChannelEvent(XmppChannel *channel,
                                                   xmps::PeerState state) {
    XmppChannelMap::iterator it = channel_map_.find(channel);

    BgpXmppChannel *bgp_xmpp_channel = NULL;
    if (state == xmps::READY) {
        if (it == channel_map_.end()) {
            bgp_xmpp_channel = CreateChannel(channel);
            channel_map_.insert(std::make_pair(channel, bgp_xmpp_channel));
            BGP_LOG_XMPP_PEER(bgp_xmpp_channel->Peer(),
                              SandeshLevel::UT_DEBUG, BGP_LOG_FLAG_SYSLOG,
                              "Received XmppChannel up event");
        } else {
            bgp_xmpp_channel = (*it).second;
            bgp_xmpp_channel->peer_->SetDeleted(false);

        }
    } else if (state == xmps::NOT_READY) {
        if (it != channel_map_.end()) {
            bgp_xmpp_channel = (*it).second;

            BGP_LOG_XMPP_PEER(bgp_xmpp_channel->Peer(),
                              SandeshLevel::UT_DEBUG, BGP_LOG_FLAG_SYSLOG,
                              "Received XmppChannel down event");

            //
            // Trigger closure of this channel
            //
            bgp_xmpp_channel->Close();
        } else {
            BGP_LOG(BgpMessage, SandeshLevel::SYS_NOTICE, BGP_LOG_FLAG_ALL,
                    "Peer not found on channel not ready event");
        }
    }
    if (bgp_xmpp_channel) {
        XmppPeerInfoData peer_info;
        peer_info.set_name(bgp_xmpp_channel->Peer()->ToUVEKey());
        peer_info.set_send_state("not advertising");
        XMPPPeerInfo::Send(peer_info);
    }
}

void BgpXmppChannel::Close() {
    if (routingtable_membership_request_map_.size()) {
        BGP_LOG(BgpMessage, SandeshLevel::SYS_WARN, BGP_LOG_FLAG_ALL,
                "Peer Close with pending membership request");
        defer_close_ = true;
        FlushDeferRegisterRequest();
        return;
    }
    peer_->Close();
}

//
// Return connection's remote tcp endpoint if available
//
boost::asio::ip::tcp::endpoint BgpXmppChannel::remote_endpoint() {
    const XmppSession *session = GetSession();
    if (session) {
        return session->remote_endpoint();
    }
    return boost::asio::ip::tcp::endpoint();
}

//
// Return connection's local tcp endpoint if available
//
boost::asio::ip::tcp::endpoint BgpXmppChannel::local_endpoint() {
    const XmppSession *session = GetSession();
    if (session) {
        return session->local_endpoint();
    }
    return boost::asio::ip::tcp::endpoint();
}
