#pragma once
//
// PgrOS persistent chat storage.
//
// Chat history is an append-only log on LittleFS, one file per thread. It is
// append-only in the strict sense: nothing already written is ever rewritten,
// including delivery-status changes, which are appended as separate patch
// records and folded in at read time. That property is what makes the store
// safe against power loss at any instant -- the worst case is a torn record at
// EOF, which is detected and truncated on the next open.
//
// Records carry their length at BOTH ends, so the file can be walked backwards
// from EOF. Opening a conversation therefore costs O(messages shown), not
// O(history), which is what keeps the messages app fast no matter how deep the
// history gets.
//
// Every message record freezes a snapshot of the sender's short and long name
// alongside the node number. Names are resolved once, at receive time. History
// stays attributable after a node ages out of NodeDB, and rendering a thread
// never has to touch NodeDB (and so never has to lock against the mesh task).

#include <stddef.h>
#include <stdint.h>

namespace pgros {

// Meshtastic's maximum data payload. Text messages cannot exceed this.
static constexpr size_t kMaxTextLen = 237;
static constexpr size_t kMaxShortName = 5;  // 4 chars + NUL
static constexpr size_t kMaxLongName = 40;  // truncated snapshot + NUL

// Delivery state as shown in the UI. Ordering matters: a status patch only ever
// moves a message forward, so an out-of-order ack cannot regress "Delivered"
// back to "Sent".
enum class MsgStatus : uint8_t {
    Unknown = 0,
    Composing = 1, // drafted locally, not yet handed to the mesh
    Queued = 2,    // accepted by the router, awaiting airtime
    Sent = 3,      // transmitted
    Delivered = 4, // acknowledged
    Failed = 5,    // routing error, or retransmits exhausted
    Received = 6,  // inbound message
    Read = 7,      // recipient's device reported it read (PgrOS read receipt)
};

enum MsgFlags : uint8_t {
    kFlagNone = 0,
    kFlagOutbound = 1 << 0, // we sent it
    kFlagWantAck = 1 << 1,
    kFlagPki = 1 << 2,       // sent/received with PKI encryption
    kFlagEmergency = 1 << 3, // emergency channel/alert
    kFlagUnread = 1 << 4,
};

// A thread is either a broadcast channel or a direct conversation with one node.
struct ThreadId {
    bool direct = false; // false: broadcast on `channel`; true: DM with `peer`
    uint8_t channel = 0; // channel index, valid when !direct
    uint32_t peer = 0;   // peer node number, valid when direct

    static ThreadId broadcast(uint8_t ch)
    {
        ThreadId t;
        t.direct = false;
        t.channel = ch;
        return t;
    }
    static ThreadId dm(uint32_t node)
    {
        ThreadId t;
        t.direct = true;
        t.peer = node;
        return t;
    }
    bool operator==(const ThreadId &o) const
    {
        return direct == o.direct && (direct ? peer == o.peer : channel == o.channel);
    }
};

// One chat message, as handed to and returned from the store.
struct ChatMessage {
    uint32_t packetId = 0; // Meshtastic packet id; 0 if not yet assigned
    uint32_t from = 0;     // sender node number
    uint32_t to = 0;       // destination node number (or broadcast)
    uint32_t rxTime = 0;   // epoch seconds; 0 when the clock was not yet set
    uint32_t uptimeMs = 0; // local millis() at insert, for ordering when rxTime is 0
    uint8_t channel = 0;
    uint8_t hopsAway = 0;
    int8_t rssi = 0;
    float snr = 0.0f;
    MsgStatus status = MsgStatus::Unknown;
    uint8_t flags = kFlagNone;

    // Sender identity, snapshotted at insert time. This is what the UI renders.
    char senderShort[kMaxShortName] = {0};
    char senderLong[kMaxLongName] = {0};

    uint16_t textLen = 0;
    char text[kMaxTextLen + 1] = {0};
};

// Lightweight per-thread summary for the conversation list, without loading
// message bodies.
struct ThreadSummary {
    ThreadId id;
    uint32_t lastActivity = 0; // rxTime of newest record, else uptimeMs
    uint16_t unread = 0;
    char title[kMaxLongName] = {0};    // channel name, or peer long name
    char preview[64] = {0};            // newest message text, truncated
    char lastSenderShort[kMaxShortName] = {0};
    bool lastWasOutbound = false;
};

// Result of opening/validating a thread file.
struct StoreStats {
    uint32_t records = 0;
    uint32_t bytes = 0;
    uint32_t truncatedBytes = 0; // torn tail discarded on open, 0 when clean
    bool recovered = false;      // true if a torn tail was repaired
};

class ChatStore
{
  public:
    // Mounts the store. Safe to call once, early, before the mesh is up.
    // Creates /pgros/ch and /pgros/dm if missing. Does NOT scan every thread --
    // that work is deferred to first use so it never sits on the boot path.
    bool begin();

    // True once begin() has succeeded.
    bool ready() const { return mReady; }

    // Append a message. Fills in uptimeMs if the caller left it zero.
    // Returns false only on a real write failure (full disk, FS error).
    bool append(const ThreadId &thread, const ChatMessage &msg);

    // Append a status patch for an earlier message. Folded in by readTail().
    // Never moves a message backwards through the MsgStatus ordering.
    bool updateStatus(const ThreadId &thread, uint32_t packetId, MsgStatus status);

    // Mark every message in a thread as read (appends one patch record).
    bool markThreadRead(const ThreadId &thread);

    // Read up to `max` of the newest messages, walking backwards from EOF and
    // applying any status patches found along the way. Results are written to
    // `out` in chronological order (oldest first), which is the order the UI
    // renders. Returns the number written.
    size_t readTail(const ThreadId &thread, ChatMessage *out, size_t max);

    // Read `max` messages older than `beforeUptimeMs`/`beforePacketId`, for
    // infinite-scroll upward paging. Chronological order, oldest first.
    size_t readBefore(const ThreadId &thread, uint32_t beforeUptimeMs, ChatMessage *out, size_t max);

    // Summarise one thread without loading bodies. Cheap: reads only the tail.
    bool summarise(const ThreadId &thread, ThreadSummary &out);

    // Enumerate every thread that has a log file. Writes up to `max` summaries,
    // newest activity first. Returns the count written.
    size_t listThreads(ThreadSummary *out, size_t max);

    // Unread count for a thread, from the tail only.
    uint16_t unreadCount(const ThreadId &thread);

    // Validate and, if needed, repair a thread file. Called implicitly on first
    // access; exposed for a Settings "verify storage" action.
    bool verify(const ThreadId &thread, StoreStats &stats);

    // Rewrite a thread keeping only the newest `keepRecords` messages, by
    // writing a new file and renaming over the old one. Never edits in place.
    bool compact(const ThreadId &thread, uint32_t keepRecords);

    // Compact any thread whose file exceeds kCompactThresholdBytes. Intended to
    // be called from an idle/background task, never from the UI task.
    void compactIfNeeded();

    // Delete a thread's history entirely.
    bool erase(const ThreadId &thread);

    // Total bytes used by all chat logs.
    uint32_t bytesUsed();

    // Size above which a thread is compacted, and how many records survive.
    static constexpr uint32_t kCompactThresholdBytes = 192 * 1024;
    static constexpr uint32_t kCompactKeepRecords = 500;

  private:
    bool mReady = false;

    // Builds "/pgros/ch/3.log" or "/pgros/dm/a1b2c3d4.log" into `out`.
    static void pathFor(const ThreadId &thread, char *out, size_t outLen);
};

// Process-wide instance. Owned by core/Boot.
extern ChatStore chatStore;

} // namespace pgros
