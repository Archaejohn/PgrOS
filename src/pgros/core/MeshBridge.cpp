//
// PgrOS <-> Meshtastic bridge -- implementation.
//
// Everything in this file runs on the Meshtastic main task. That is not a
// stylistic preference, it is a correctness requirement:
//
//   * generatePacketId() increments an unguarded function-local static.
//   * NodeDB::meshNodes is an unguarded std::vector that REALLOCATES on insert,
//     so any meshtastic_NodeInfoLite* is invalidated the moment a new node is
//     heard.
//
// So send(), resolve(), me(), listThreads() and every NodeDB or Channels read
// below must be called from the mesh task only. The UI task reaches this data
// exclusively through ChatStore snapshots and EventBus events, both of which
// are copies.
//
// The three shims below are the entry points from Meshtastic:
//
//   PgrosTextModule       observes textMessageModule (inbound text)
//   PgrosRoutingObserver  a MeshModule that sniffs ROUTING_APP acks
//   PgrosNodeObserver     observes nodeDB->newStatus
//
// The first two carry the names MeshBridge.h declares as friends. Note the
// names read backwards relative to what the classes actually are -- the text
// one is an Observer and the routing one is a MeshModule -- but the friend
// declarations fix them, so they stay as-is.
//

#ifdef PGROS

#include "MeshBridge.h"

#include "NodeStatus.h"
#include "Observer.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/MeshModule.h"
#include "mesh/MeshService.h"
#include "mesh/MeshTypes.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/TextMessageModule.h"

#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <string.h>

namespace pgros
{

MeshBridge mesh;

namespace
{

// The wire payload cap. ChatStore sizes its text field at kMaxTextLen (237),
// which is the older constant; the tree we build against says 233.
constexpr uint16_t kPayloadCap = (uint16_t)meshtastic_Constants_DATA_PAYLOAD_LEN;
static_assert(kPayloadCap <= kMaxTextLen, "ChatMessage::text cannot hold a full mesh payload");

// How many in-flight sends we track acks for. A pager cannot realistically have
// more outstanding than this, and the table is a fixed ring so a lost ack ages
// out instead of leaking.
constexpr size_t kPendingMax = 16;

// Node churn on a busy mesh notifies far faster than anyone can read. One
// NodeUpdated per this many ms is plenty for a status bar and a node list.
constexpr uint32_t kNodeEventThrottleMs = 1500;

bool gStarted = false;

// ---------------------------------------------------------------------------
// Node snapshot.
//
// Published by the mesh task, read by the UI task. See the seqlock note in
// MeshBridge.h. gNodeGen is odd while a write is in progress; a reader that
// sees an odd generation, or a generation that moved across its copy, retries.
// ---------------------------------------------------------------------------
NodeBrief gNodes[MeshBridge::kNodeSnapshotMax];
uint16_t gNodeCount = 0;
std::atomic<uint32_t> gNodeGen{0};


struct PendingTx {
    uint32_t packetId = 0; // 0 == free slot
    uint32_t dest = 0;     // NODENUM_BROADCAST or the unicast peer
    ThreadId thread;
    uint32_t atMs = 0;
};

PendingTx gPending[kPendingMax];
size_t gPendingNext = 0;

// Remember an outbound packet so a later routing ack can be attributed to the
// right thread. Oldest entry is overwritten; nothing is allocated.
void notePending(uint32_t packetId, uint32_t dest, const ThreadId &thread)
{
    PendingTx &slot = gPending[gPendingNext];
    gPendingNext = (gPendingNext + 1) % kPendingMax;
    slot.packetId = packetId;
    slot.dest = dest;
    slot.thread = thread;
    slot.atMs = millis();
}

int pendingFind(uint32_t packetId)
{
    if (!packetId)
        return -1;
    for (size_t i = 0; i < kPendingMax; i++)
        if (gPending[i].packetId == packetId)
            return (int)i;
    return -1;
}

// Bounded copy that treats src as at most srcMax bytes and does not assume it is
// NUL-terminated. NodeInfoLite::long_name is char[25] while our snapshot field is
// char[40]; the sizes differ in both directions across this file, so every name
// copy goes through here.
void copyField(char *dst, size_t dstSize, const char *src, size_t srcMax)
{
    if (!dst || !dstSize)
        return;
    size_t n = 0;
    if (src) {
        while (n < srcMax && (n + 1) < dstSize && src[n])
            n++;
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

void postMsgEvent(EventType type, const ThreadId &thread, uint32_t packetId, uint32_t from, MsgStatus status, bool outbound)
{
    Event ev = {};
    ev.type = type;
    ev.atMs = millis();
    ev.msg.thread.direct = thread.direct ? 1 : 0;
    ev.msg.thread.channel = thread.channel;
    ev.msg.thread.peer = thread.peer;
    ev.msg.packetId = packetId;
    ev.msg.from = from;
    ev.msg.status = (uint8_t)status;
    ev.msg.outbound = outbound ? 1 : 0;
    events.post(ev);
}

// Hops travelled, from the two header fields. hop_start == 0 means "unknown" on
// pre-2.3 senders, so it degrades to 0 rather than lying.
uint8_t hopsTravelled(const meshtastic_MeshPacket &mp)
{
    return (mp.hop_start > mp.hop_limit) ? (uint8_t)(mp.hop_start - mp.hop_limit) : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------

// Inbound text. TextMessageModule::handleReceived() calls notifyObservers() from
// inside callModules(), i.e. on the mesh task, in the middle of packet dispatch.
// Nothing here may touch LVGL or block -- it appends one ChatStore record and
// posts one event, and that is deliberately all it does.
class PgrosTextModule : public Observer<const meshtastic_MeshPacket *>
{
  protected:
    int onNotify(const meshtastic_MeshPacket *mp) override
    {
        mesh.onTextMessage(mp);
        return 0; // 0 == let every other observer see this packet too
    }
};

// Routing acks. Registering is the constructor's job (MeshModule.cpp:21), so
// `new PgrosRoutingObserver()` in begin() is the whole registration.
//
// ourPortNum is deliberately left UNKNOWN_APP: it makes MeshModule's
// replyPortMatches() false for us, so callModules() can never ask this module to
// generate a response. We only ever listen.
class PgrosRoutingObserver : public MeshModule
{
  public:
    PgrosRoutingObserver() : MeshModule("pgrosAck") {}

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        return p && p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
               p->decoded.portnum == meshtastic_PortNum_ROUTING_APP;
    }

    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override
    {
        do {
            if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag)
                break;
            if (mp.decoded.portnum != meshtastic_PortNum_ROUTING_APP)
                break;
            if (!isToUs(&mp))
                break;

            // A delivery signal is a ROUTING_APP packet addressed to us whose
            // request_id is the id of something we sent.
            const int idx = pendingFind(mp.decoded.request_id);
            if (idx < 0)
                break;

            meshtastic_Routing decoded = meshtastic_Routing_init_default;
            if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, meshtastic_Routing_fields, &decoded))
                break;

            const bool isAck = (decoded.error_reason == meshtastic_Routing_Error_NONE);
            const bool wasBroadcast = isBroadcast(gPending[idx].dest);
            const bool fromDest = (mp.from == gPending[idx].dest);

            // The ack semantics, which are easy to get wrong:
            //
            //   broadcast -- acks are implicit (someone rebroadcast it), so any
            //                ack at all means it reached the mesh: Delivered.
            //   unicast   -- ONLY an ack whose `from` is the destination proves
            //                delivery. An ack from an intermediate relay means
            //                "I forwarded it", which is Sent, not Delivered.
            //
            // Mirrors CannedMessageModule::handleReceived().
            const bool delivered = isAck && (wasBroadcast || fromDest);
            mesh.onRouting(mp.decoded.request_id, delivered, (uint8_t)decoded.error_reason);
        } while (false);

        // ALWAYS CONTINUE. RoutingModule is what forwards these packets to the
        // phone app; returning STOP here would silently break every official
        // client's delivery indicator.
        return ProcessMessage::CONTINUE;
    }
};

namespace
{

// NodeDB change mirror. NodeStatus itself carries only online/total counts, so
// the changed node is read from nodeDB->updateGUIforNode, which NodeDB sets
// immediately before notifying. That is a raw pointer into the meshNodes vector,
// so it is range-checked against the live vector before being dereferenced -- a
// removeNodeByNum() elsewhere can otherwise leave it dangling.
class PgrosNodeObserver : public Observer<const meshtastic::NodeStatus *>
{
  protected:
    int onNotify(const meshtastic::NodeStatus *) override
    {
        const uint32_t now = millis();
        if (mLastPostMs && (uint32_t)(now - mLastPostMs) < kNodeEventThrottleMs)
            return 0; // throttled: a busy mesh notifies far faster than the UI can use
        mLastPostMs = now;

        // We are on the mesh task here, which is the only place NodeDB may be
        // walked. Republish the snapshot before telling the UI anything changed,
        // so a Contacts screen reacting to this event reads fresh data.
        mesh.refreshNodes();

        Event ev = {};
        ev.type = EventType::NodeUpdated;
        ev.atMs = now;

        if (nodeDB && nodeDB->meshNodes && !nodeDB->meshNodes->empty()) {
            const meshtastic_NodeInfoLite *n = nodeDB->updateGUIforNode;
            const meshtastic_NodeInfoLite *base = nodeDB->meshNodes->data();
            if (n && n >= base && n < base + nodeDB->numMeshNodes) {
                ev.node.num = n->num;
                ev.node.hopsAway = n->has_hops_away ? n->hops_away : 0;
                ev.node.viaMqtt = nodeInfoLiteViaMqtt(n) ? 1 : 0;
            }
        }
        // num == 0 means "some node changed, reload the list" -- NodeStatus does
        // not name a node, so that is the honest signal when we cannot identify
        // one.
        events.post(ev);
        return 0;
    }

  private:
    uint32_t mLastPostMs = 0;
};

PgrosTextModule *gTextObserver = nullptr;
PgrosRoutingObserver *gRoutingModule = nullptr;
PgrosNodeObserver *gNodeObserver = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool MeshBridge::begin()
{
    if (gStarted)
        return true;

    if (!nodeDB || !router || !service) {
        LOG_ERROR("MeshBridge: mesh stack not up (nodeDB=%p router=%p service=%p)", (void *)nodeDB, (void *)router,
                  (void *)service);
        return false;
    }

    // Everything below is `new`, never static: MeshModule instances must not be
    // constructed before setup() runs, because the module registry vector is
    // built lazily by MeshModule's own constructor.
    if (textMessageModule) {
        gTextObserver = new PgrosTextModule();
        gTextObserver->observe(textMessageModule);
    } else {
        LOG_ERROR("MeshBridge: textMessageModule is null, inbound text will not be stored");
    }

    // The MeshModule constructor registers it. There is nothing else to call.
    gRoutingModule = new PgrosRoutingObserver();

    gNodeObserver = new PgrosNodeObserver();
    gNodeObserver->observe(&nodeDB->newStatus);

    gStarted = true;
    refreshNodes();
    LOG_INFO("MeshBridge up: node 0x%08x, %u channels", (unsigned)myNodeNum(), (unsigned)channelCount());
    return true;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void MeshBridge::onTextMessage(const void *packet)
{
    const meshtastic_MeshPacket *pp = static_cast<const meshtastic_MeshPacket *>(packet);
    if (!pp)
        return;
    const meshtastic_MeshPacket &mp = *pp;

    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return;
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return;

    size_t len = mp.decoded.payload.size;
    if (!len)
        return;
    if (len > kMaxTextLen)
        len = kMaxTextLen;

    // getFrom(), not mp.from. A packet the phone app injected carries from == 0,
    // and getFrom() substitutes our own node number for that case.
    const NodeNum senderNum = getFrom(&mp);

    // Resolve the sender HERE, while NodeDB is authoritative and we are on the
    // task that owns it. The names are then frozen into the record, so the UI can
    // render this message forever without touching NodeDB -- including after the
    // sender ages out of the database.
    const SenderIdentity id = resolve(senderNum);

    // Our own node as source means the phone app sent this and Router looped it
    // back to us. Our own send() path is filtered out of callModules by the
    // RX_SRC_LOCAL loopback gate, so this is not a duplicate of send().
    const bool outbound = (senderNum == myNodeNum());
    const bool broadcast = isBroadcast(mp.to);

    ThreadId thread;
    if (broadcast)
        thread = ThreadId::broadcast(mp.channel); // mp.channel, NOT node->channel
    else if (outbound)
        thread = ThreadId::dm(mp.to); // our own DM echo belongs in the peer's thread
    else
        thread = ThreadId::dm(senderNum);

    ChatMessage m;
    m.packetId = mp.id;
    m.from = senderNum;
    m.to = mp.to;
    m.rxTime = mp.has_rx_time ? mp.rx_time : getValidTime(RTCQualityDevice);
    m.uptimeMs = millis();
    m.channel = mp.channel;
    m.hopsAway = hopsTravelled(mp);
    if (mp.has_rx_rssi) {
        int32_t rssi = mp.rx_rssi;
        if (rssi > 127)
            rssi = 127;
        if (rssi < -128)
            rssi = -128;
        m.rssi = (int8_t)rssi;
    }
    m.snr = mp.rx_snr;
    m.status = outbound ? MsgStatus::Sent : MsgStatus::Received;

    uint8_t flags = kFlagNone;
    if (outbound)
        flags |= kFlagOutbound;
    else
        flags |= kFlagUnread;
    if (mp.want_ack)
        flags |= kFlagWantAck;
    if (mp.pki_encrypted)
        flags |= kFlagPki;
    m.flags = flags;

    copyField(m.senderShort, sizeof(m.senderShort), id.shortName, sizeof(id.shortName));
    copyField(m.senderLong, sizeof(m.senderLong), id.longName, sizeof(id.longName));

    memcpy(m.text, mp.decoded.payload.bytes, len);
    m.text[len] = 0;
    m.textLen = (uint16_t)len;

    if (!chatStore.append(thread, m))
        LOG_ERROR("MeshBridge: chat append failed for id=0x%08x", (unsigned)mp.id);

    mRxCount++;

    // Hearing a node is exactly when its Contacts row goes stale, and we are
    // already on the task that may read NodeDB.
    refreshNodes();

    // The UI task learns about this only from the event; it must never be handed
    // the packet, which is about to be recycled into the pool.
    postMsgEvent(EventType::MessageReceived, thread, m.packetId, m.from, m.status, outbound);
}

void MeshBridge::onRouting(uint32_t packetId, bool ok, uint8_t errorReason)
{
    const int idx = pendingFind(packetId);
    if (idx < 0)
        return;
    PendingTx &pt = gPending[idx];

    // `ok` has already been narrowed by the caller to "this ack proves delivery"
    // (see PgrosRoutingObserver::handleReceived). An ack with no error that is
    // NOT a delivery came from a relay, which only tells us the packet is moving.
    MsgStatus status;
    if (errorReason != (uint8_t)meshtastic_Routing_Error_NONE)
        status = MsgStatus::Failed; // includes MAX_RETRANSMIT
    else if (ok)
        status = MsgStatus::Delivered;
    else
        status = MsgStatus::Sent; // relayed, not yet delivered

    // ChatStore never moves a message backwards through the MsgStatus ordering,
    // so a late relay ack cannot demote an already-Delivered message.
    if (!chatStore.updateStatus(pt.thread, packetId, status))
        LOG_WARN("MeshBridge: status patch failed for id=0x%08x", (unsigned)packetId);

    postMsgEvent(EventType::MessageStatus, pt.thread, packetId, myNodeNum(), status, true);

    // Keep the slot alive after a relay ack: the destination's own ack may still
    // be on its way. Retire it once the outcome is final.
    if (status != MsgStatus::Sent)
        pt.packetId = 0;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

SendResult MeshBridge::send(const OutgoingMessage &msg)
{
    SendResult result;

    // MESH TASK ONLY. generatePacketId() below bumps an unguarded static, and
    // resolve()/me() walk NodeDB's unguarded vector. Calling this from the UI
    // task is a data race, not merely a slow path.
    if (!gStarted || !router || !service || !nodeDB) {
        result.error = "mesh not ready";
        return result;
    }
    if (!msg.text || !msg.text[0]) {
        result.error = "empty message";
        return result;
    }

    const size_t len = strlen(msg.text);
    if (len > maxTextLen(msg.thread)) {
        result.error = "message too long";
        return result;
    }

    const uint32_t dest = msg.thread.direct ? msg.thread.peer : (uint32_t)NODENUM_BROADCAST;
    if (msg.thread.direct && (dest == 0 || dest == (uint32_t)NODENUM_BROADCAST)) {
        result.error = "bad destination";
        return result;
    }
    if (!msg.thread.direct && !channelUsable(msg.thread.channel)) {
        result.error = "channel disabled";
        return result;
    }

    // For a DM we leave channel 0 and let Router::sendLocal() substitute the
    // channel we last heard that node on (getEffectiveChannelIndex). Setting it
    // ourselves would override that with a guess.
    const uint8_t channelIdx = msg.thread.direct ? 0 : msg.thread.channel;

    // The id is minted before anything else so the ChatStore record and the
    // packet share it, which is what lets a routing ack find the record later.
    const uint32_t packetId = generatePacketId();

    // Store FIRST, with Queued. The message shows up in the conversation the
    // instant the user hits send, rather than after the radio gets around to it.
    const SenderIdentity self = me();

    ChatMessage m;
    m.packetId = packetId;
    m.from = myNodeNum();
    m.to = dest;
    m.rxTime = getValidTime(RTCQualityDevice);
    m.uptimeMs = millis();
    m.channel = channelIdx;
    m.status = MsgStatus::Queued;
    m.flags = (uint8_t)(kFlagOutbound | (msg.wantAck ? kFlagWantAck : 0));
    copyField(m.senderShort, sizeof(m.senderShort), self.shortName, sizeof(self.shortName));
    copyField(m.senderLong, sizeof(m.senderLong), self.longName, sizeof(self.longName));
    memcpy(m.text, msg.text, len);
    m.text[len] = 0;
    m.textLen = (uint16_t)len;

    if (!chatStore.append(msg.thread, m))
        LOG_ERROR("MeshBridge: chat append failed for outgoing id=0x%08x", (unsigned)packetId);

    postMsgEvent(EventType::MessageReceived, msg.thread, packetId, m.from, MsgStatus::Queued, true);

    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        // Pool exhausted. The record already exists, so patch it rather than
        // leaving a message stuck on Queued forever.
        chatStore.updateStatus(msg.thread, packetId, MsgStatus::Failed);
        postMsgEvent(EventType::MessageStatus, msg.thread, packetId, m.from, MsgStatus::Failed, true);
        result.error = "packet pool empty";
        return result;
    }

    p->id = packetId;
    p->to = dest;
    p->channel = channelIdx;
    p->want_ack = msg.wantAck;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.payload.size = (uint16_t)len;
    memcpy(p->decoded.payload.bytes, msg.text, len);
    // p->pki_encrypted is deliberately NOT set. Router::perhapsEncode applies PKC
    // to unicast text on its own when the peer's key is known; forcing the flag
    // turns "no key yet" into a hard send failure instead of a plaintext fallback.

    notePending(packetId, dest, msg.thread);

    // sendToMesh ALWAYS consumes p -- it is released to the pool inside, whether
    // the send succeeded or not. Nothing may touch p after this line, which is
    // why packetId was captured above.
    //
    // ccToPhone = true so the message lands in the official phone app's thread
    // for this conversation; without it the phone never sees what we sent.
    service->sendToMesh(p, RX_SRC_LOCAL, true);
    p = nullptr;

    mTxCount++;
    result.accepted = true;
    result.packetId = packetId;
    return result;
}

uint16_t MeshBridge::maxTextLen(const ThreadId &) const
{
    // Same cap for both thread kinds. PKI's cost is in the packet header (the
    // public key and the larger MIC), not in the Data payload, so a PKI DM still
    // carries a full DATA_PAYLOAD_LEN of text here.
    return kPayloadCap;
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

SenderIdentity MeshBridge::resolve(uint32_t nodeNum) const
{
    SenderIdentity out;
    out.num = nodeNum;

    // NodeInfoLite is FLATTENED in this tree: no `user` sub-struct, and presence
    // is a bit in `bitfield` rather than a has_user flag.
    const meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode((NodeNum)nodeNum) : nullptr;
    if (nodeInfoLiteHasUser(node)) {
        // long_name is char[25] here but char[40] in meshtastic_User and in our
        // snapshot, so the bound comes from the source field, not the destination.
        copyField(out.shortName, sizeof(out.shortName), node->short_name, sizeof(node->short_name));
        copyField(out.longName, sizeof(out.longName), node->long_name, sizeof(node->long_name));
        out.known = out.shortName[0] || out.longName[0];
    }

    // Never return an empty string: the UI renders these directly, and a blank
    // sender is worse than a synthesised one. shortName holds four characters, so
    // it gets the low half of the node number; longName gets the full "!a1b2c3d4".
    if (!out.shortName[0])
        snprintf(out.shortName, sizeof(out.shortName), "%04x", (unsigned)(nodeNum & 0xffff));
    if (!out.longName[0])
        snprintf(out.longName, sizeof(out.longName), "!%08x", (unsigned)nodeNum);

    return out;
}

uint32_t MeshBridge::myNodeNum() const
{
    return nodeDB ? (uint32_t)nodeDB->getNodeNum() : 0;
}

SenderIdentity MeshBridge::me() const
{
    SenderIdentity out = resolve(myNodeNum());

    // `owner` is authoritative for us and is populated before our own NodeDB
    // record is, so prefer it when it has anything to say.
    if (owner.short_name[0])
        copyField(out.shortName, sizeof(out.shortName), owner.short_name, sizeof(owner.short_name));
    if (owner.long_name[0]) {
        copyField(out.longName, sizeof(out.longName), owner.long_name, sizeof(owner.long_name));
        out.known = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

void MeshBridge::refreshNodes()
{
    // MESH TASK ONLY: nodeDB->meshNodes is an unguarded vector.
    if (!nodeDB)
        return;

    const uint32_t self = myNodeNum();

    // Build into locals first, then publish under the seqlock, so the window in
    // which a reader can see a torn snapshot is one memcpy long rather than a
    // whole NodeDB walk.
    static NodeBrief staging[kNodeSnapshotMax];
    uint16_t count = 0;

    const size_t total = nodeDB->numMeshNodes;
    for (size_t i = 0; i < total; i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || !n->num)
            continue;

        NodeBrief b;
        b.num = n->num;
        b.lastHeard = n->last_heard;
        b.hopsAway = n->has_hops_away ? n->hops_away : 0;
        b.snr = n->snr;

        uint8_t flags = 0;
        if (n->num == self)
            flags |= kNodeSelf;
        if (nodeInfoLiteViaMqtt(n))
            flags |= kNodeViaMqtt;
        if (nodeInfoLiteIsFavorite(n))
            flags |= kNodeFavorite;
        if (n->has_hops_away)
            flags |= kNodeHopsKnown;
        if (nodeInfoLiteHasSnr(n))
            flags |= kNodeSnrKnown;
        b.flags = flags;

        // Bounded by the SOURCE field width: long_name is char[25] here and
        // char[40] in our snapshot, so sizeof(dst) is the wrong bound.
        if (nodeInfoLiteHasUser(n)) {
            copyField(b.shortName, sizeof(b.shortName), n->short_name, sizeof(n->short_name));
            copyField(b.longName, sizeof(b.longName), n->long_name, sizeof(n->long_name));
        }
        if (!b.shortName[0])
            snprintf(b.shortName, sizeof(b.shortName), "%04x", (unsigned)(b.num & 0xffff));
        if (!b.longName[0])
            snprintf(b.longName, sizeof(b.longName), "!%08x", (unsigned)b.num);

        if (count < kNodeSnapshotMax) {
            staging[count++] = b;
            continue;
        }

        // Full. Keep the most recently heard, so a mesh larger than the buffer
        // degrades into "the nodes you actually care about" rather than "the
        // first 48 NodeDB happens to hold".
        uint16_t oldest = 0;
        for (uint16_t j = 1; j < count; j++)
            if (staging[j].lastHeard < staging[oldest].lastHeard)
                oldest = j;
        if (b.lastHeard > staging[oldest].lastHeard)
            staging[oldest] = b;
    }

    std::sort(staging, staging + count,
              [](const NodeBrief &a, const NodeBrief &b) { return a.lastHeard > b.lastHeard; });

    // Publish. Odd generation == write in progress.
    gNodeGen.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    memcpy(gNodes, staging, sizeof(NodeBrief) * count);
    gNodeCount = count;
    std::atomic_thread_fence(std::memory_order_release);
    gNodeGen.fetch_add(1, std::memory_order_acq_rel);
}

size_t MeshBridge::listNodes(NodeBrief *out, size_t max) const
{
    if (!out || !max)
        return 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t before = gNodeGen.load(std::memory_order_acquire);
        if (before & 1u)
            continue; // a write is in flight; try again

        size_t n = gNodeCount;
        if (n > max)
            n = max;
        memcpy(out, gNodes, sizeof(NodeBrief) * n);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (gNodeGen.load(std::memory_order_acquire) == before)
            return n;
    }

    // Four collisions in a row means the mesh task is republishing continuously,
    // which it cannot do -- the observer is throttled. Report nothing rather
    // than hand back a torn read.
    return 0;
}

uint16_t MeshBridge::nodeCount() const
{
    const uint16_t n = gNodeCount;
    return n > kNodeSnapshotMax ? (uint16_t)kNodeSnapshotMax : n;
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

uint8_t MeshBridge::channelCount() const
{
    return (uint8_t)channels.getNumChannels();
}

const char *MeshBridge::channelName(uint8_t index) const
{
    if (index >= MAX_NUM_CHANNELS)
        return "";
    // Channels::getName is bounds-safe and synthesises a name from the modem
    // preset when the channel has none set. It never returns null.
    const char *name = channels.getName(index);
    return name ? name : "";
}

bool MeshBridge::channelUsable(uint8_t index) const
{
    if (index >= channels.getNumChannels())
        return false;
    return channels.getByIndex(index).role != meshtastic_Channel_Role_DISABLED;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

size_t MeshBridge::listThreads(ThreadSummary *out, size_t max)
{
    if (!out || !max)
        return 0;

    // `out` doubles as the scratch buffer: ChatStore fills it with every thread
    // that has history, and the channel merge happens in place afterwards. A
    // separate temp array of ThreadSummary would be several KB of mesh-task stack
    // for no benefit.
    size_t count = chatStore.listThreads(out, max);

    // Drop broadcast history for channels that are no longer enabled -- the list
    // is "every enabled channel plus every DM with history", and a stale channel
    // the user cannot post to is not one of them.
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (!out[i].id.direct && !channelUsable(out[i].id.channel))
            continue;
        if (kept != i)
            out[kept] = out[i];
        kept++;
    }
    count = kept;

    // Add every enabled channel that had no history to contribute. If the caller
    // gave us a buffer too small to hold them all, the empty channels are what
    // gets dropped -- threads with real activity always survive.
    const uint8_t nch = channelCount();
    for (uint8_t ch = 0; ch < nch && count < max; ch++) {
        if (!channelUsable(ch))
            continue;
        bool present = false;
        for (size_t i = 0; i < count && !present; i++)
            present = (!out[i].id.direct && out[i].id.channel == ch);
        if (present)
            continue;

        ThreadSummary s;
        s.id = ThreadId::broadcast(ch);
        s.lastActivity = 0; // no history: sorts to the bottom
        threadTitle(s.id, s.title, sizeof(s.title));
        out[count++] = s;
    }

    std::sort(out, out + count, [](const ThreadSummary &a, const ThreadSummary &b) { return a.lastActivity > b.lastActivity; });
    return count;
}

void MeshBridge::threadTitle(const ThreadId &thread, char *out, size_t outLen) const
{
    if (!out || !outLen)
        return;

    if (!thread.direct) {
        snprintf(out, outLen, "%s", channelName(thread.channel));
        if (!out[0])
            snprintf(out, outLen, "Channel %u", (unsigned)thread.channel);
        return;
    }

    // resolve() already falls back to "!a1b2c3d4", so this cannot come back blank
    // even for a peer NodeDB has forgotten.
    const SenderIdentity id = resolve(thread.peer);
    snprintf(out, outLen, "%s", id.longName);
}

} // namespace pgros

#endif // PGROS
