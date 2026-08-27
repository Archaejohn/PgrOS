#ifdef PGROS

#include "core/Service.h"

#include "core/Boot.h" // bootComplete()

#include "configuration.h"

#include "core/MeshBridge.h"
#include "core/Policy.h"
#include "net/Portal.h"
#include "net/RadioCoex.h"
#include "net/WifiManager.h"
#include "store/ChatStore.h"

#include "concurrency/OSThread.h"
#include "main.h" // rebootAtMsec

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <string.h>

namespace pgros
{

Service service_;

// Depth 12 is generous for a UI that can only produce intents as fast as a
// human types. If this ever fills, something is wedged on the main task and the
// drop counter is the signal.
static constexpr uint16_t kQueueDepth = 12;

ThreadId toThreadId(const ThreadRef &r)
{
    return r.direct ? ThreadId::dm(r.peer) : ThreadId::broadcast(r.channel);
}

ThreadRef toThreadRef(const ThreadId &t)
{
    ThreadRef r;
    r.direct = t.direct ? 1 : 0;
    r.channel = t.channel;
    r.peer = t.peer;
    return r;
}

// ---------------------------------------------------------------------------
// The draining thread.
//
// This runs in Meshtastic's cooperative mainController, on the main task, which
// is exactly where NodeDB, the router and generatePacketId() are safe to touch.
// runOnce() returns the ms until it wants to run again; 20 ms keeps the UI
// feeling immediate without adding meaningful load.
// ---------------------------------------------------------------------------
class ServiceThread : public concurrency::OSThread
{
  public:
    ServiceThread() : concurrency::OSThread("PgrosService") {}

  protected:
    int32_t runOnce() override
    {
        // The first time this runs we are in loop(), which means setup() has
        // returned and the whole mesh stack is up. That is the signal PgrOS uses
        // to dismiss the boot overlay -- there is no callback from setup() and
        // nothing else knows the moment boot actually finished.
        if (!mAnnouncedBoot) {
            mAnnouncedBoot = true;
            bootComplete();
            restoreRadioMode();
        }

        service_.drain();

        // Housekeeping. ChatStore::compactIfNeeded() only does work once a
        // thread exceeds its threshold, but nothing was ever calling it, so logs
        // grew without bound. Ten minutes is far more often than a pager can
        // fill 192 KB of one conversation.
        const uint32_t now = millis();
        if (now - mLastCompactMs > kCompactIntervalMs) {
            mLastCompactMs = now;
            chatStore.compactIfNeeded();
        }

        // Pump the web server.
        //
        // esp32_https_server is synchronous: it accepts connections and services
        // open ones only when loop() is called, and nothing else in PgrOS drives
        // it. Without this the listening socket is up -- a phone associates and
        // gets an address quite happily -- but no HTTP request is ever read, so
        // the browser just hangs. Meshtastic's own WebServerThread pumps its
        // server the same way, from this same main task.
        if (portal.running()) {
            portal.loop();
            // 20 ms per accept-and-serve step makes page loads feel sluggish;
            // an idle loop() is cheap, so tick faster while anyone might be
            // connected.
            return 5;
        }

        return 20;
    }

  private:
    // Bring the radio back to whatever the user last chose.
    //
    // Meshtastic's own config cannot express "run an access point" -- it has a
    // single wifi_enabled bool and initWifi() only ever does station mode. So a
    // hotspot, which has to restart to release the Bluetooth controller, would
    // otherwise come back as nothing at all and the portal would never start.
    // PgrOS records the real intent in policy and reapplies it here, on the main
    // task, once boot is genuinely finished.
    void restoreRadioMode()
    {
        const auto want = (CoexMode)policy.get().bootRadioMode;
        if (want == CoexMode::Off || want == coex.mode())
            return;

        // Only ever restore a WiFi mode. Bluetooth is already brought up by
        // Meshtastic's PowerFSM when the config says so; doing it here too would
        // fight it.
        if (want != CoexMode::WifiStation && want != CoexMode::WifiAp)
            return;

        LOG_INFO("PgrOS: restoring radio mode %u after boot", (unsigned)want);
        coex.request(want, CoexReason::BootDefault);
    }

    static constexpr uint32_t kCompactIntervalMs = 10 * 60 * 1000;

    bool mAnnouncedBoot = false;
    uint32_t mLastCompactMs = 0;
};

static ServiceThread *serviceThread = nullptr;

bool Service::begin()
{
    if (mQueue)
        return true;

    mQueue = xQueueCreate(kQueueDepth, sizeof(Intent));
    if (!mQueue) {
        LOG_ERROR("PgrOS: service queue alloc failed");
        return false;
    }

    // Must be `new`ed here rather than declared statically: OSThread asserts
    // that instances are constructed after concurrency::hasBeenSetup.
    serviceThread = new ServiceThread();
    LOG_INFO("PgrOS: service task ready");
    return true;
}

bool Service::post(const Intent &in)
{
    if (!mQueue) {
        LOG_WARN("PgrOS: intent %d dropped, service not started", (int)in.type);
        return false;
    }
    if (xQueueSend((QueueHandle_t)mQueue, &in, 0) != pdTRUE) {
        mDropped = mDropped + 1;
        LOG_WARN("PgrOS: intent queue full, dropped type %d (total %u)", (int)in.type, (unsigned)mDropped);
        return false;
    }
    return true;
}

void Service::drain()
{
    if (!mQueue)
        return;

    Intent in;
    // Bounded per tick: a burst of intents must not stall the mesh loop.
    for (uint8_t i = 0; i < kMaxPerTick; ++i) {
        if (xQueueReceive((QueueHandle_t)mQueue, &in, 0) != pdTRUE)
            return;
        execute(in);
    }
}

// Scratch space for the two intents that need to look back through a thread.
//
// This is deliberately NOT on the stack. ChatMessage is ~320 bytes, so even a
// modest window is measured in kilobytes, and execute() runs on the Arduino
// loop task whose stack is 8 KB total -- shared with the LittleFS call chain
// these very intents trigger. A 24-element local array here overflowed that
// stack and corrupted the return address, which surfaced as a LoadProhibited
// panic deep inside esp_flash_read() with a nonsense chip pointer.
//
// Safe as a single shared buffer because execute() is only ever called from
// Service::drain(), which only ever runs on the main task.
static constexpr size_t kScratchMessages = 8;
static ChatMessage sScratch[kScratchMessages];

void Service::execute(const Intent &in)
{
    switch (in.type) {

    case IntentType::SendText: {
        OutgoingMessage out;
        out.thread = toThreadId(in.thread);
        out.text = in.text.body;
        out.wantAck = true;
        SendResult r = mesh.send(out);
        if (!r.accepted)
            postNotification(r.error && r.error[0] ? r.error : "Send failed", 2);
        break;
    }

    case IntentType::RetrySend:
        // The original text still lives in the log; re-read the tail and resend
        // the record with this packet id.
        {
            ThreadId t = toThreadId(in.thread);
            // Scratch, not stack. See kScratchMessages above.
            const size_t n = chatStore.readTail(t, sScratch, kScratchMessages);
            for (size_t i = 0; i < n; ++i) {
                if (sScratch[i].packetId == in.packetId) {
                    OutgoingMessage out;
                    out.thread = t;
                    out.text = sScratch[i].text;
                    out.wantAck = true;
                    SendResult r = mesh.send(out);
                    if (!r.accepted)
                        postNotification("Retry failed", 2);
                    break;
                }
            }
        }
        break;

    case IntentType::MarkThreadRead: {
        ThreadId t = toThreadId(in.thread);

        // Collect the ids we are about to mark read BEFORE clearing the flags,
        // otherwise there is nothing left to report. Only direct threads get a
        // receipt: broadcasting "I read your channel message" to everyone in
        // range would be meaningless and would waste shared airtime.
        uint32_t receiptIds[MeshBridge::kMaxReceiptIds];
        size_t receiptCount = 0;
        if (t.direct && policy.get().sendReadReceipts) {
            // Scratch, not stack. See kScratchMessages above.
            const size_t got = chatStore.readTail(t, sScratch, kScratchMessages);
            // Walk newest-first so a burst longer than kMaxReceiptIds reports
            // the most recent ones; the older reads are implied by them.
            for (size_t i = got; i-- > 0 && receiptCount < MeshBridge::kMaxReceiptIds;) {
                const ChatMessage &m = sScratch[i];
                if (!(m.flags & kFlagOutbound) && (m.flags & kFlagUnread) && m.packetId)
                    receiptIds[receiptCount++] = m.packetId;
            }
        }

        chatStore.markThreadRead(t);

        if (receiptCount)
            mesh.sendReadReceipt(t, receiptIds, receiptCount);
        Event ev{};
        ev.type = EventType::ThreadRead;
        ev.atMs = millis();
        ev.msg.thread = in.thread;
        events.post(ev);
        break;
    }

    case IntentType::SetRadioMode:
        // May block, and may reboot the device. This is precisely why it is
        // here and not on the UI task.
        coex.request((CoexMode)in.radio.mode, CoexReason::UserRequest);
        break;

    case IntentType::WifiScan:
        if (!wifi.scanStart())
            postNotification("Scan unavailable", 1);
        break;

    case IntentType::WifiJoin:
        if (!wifi.join(in.wifi.ssid, in.wifi.psk))
            postNotification("Could not join network", 2);
        break;

    case IntentType::WifiForget:
        if (in.wifi.ssid[0])
            wifi.forget(in.wifi.ssid);
        else
            wifi.forgetAll();
        break;

    case IntentType::PortalStart:
        if (!portal.start())
            postNotification("Portal failed to start", 2);
        break;

    case IntentType::PortalStop:
        portal.stop();
        break;

    case IntentType::SavePolicy:
        policy.save();
        break;

    case IntentType::CompactStore:
        chatStore.compactIfNeeded();
        break;

    case IntentType::Reboot:
        LOG_INFO("PgrOS: reboot requested");
        rebootAtMsec = millis() + 500;
        break;

    case IntentType::None:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

bool Service::sendText(const ThreadId &thread, const char *text)
{
    if (!text || !text[0])
        return false;

    Intent in;
    in.type = IntentType::SendText;
    in.thread = toThreadRef(thread);

    size_t len = strnlen(text, kMaxTextLen);
    memcpy(in.text.body, text, len);
    in.text.body[len] = '\0';
    in.text.len = (uint16_t)len;
    return post(in);
}

bool Service::retrySend(const ThreadId &thread, uint32_t packetId)
{
    Intent in;
    in.type = IntentType::RetrySend;
    in.thread = toThreadRef(thread);
    in.packetId = packetId;
    return post(in);
}

bool Service::markRead(const ThreadId &thread)
{
    Intent in;
    in.type = IntentType::MarkThreadRead;
    in.thread = toThreadRef(thread);
    return post(in);
}

bool Service::setRadioMode(uint8_t coexMode)
{
    Intent in;
    in.type = IntentType::SetRadioMode;
    in.radio.mode = coexMode;
    return post(in);
}

bool Service::wifiScan()
{
    Intent in;
    in.type = IntentType::WifiScan;
    return post(in);
}

bool Service::wifiJoin(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0])
        return false;

    Intent in;
    in.type = IntentType::WifiJoin;
    strncpy(in.wifi.ssid, ssid, sizeof(in.wifi.ssid) - 1);
    in.wifi.ssid[sizeof(in.wifi.ssid) - 1] = '\0';
    if (psk) {
        strncpy(in.wifi.psk, psk, sizeof(in.wifi.psk) - 1);
        in.wifi.psk[sizeof(in.wifi.psk) - 1] = '\0';
    } else {
        in.wifi.psk[0] = '\0';
    }
    return post(in);
}

bool Service::wifiForget(const char *ssid)
{
    Intent in;
    in.type = IntentType::WifiForget;
    if (ssid) {
        strncpy(in.wifi.ssid, ssid, sizeof(in.wifi.ssid) - 1);
        in.wifi.ssid[sizeof(in.wifi.ssid) - 1] = '\0';
    } else {
        in.wifi.ssid[0] = '\0';
    }
    return post(in);
}

bool Service::portalStart()
{
    Intent in;
    in.type = IntentType::PortalStart;
    return post(in);
}

bool Service::portalStop()
{
    Intent in;
    in.type = IntentType::PortalStop;
    return post(in);
}

bool Service::savePolicy()
{
    Intent in;
    in.type = IntentType::SavePolicy;
    return post(in);
}

bool Service::reboot()
{
    Intent in;
    in.type = IntentType::Reboot;
    return post(in);
}

} // namespace pgros

#endif // PGROS
