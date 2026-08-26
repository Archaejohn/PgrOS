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
#include <stdint.h>

namespace pgros {

// Identity resolved from NodeDB at receive time.
struct SenderIdentity {
    uint32_t num = 0;
    char shortName[kMaxShortName] = {0};
    char longName[kMaxLongName] = {0};
    bool known = false; // false when NodeDB has no record; names are synthesised
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

    // --- diagnostics -----------------------------------------------------

    uint32_t messagesReceived() const { return mRxCount; }
    uint32_t messagesSent() const { return mTxCount; }

  private:
    // Called by the module shim when a text packet arrives.
    void onTextMessage(const void *packet);
    // Called when a routing ack/nak arrives for one of our packets.
    void onRouting(uint32_t packetId, bool ok, uint8_t errorReason);

    uint32_t mRxCount = 0;
    uint32_t mTxCount = 0;

    friend class PgrosTextModule;
    friend class PgrosRoutingObserver;
};

extern MeshBridge mesh;

} // namespace pgros
