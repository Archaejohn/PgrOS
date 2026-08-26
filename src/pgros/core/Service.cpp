#ifdef PGROS

#include "core/Service.h"

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
        service_.drain();
        return 20;
    }
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
        mDropped++;
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
            ChatMessage msgs[24];
            size_t n = chatStore.readTail(t, msgs, 24);
            for (size_t i = 0; i < n; ++i) {
                if (msgs[i].packetId == in.packetId) {
                    OutgoingMessage out;
                    out.thread = t;
                    out.text = msgs[i].text;
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
        chatStore.markThreadRead(t);
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
