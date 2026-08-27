#ifdef PGROS
//
// Input bridge implementation. See Keyboard.h for the threading contract; the
// short version is that everything in onInputEvent() runs on the Meshtastic
// main task and must not touch LVGL.

#include "Keyboard.h"

#include "configuration.h"
#include "input/InputBroker.h"
#include "mesh/NodeDB.h"
#include "ui/Shell.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Silence owns buzzer/haptic policy and is written separately. Guarded so this
// file builds whether or not it has landed yet; when it does, key feedback
// starts working with no change here.
#if __has_include("hal/Silence.h")
#include "hal/Silence.h"
#define PGROS_HAVE_SILENCE 1
#endif

namespace pgros {

Keyboard keyboard;

namespace {

// 24 keys is about a second of very fast typing. If the UI task is more than a
// second behind, dropping keystrokes is the correct behaviour -- queueing them
// deeper only produces a burst of ghost input when it catches up.
constexpr uint16_t kQueueDepth = 24;

// The InputBroker observer. File scope rather than a Keyboard member so that
// Keyboard.h does not have to include Observer.h and drag the Meshtastic tree
// into every app that wants a key code.
CallbackObserver<Keyboard, const InputEvent *> sObserver(&keyboard, &Keyboard::onInputEvent);

// ---------------------------------------------------------------------------
// Rotary encoder config, re-asserted defensively.
//
// RotaryEncoderImpl::init() (src/input/RotaryEncoderImpl.cpp:30) returns early
// and silently if moduleConfig.canned_message.updown1_enabled is false or the
// pins are zero. NodeDB installs the correct values for T_LORA_PAGER
// (NodeDB.cpp:1301-1309), but moduleConfig is PERSISTED: a device that was ever
// flashed with a build where the flag was false, or whose config was written by
// an older phone app, carries the bad value forward across reflashes and the
// encoder just never works, with nothing in the log to say why.
//
// Note the values NodeDB picks for this board, which is the whole reason
// translate() looks the way it does:
//     inputbroker_event_cw  = 28  (INPUT_BROKER_USER_PRESS)
//     inputbroker_event_ccw = 29  (INPUT_BROKER_ALT_PRESS)
// The pager's encoder does NOT emit UP/DOWN.
// ---------------------------------------------------------------------------
bool ensureRotaryConfig()
{
    bool changed = false;
    auto &cm = moduleConfig.canned_message;

    if (!cm.updown1_enabled) {
        LOG_WARN("PgrOS: rotary updown1_enabled was false in persisted config; re-enabling");
        cm.updown1_enabled = true;
        changed = true;
    }
#ifdef ROTARY_A
    if (cm.inputbroker_pin_a == 0) {
        cm.inputbroker_pin_a = ROTARY_A;
        changed = true;
    }
#endif
#ifdef ROTARY_B
    if (cm.inputbroker_pin_b == 0) {
        cm.inputbroker_pin_b = ROTARY_B;
        changed = true;
    }
#endif
#ifdef ROTARY_PRESS
    if (cm.inputbroker_pin_press == 0) {
        cm.inputbroker_pin_press = ROTARY_PRESS;
        changed = true;
    }
#endif
    if (cm.inputbroker_event_cw == 0 || cm.inputbroker_event_ccw == 0) {
        cm.inputbroker_event_cw = (meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar)INPUT_BROKER_USER_PRESS;
        cm.inputbroker_event_ccw = (meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar)INPUT_BROKER_ALT_PRESS;
        changed = true;
    }
    moduleConfig.has_canned_message = true;
    return changed;
}

} // namespace

// ---------------------------------------------------------------------------

bool Keyboard::begin()
{
    if (mStarted)
        return true;

    mQueue = xQueueCreate(kQueueDepth, sizeof(uint32_t));
    if (!mQueue) {
        LOG_ERROR("PgrOS: keyboard queue alloc failed");
        return false;
    }

    // We run after inputBroker->Init() and setupModules(), so the encoder impl
    // has already decided whether to arm itself. Fixing the config now takes
    // effect on the next boot; persisting it is what makes that stick, and it
    // is a rare, one-off write on the main task during setup(), not a hot path.
    if (ensureRotaryConfig()) {
        LOG_WARN("PgrOS: repaired rotary encoder config; persisting (effective next boot)");
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_MODULECONFIG);
    }

    if (!inputBroker) {
        // Modules.cpp only allocates InputBroker for certain display modes; if
        // it is missing the device has no usable input at all and that is worth
        // shouting about rather than failing quietly.
        LOG_ERROR("PgrOS: inputBroker is null -- keyboard and rotary will not work");
        return false;
    }

    sObserver.observe(inputBroker);
    mStarted = true;
    LOG_INFO("PgrOS: keyboard bridged to InputBroker");
    return true;
}

// ---------------------------------------------------------------------------
// MESHTASTIC MAIN TASK from here down (except poll()).
// ---------------------------------------------------------------------------

uint32_t Keyboard::translate(const InputEvent *ev)
{
    switch (ev->inputEvent) {
    case INPUT_BROKER_UP:
        return key::Up;
    case INPUT_BROKER_DOWN:
        return key::Down;
    case INPUT_BROKER_LEFT:
        return key::Left;
    case INPUT_BROKER_RIGHT:
        return key::Right;
    case INPUT_BROKER_SELECT:
        return key::Select;
    case INPUT_BROKER_SELECT_LONG:
        return key::SelectLong;
    case INPUT_BROKER_UP_LONG:
        return key::UpLong;
    case INPUT_BROKER_DOWN_LONG:
        return key::DownLong;
    // The physical Backspace key and a Back navigation gesture arrive as the
    // SAME event: kbI2cBase maps the keyboard's BSP to INPUT_BROKER_BACK and
    // puts 0x08 in kbchar. Treating them alike made Backspace act as "cancel",
    // which in a text field discards everything the user had typed -- reported
    // as "backspace deletes the whole box".
    case INPUT_BROKER_BACK:
        return (ev->kbchar == 0x08 || ev->kbchar == 0x7F) ? key::Backspace : key::Back;
    case INPUT_BROKER_CANCEL:
        return key::Cancel;

    // The rotary encoder, NOT a pair of buttons. NodeDB maps clockwise to
    // USER_PRESS and counter-clockwise to ALT_PRESS on this board; a UI that
    // waits for UP/DOWN from the encoder waits forever.
    case INPUT_BROKER_USER_PRESS:
        return key::RotateCw;
    case INPUT_BROKER_ALT_PRESS:
        return key::RotateCcw;

    case INPUT_BROKER_FN_F1:
        return key::Fn1;
    case INPUT_BROKER_FN_F2:
        return key::Fn2;
    case INPUT_BROKER_FN_F3:
        return key::Fn3;
    case INPUT_BROKER_FN_F4:
        return key::Fn4;
    case INPUT_BROKER_FN_F5:
        return key::Fn5;

    // Printable characters arrive as ANYKEY with the byte in kbchar. So do a
    // few control codes the keyboard sends directly.
    case INPUT_BROKER_ANYKEY:
    case INPUT_BROKER_MATRIXKEY: {
        const unsigned char ch = ev->kbchar;
        if (ch >= 0x20 && ch <= 0x7E)
            return (uint32_t)ch;
        if (ch == 0x08 || ch == 0x7F)
            return key::Backspace;
        if (ch == 0x0D || ch == 0x0A)
            return key::Enter;
        if (ch == 0x09)
            return key::Tab;
        if (ch == 0x1B)
            return key::Cancel;
        return key::None;
    }

    default:
        // Shutdown, factory reset, GPS toggle and friends are Meshtastic's
        // business; we deliberately do not consume or react to them.
        return key::None;
    }
}

int Keyboard::onInputEvent(const InputEvent *ev)
{
    // Return 0 always: other observers (ButtonThread, canned messages, the
    // phone API) must still see every event. PgrOS listens, it does not filter.
    if (!ev)
        return 0;

    const uint32_t k = translate(ev);
    if (k == key::None)
        return 0;

    mReceived++;

    // Wake the panel and reset the blank timer. noteActivity() is explicitly
    // safe off the UI task: it stores a timestamp and raises a flag, and the UI
    // task is what actually drives the backlight.
    shell.noteActivity();

#ifdef PGROS_HAVE_SILENCE
    // Per-keystroke click/haptic, gated by Policy (off by default). This runs
    // on the main task, which is where the I2C haptic driver belongs -- never
    // on the UI task.
    Silence::keyFeedback();
#endif

    if (!mQueue)
        return 0;

    // Never block the main task on a full UI queue. Dropping a keystroke beats
    // stalling the router.
    if (xQueueSend((QueueHandle_t)mQueue, &k, 0) != pdTRUE)
        mDropped++;

    return 0;
}

// ---------------------------------------------------------------------------
// UI TASK
// ---------------------------------------------------------------------------

bool Keyboard::poll(uint32_t &key)
{
    if (!mQueue)
        return false;
    return xQueueReceive((QueueHandle_t)mQueue, &key, 0) == pdTRUE;
}

} // namespace pgros

#endif // PGROS
