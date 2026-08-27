#pragma once
//
// The service task: how the UI asks for things it is not allowed to do itself.
//
// The UI task may not block, and it may not touch Meshtastic's mutable state.
// Both restrictions are real, not stylistic:
//
//   * `generatePacketId()` keeps an unguarded `static` counter.
//   * `NodeDB::meshNodes` is an unguarded `std::vector` that REALLOCATES on
//     insert, invalidating any pointer another task is holding.
//   * `coex.request()` can take hundreds of milliseconds, or reboot the device.
//   * `wifi.scanStart()` / `join()` block for seconds.
//
// So the UI never calls those directly. It posts an Intent -- a small POD, same
// discipline as the event bus in the other direction -- and this drains the
// queue from an OSThread running on the Meshtastic main task, where all of the
// above is safe. Results come back as events.
//
//      UI task  --Intent-->  service queue  --drained on main task-->  work
//         ^                                                             |
//         +---------------------- Event -------------------------------+
//
// This is the same shape Meshtastic itself uses for cross-task work; see the
// comment block above BluetoothPhoneAPI in src/nimble/NimbleBluetooth.cpp,
// which explains why the rest of the codebase gets to stay single-threaded.

#include "core/EventBus.h"
#include "store/ChatStore.h"
#include <stdint.h>

namespace pgros {

enum class IntentType : uint8_t {
    None = 0,
    SendText,      // text.* -- send a message
    RetrySend,     // resend a failed message by packet id
    MarkThreadRead,
    SetRadioMode,  // radio.mode -- may reboot; see RadioCoex
    WifiScan,
    WifiJoin,      // wifi.ssid + wifi.psk
    WifiForget,    // wifi.ssid, or empty for all
    PortalStart,
    PortalStop,
    ApplyNodeConfig, // node.* -- one Meshtastic node setting
    Discover,        // ask nearby nodes to identify themselves
    SavePolicy,    // flush the debounced policy to flash
    CompactStore,  // housekeeping; runs off the UI task
    Reboot,
};

// Fixed-size intent. Text is copied inline -- no pointers into UI memory, for
// the same reason events carry none: the UI may have moved on by the time this
// is drained.
struct Intent {
    IntentType type = IntentType::None;

    ThreadRef thread{0, 0, 0};
    uint32_t packetId = 0;

    union {
        struct {
            uint8_t mode; // CoexMode
        } radio;

        struct {
            char ssid[33];
            char psk[64];
        } wifi;

        struct {
            uint16_t len;
            char body[kMaxTextLen + 1];
        } text;

        struct {
            uint8_t field;  // MeshBridge::NodeField
            int32_t value;
            char text[40];
        } node;

        uint8_t raw[300];
    };

    Intent() : raw{} {}
};

class Service
{
  public:
    // Creates the intent queue and the draining OSThread. Must be called after
    // setup() has begun -- Meshtastic asserts that OSThreads are never
    // constructed statically.
    bool begin();

    // Post an intent. Safe from the UI task. Never blocks; returns false if the
    // queue is full (and counts the drop). A dropped intent is a real failure
    // the user should hear about, unlike a dropped status event -- callers
    // should surface it.
    bool post(const Intent &in);

    // --- convenience wrappers, all safe from the UI task -------------------
    bool sendText(const ThreadId &thread, const char *text);
    bool retrySend(const ThreadId &thread, uint32_t packetId);
    bool markRead(const ThreadId &thread);
    bool setRadioMode(uint8_t coexMode);
    bool wifiScan();
    bool wifiJoin(const char *ssid, const char *psk);
    bool wifiForget(const char *ssid);
    bool portalStart();
    bool portalStop();
    bool applyNodeConfig(uint8_t field, int32_t value, const char *text);
    bool discover();
    bool savePolicy();
    bool reboot();

    uint32_t dropped() const { return mDropped; }

    // Drains and executes queued intents. Called from the OSThread on the main
    // task. Processes a bounded number per tick so a burst cannot stall the
    // mesh loop.
    void drain();

    static constexpr uint8_t kMaxPerTick = 4;

  private:
    void execute(const Intent &in);

    void *mQueue = nullptr; // QueueHandle_t
    volatile uint32_t mDropped = 0;
};

extern Service service_;

// Converts between the POD ThreadRef used on the wire between tasks and the
// richer ThreadId used by the store.
ThreadId toThreadId(const ThreadRef &r);
ThreadRef toThreadRef(const ThreadId &t);

} // namespace pgros
