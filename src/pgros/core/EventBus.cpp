//
// PgrOS event bus -- FreeRTOS queue implementation.
//
// The queue holds Event by value, not by pointer. That is the whole point: a
// producer on the mesh task can fill in an Event on its own stack, post it, and
// return immediately, with no ownership question and no allocation on the
// receive path. sizeof(Event) is fixed by the union in the header, so the queue
// is a single contiguous allocation made once at begin().
//
// EventBus::mQueue is declared `void *` in the header so that nothing which
// includes EventBus.h drags in FreeRTOS. The cast lives here and nowhere else.
//

#ifdef PGROS

#include "EventBus.h"

#include "configuration.h"
#include "freertosinc.h"

#include <Arduino.h>
#include <string.h>

namespace pgros
{

EventBus events;

namespace
{

inline QueueHandle_t handleOf(void *q)
{
    return static_cast<QueueHandle_t>(q);
}

// strlcpy is not portable across every Arduino core in this tree, and we want
// truncation rather than the silent non-termination strncpy gives you.
void copyTruncated(char *dst, size_t dstSize, const char *src)
{
    if (dstSize == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < dstSize && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/// Fills in the fields every event carries, so the helpers below only have to
/// think about their own payload.
Event makeEvent(EventType type)
{
    Event ev;
    // Zero the whole struct: the union's unused tail would otherwise be stack
    // garbage, and a memcmp or a logging dump of an event would be noise.
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.atMs = millis();
    return ev;
}

} // namespace

bool EventBus::begin(uint16_t depth)
{
    if (mQueue)
        return true; // begin() twice is a caller bug, but not a fatal one

    if (depth == 0)
        depth = 1;

    QueueHandle_t q = xQueueCreate(depth, sizeof(Event));
    if (!q) {
        LOG_ERROR("PgrOS: event queue alloc failed (%u x %u bytes)", (unsigned)depth, (unsigned)sizeof(Event));
        return false;
    }

    mQueue = q;
    mDropped = 0;
    LOG_INFO("PgrOS: event bus up, %u slots x %u bytes", (unsigned)depth, (unsigned)sizeof(Event));
    return true;
}

bool EventBus::post(const Event &ev)
{
    if (!mQueue) {
        // Producers legitimately run before begin() during early boot. Count it
        // as a drop rather than crashing; the diagnostics screen will show it.
        mDropped++;
        return false;
    }

    // Timeout of 0 is load-bearing. This is called from the radio and router
    // paths; blocking here to wait for the UI task would add jitter to LoRa
    // timing. A dropped status update is always the cheaper failure.
    if (xQueueSend(handleOf(mQueue), &ev, 0) != pdTRUE) {
        mDropped++;
        return false;
    }
    return true;
}

bool EventBus::postFromIsr(const Event &ev)
{
    if (!mQueue) {
        mDropped++;
        return false;
    }

    BaseType_t higherPriorityTaskWoken = pdFALSE;
    BaseType_t ok = xQueueSendFromISR(handleOf(mQueue), &ev, &higherPriorityTaskWoken);

    if (ok != pdTRUE) {
        // Deliberately a plain increment and not an atomic: mDropped is only
        // ever read for display, and taking a lock in an ISR to protect a
        // counter would be worse than the occasional lost count.
        mDropped++;
    }

    // If unblocking the UI task made a higher-priority task runnable, ask for a
    // context switch on the way out of the ISR instead of waiting for the next
    // tick. On ESP32 this macro takes the flag as an argument.
    portYIELD_FROM_ISR(higherPriorityTaskWoken);

    return ok == pdTRUE;
}

bool EventBus::receive(Event &out, uint32_t timeoutMs)
{
    if (!mQueue)
        return false;

    return xQueueReceive(handleOf(mQueue), &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// -------------------------------------------------------------------------
// Producer helpers
// -------------------------------------------------------------------------

void postBootStage(uint8_t stage, uint8_t percent)
{
    Event ev = makeEvent(EventType::BootStage);
    ev.boot.stage = stage > kBootStageCount ? kBootStageCount : stage;
    ev.boot.percent = percent > 100 ? 100 : percent;
    events.post(ev);
}

void postSubsysReady(Subsys id, bool ok)
{
    Event ev = makeEvent(EventType::SubsysReady);
    ev.subsys.id = static_cast<uint8_t>(id);
    ev.subsys.ok = ok ? 1 : 0;
    events.post(ev);
}

void postNotification(const char *text, uint8_t severity)
{
    Event ev = makeEvent(EventType::Notification);
    ev.note.severity = severity > 2 ? 2 : severity;
    // note.text is 40 bytes including the terminator. Anything longer is
    // truncated here, at post time, rather than being pointed at -- the caller's
    // buffer may be gone by the time the UI task drains the queue.
    copyTruncated(ev.note.text, sizeof(ev.note.text), text);
    events.post(ev);
}

void postBlePairing(uint32_t passkey)
{
    Event ev = makeEvent(EventType::BlePairing);
    ev.ble.passkey = passkey;
    events.post(ev);
}

void postRadioState(RadioMode mode)
{
    Event ev = makeEvent(EventType::RadioState);
    ev.radio.state = static_cast<uint8_t>(mode);
    events.post(ev);
}

} // namespace pgros

#endif // PGROS
