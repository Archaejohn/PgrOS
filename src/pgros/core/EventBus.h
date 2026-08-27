#pragma once
//
// PgrOS event bus.
//
// The only sanctioned channel between the Meshtastic side of the firmware
// (running on the main task) and the LVGL UI task. Producers post small,
// trivially-copyable PODs; the UI task drains the queue once per frame and
// mutates its own state from them.
//
// Two rules make this safe, and both are load-bearing:
//
//   1. Events never contain pointers into someone else's memory. Anything the
//      UI needs -- node names, channel names, message text -- is COPIED into
//      the event at post time. The UI task must never dereference NodeDB or a
//      packet buffer, because the mesh task may be mutating or freeing it.
//
//   2. post() is safe from any task and from an ISR, and it never blocks. If
//      the queue is full the event is dropped and counted. Dropping a status
//      update is always better than stalling the radio task.
//
// LVGL calls are legal ONLY on the UI task. Nothing in this header touches LVGL.

#include <stdint.h>

namespace pgros {

enum class EventType : uint8_t {
    None = 0,

    // --- boot / lifecycle -------------------------------------------------
    BootStage,   // boot.stage advanced; boot.percent for the progress bar
    SubsysReady, // subsys.id came up (radio, gps, fs, ble...)

    // --- messaging --------------------------------------------------------
    MessageReceived, // a text message was stored; msg.* identifies it
    MessageStatus,   // delivery state changed for msg.packetId
    ThreadRead,      // a thread was marked read (badge refresh)

    // --- mesh -------------------------------------------------------------
    NodeUpdated,     // node.num's position/name/battery changed
    ChannelsChanged, // channel set was edited (via phone app or UI)

    // --- sensors / power --------------------------------------------------
    GpsFix,       // gps.* valid; fixValid distinguishes fix from loss
    PowerChanged, // power.* battery state

    // --- radios -----------------------------------------------------------
    RadioState,    // coexistence state machine moved; radio.state
    BlePairing,    // SHOW THE PASSKEY: ble.passkey is a 6-digit code
    BlePairingEnd, // pairing completed or aborted; dismiss the modal
    BleConnection, // ble.connected
    WifiState,     // wifi.state
    WifiScanDone,  // wifi.count networks available from WifiManager

    // --- ui ---------------------------------------------------------------
    Notification, // transient toast; note.* carries a short text
};

// Which subsystem reported ready. Also used for status-bar icons.
enum class Subsys : uint8_t { Fs = 0, NodeDb, Radio, Gps, Ble, Wifi, Store, Count };

// Coexistence state. Mirrors net::RadioCoex::State; duplicated here so this
// header stays free of net/ dependencies.
enum class RadioMode : uint8_t { Off = 0, Bluetooth, WifiStation, WifiAp };

// FailedSaved is kept distinct from Failed on purpose: it is the one failure a
// retry cannot fix. The stored password is wrong, so the user has to forget the
// network and type it again, and the UI has to say so.
enum class WifiState : uint8_t { Idle = 0, Scanning, Connecting, Connected, ApRunning, Failed, FailedSaved };

// Plain thread reference. Deliberately not store::ThreadId -- this must stay a
// trivially-copyable POD so it can live inside the event union.
struct ThreadRef {
    uint8_t direct;  // 0 = broadcast channel, 1 = direct message
    uint8_t channel; // valid when direct == 0
    uint32_t peer;   // valid when direct == 1
};

// Fixed-size event. Keep every member trivially copyable: no constructors, no
// NSDMIs inside the union, no std::string, no pointers to shared state.
struct Event {
    EventType type;
    uint32_t atMs; // millis() at post time

    union {
        struct {
            uint8_t stage;   // 0..kBootStageCount
            uint8_t percent; // 0..100
        } boot;

        struct {
            uint8_t id; // Subsys
            uint8_t ok; // 0 = failed to init, 1 = ready
        } subsys;

        struct {
            ThreadRef thread;
            uint32_t packetId;
            uint32_t from;
            uint8_t status;   // MsgStatus
            uint8_t outbound; // 0/1
        } msg;

        struct {
            uint32_t num;
            uint8_t viaMqtt;
            uint8_t hopsAway;
        } node;

        struct {
            int32_t latI;   // degrees * 1e7
            int32_t lonI;   // degrees * 1e7
            int32_t altM;   // metres
            uint8_t sats;   // satellites in view
            uint8_t fixValid; // 0 = fix lost, 1 = have fix
        } gps;

        struct {
            uint8_t percent;   // battery charge
            uint16_t millivolts;
            uint8_t charging;  // 0/1
            uint8_t usbPowered; // 0/1
        } power;

        struct {
            uint8_t state; // RadioMode
        } radio;

        struct {
            uint32_t passkey; // 6-digit BLE pairing code, shown to the user
            uint8_t connected;
        } ble;

        struct {
            uint8_t state; // WifiState
            uint8_t count; // networks found, for WifiScanDone
        } wifi;

        struct {
            uint8_t severity; // 0 info, 1 warn, 2 error
            char text[40];    // NUL-terminated, truncated
        } note;

        // Ensures a stable minimum size and makes the union's footprint obvious.
        uint8_t raw[48];
    };
};

static constexpr uint8_t kBootStageCount = 5;

class EventBus
{
  public:
    // Creates the underlying queue. Call once, before any producer runs.
    bool begin(uint16_t depth = 48);

    // Post an event. Never blocks. Returns false if the queue was full (the
    // event is dropped and `dropped()` is incremented). Safe from any task.
    bool post(const Event &ev);

    // ISR-safe variant. Must not be called from a normal task.
    bool postFromIsr(const Event &ev);

    // Receive one event, waiting up to `timeoutMs`. UI TASK ONLY.
    bool receive(Event &out, uint32_t timeoutMs);

    // Non-blocking receive. UI TASK ONLY.
    bool poll(Event &out) { return receive(out, 0); }

    // Number of events dropped because the queue was full. Surfaced in the
    // diagnostics screen -- a nonzero value means the UI task is falling behind
    // and is worth knowing about.
    uint32_t dropped() const { return mDropped; }

    bool ready() const { return mQueue != nullptr; }

  private:
    void *mQueue = nullptr; // QueueHandle_t, kept opaque to avoid a FreeRTOS include here
    volatile uint32_t mDropped = 0;
};

extern EventBus events;

// -------------------------------------------------------------------------
// Small helpers so producers do not hand-roll event structs at every call site.
// All are safe to call from the mesh task.
// -------------------------------------------------------------------------

void postBootStage(uint8_t stage, uint8_t percent);
void postSubsysReady(Subsys id, bool ok);
void postNotification(const char *text, uint8_t severity = 0);
void postBlePairing(uint32_t passkey);
void postRadioState(RadioMode mode);

} // namespace pgros
