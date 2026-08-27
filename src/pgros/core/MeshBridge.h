#pragma once
//
// The bridge between Meshtastic's mesh stack and PgrOS.
//
// This is the ONLY place PgrOS touches Router/NodeDB/Channels. Everything above
// it (the UI, the web portal) talks to chat history and the event bus instead,
// which is what keeps the UI task off NodeDB's data structures while the mesh
// task is mutating them.
//
// Responsibilities, in order of importance:
//
//   1. Receive TEXT_MESSAGE_APP packets, resolve the sender to a name RIGHT NOW
//      while NodeDB is authoritative, and append the message plus that name
//      snapshot to ChatStore. This is what makes channel history attributable
//      after a node ages out.
//   2. Send text messages, and track their delivery state through the routing
//      acks so the UI can show sent/delivered/failed.
//   3. Mirror node, channel, position and power changes onto the event bus.
//
// Everything here runs on the Meshtastic main task.

#include "core/EventBus.h"
#include "store/ChatStore.h"
#include "store/TrackStore.h"
#include <stdint.h>

namespace pgros {

// Identity resolved from NodeDB at receive time.
struct SenderIdentity {
    uint32_t num = 0;
    char shortName[kMaxShortName] = {0};
    char longName[kMaxLongName] = {0};
    bool known = false; // false when NodeDB has no record; names are synthesised
};

// One node as the Contacts list renders it. A frozen snapshot, never a pointer
// into NodeDB: the UI task reads these long after the mesh task has moved on,
// and meshNodes is an unguarded vector that reallocates on insert.
struct NodeBrief {
    uint32_t num = 0;
    uint32_t lastHeard = 0; // epoch seconds; 0 when never heard / clock unset
    float snr = 0.0f;
    uint8_t hopsAway = 0;
    uint8_t flags = 0; // NodeBriefFlags
    char shortName[kMaxShortName] = {0};
    char longName[kMaxLongName] = {0};
};

enum NodeBriefFlags : uint8_t {
    kNodeSelf = 1 << 0,
    kNodeViaMqtt = 1 << 1,
    kNodeFavorite = 1 << 2,
    kNodeHopsKnown = 1 << 3, // hopsAway is meaningful rather than defaulted
    kNodeSnrKnown = 1 << 4,
};

// A message the UI wants to send.
struct OutgoingMessage {
    ThreadId thread;
    const char *text = nullptr; // NUL-terminated, copied immediately
    bool wantAck = true;
};

// Result of a send attempt, returned synchronously. Delivery is reported later
// via EventType::MessageStatus.
struct SendResult {
    bool accepted = false;
    uint32_t packetId = 0;
    const char *error = ""; // set when accepted == false
};

class MeshBridge
{
  public:
    // Registers the module and observers. Called from setup() after NodeDB and
    // the router exist, but it must not block.
    bool begin();

    // --- sending ---------------------------------------------------------

    // Queue a text message. Appends it to ChatStore immediately with status
    // Queued so it appears in the UI without waiting for the radio, then hands
    // it to the router. Safe to call from the service task, NOT the UI task.
    SendResult send(const OutgoingMessage &msg);

    // Maximum text length that will fit in one packet for this thread. DMs with
    // PKI carry a smaller payload than plaintext broadcast, and the composer
    // needs to know so it can show an accurate character counter.
    uint16_t maxTextLen(const ThreadId &thread) const;

    // --- identity --------------------------------------------------------

    // Resolve a node number to names. Falls back to a synthesised "!a1b2c3d4"
    // short name when NodeDB has no record, so the UI never renders blank.
    SenderIdentity resolve(uint32_t nodeNum) const;

    // Our own node number and names.
    uint32_t myNodeNum() const;
    SenderIdentity me() const;

    // --- channels --------------------------------------------------------

    // Number of configured channels.
    uint8_t channelCount() const;

    // Channel display name for `index`. Returns the well-known name for the
    // primary channel when it has no explicit name set. Never returns null.
    const char *channelName(uint8_t index) const;

    // True if the channel is enabled and usable for sending.
    bool channelUsable(uint8_t index) const;

    // --- threads ---------------------------------------------------------

    // Build the thread list the Messages app shows: every enabled channel, plus
    // every DM thread that has history. Returns the count written.
    size_t listThreads(ThreadSummary *out, size_t max);

    // Human-readable title for a thread (channel name, or peer long name).
    void threadTitle(const ThreadId &thread, char *out, size_t outLen) const;

    // --- nodes -----------------------------------------------------------
    //
    // The Contacts screen needs a node list, and walking NodeDB from the UI
    // task is exactly the race this class exists to prevent. So the mesh task
    // publishes a snapshot instead: refreshNodes() runs on the main task (from
    // begin(), from inbound text, and from the NodeDB change observer, which is
    // already throttled), and listNodes() hands the UI task a copy of it.
    //
    // The handoff is a seqlock: the writer bumps a generation counter either
    // side of the copy and the reader retries if it moved. That is enough for
    // two tasks on a cache-coherent dual-core part, and it costs the writer
    // nothing in the common case where nobody is reading.

    // Rebuild the snapshot from NodeDB. MESH TASK ONLY.
    void refreshNodes();

    // Copy up to `max` nodes, most recently heard first. Safe from any task.
    size_t listNodes(NodeBrief *out, size_t max) const;

    // Number of nodes in the snapshot (capped at kNodeSnapshotMax).
    uint16_t nodeCount() const;

    // Deep enough that a pager's Contacts list never feels truncated, small
    // enough that the snapshot is not a meaningful slice of internal RAM.
    static constexpr size_t kNodeSnapshotMax = 48;

    // --- node settings ----------------------------------------------------
    //
    // The small, curated set of Meshtastic node config PgrOS edits on-device.
    // The phone app remains the better place for the rest; this is for when you
    // are out with no phone. Enum vocabularies come from MeshSettingsTable.h,
    // generated from the vendored protobufs so they cannot drift from upstream.
    //
    // MESH TASK ONLY -- these write `owner` and `config` and call into
    // MeshService, none of which is safe from the UI task.

    enum class NodeField : uint8_t {
        LongName = 0,
        ShortName,
        Role,
        Region,
        ModemPreset,
        HopLimit,
        BtPairing,
        Count
    };

    // Applies one setting and persists it. Returns true if the change needs a
    // reboot before it takes effect -- radio and Bluetooth settings do, because
    // the stacks are configured once at startup.
    bool applyNodeConfig(NodeField f, int32_t value, const char *text);

    // Current value, for the UI to render and step from.
    int32_t nodeConfigValue(NodeField f) const;

    // Current text, for the two name fields. Always NUL-terminates.
    void nodeConfigText(NodeField f, char *out, size_t outLen) const;

    // --- mesh density -----------------------------------------------------
    //
    // Signal-bar equivalent for the mesh rather than for one link. Most of what
    // a node hears is traffic it merely relays -- silent from the user's point of
    // view -- and that traffic is exactly what says how alive the mesh is around
    // you. Counted promiscuously, including packets we cannot decrypt.
    struct MeshDensity {
        uint8_t directNeighbours; // distinct nodes heard at zero hops, recently
        uint16_t nodesRecent;     // nodes heard at all, recently
        uint16_t packetsPerMin;   // everything on the air we could hear
        uint8_t utilizationPct;   // channel airtime over the last minute
        int8_t bestRssi;          // strongest DIRECT packet in the window, dBm
        int8_t bestSnr;           // its SNR, dB (rounded)
        bool heardDirect;         // false when bestRssi/bestSnr mean nothing
        uint8_t bars;             // 0..4, for the status bar
    };

    // Safe from any task: plain integer reads of counters the mesh task writes.
    MeshDensity density() const;

    // A node counts as "recent" for density purposes for this long.
    static constexpr uint32_t kDensityRecentSecs = 30 * 60;

    // --- discovery --------------------------------------------------------
    //
    // Broadcasts our NodeInfo with wantReplies set, which asks everyone in
    // earshot to identify themselves. Replies arrive as ordinary NodeInfo
    // packets, so the contact list refreshes itself through the usual
    // NodeUpdated path -- there is no separate result set to collect.
    //
    // MESH TASK ONLY.
    bool startDiscovery();

    // Milliseconds left in the current discovery window, 0 when idle. Safe from
    // any task: it is one integer read.
    uint32_t discoveryRemainingMs() const;

    // How long to keep saying "listening". Replies trickle in over several
    // seconds on a busy mesh because everyone staggers their transmit.
    static constexpr uint32_t kDiscoveryWindowMs = 30000;

    // --- read receipts ----------------------------------------------------
    //
    // PgrOS-private, sent on PortNum PRIVATE_APP so any node that does not speak
    // it simply ignores the packet -- including the official phone apps, which
    // stay unaffected. Only ever sent for direct threads: a read receipt to a
    // broadcast channel would be both meaningless and rude on shared airtime.
    //
    // MESH TASK ONLY.
    void sendReadReceipt(const ThreadId &thread, const uint32_t *packetIds, size_t count);

    // Largest number of ids one receipt carries. Beyond this the oldest are
    // dropped: the newest read marker implies the older ones anyway.
    static constexpr size_t kMaxReceiptIds = 8;

    // --- diagnostics -----------------------------------------------------

    uint32_t messagesReceived() const { return mRxCount; }
    uint32_t messagesSent() const { return mTxCount; }

  private:
    // Called by the module shim when a text packet arrives.
    void onTextMessage(const void *packet);
    // Called when a routing ack/nak arrives for one of our packets.
    void onRouting(uint32_t packetId, bool ok, uint8_t errorReason);

    uint32_t mDiscoveryUntilMs = 0;
    uint32_t mRxCount = 0;
    uint32_t mTxCount = 0;

    friend class PgrosTextModule;
    friend class PgrosRoutingObserver;
};

extern MeshBridge mesh;

} // namespace pgros
