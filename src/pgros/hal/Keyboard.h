#pragma once
//
// Input bridge: Meshtastic InputBroker -> PgrOS UI task.
//
// ---------------------------------------------------------------------------
// THREADING. Read this before touching Keyboard.cpp.
// ---------------------------------------------------------------------------
//
// InputBroker notifies its observers on the MESHTASTIC MAIN TASK (the keyboard
// and rotary drivers are OSThreads on that task's scheduler). onInputEvent()
// therefore runs on a different core and task from every LVGL call in the
// firmware.
//
// So onInputEvent() does exactly one thing with the key: pushes a word into a
// FreeRTOS queue. The UI task drains it with poll() inside its render loop.
// Calling into LVGL -- even lv_obj_invalidate() -- from the broker callback is
// the classic route to the random heap corruption this design exists to avoid.
//
// Keys are represented as a single uint32_t so the queue item is word sized and
// the whole path is allocation free:
//
//   0x20 .. 0x7E   printable ASCII, straight from InputEvent::kbchar
//   0x08 / 0x0D    backspace / enter, as the keyboard reports them
//   0x100 ..       synthesised navigation keys, see namespace key below
//
// App::onKey(uint32_t) receives exactly these values.

#include <stdint.h>

// InputBroker's event struct, forward declared so this header stays free of the
// Meshtastic include tree (it is pulled in by the shell and by apps).
struct _InputEvent;
typedef struct _InputEvent InputEvent;

namespace pgros {

// Navigation keys. Above the ASCII range so a printable character can never be
// mistaken for one.
namespace key {
static constexpr uint32_t None = 0;

static constexpr uint32_t Backspace = 0x08;
static constexpr uint32_t Tab = 0x09;
static constexpr uint32_t Enter = 0x0D;

static constexpr uint32_t Up = 0x100;
static constexpr uint32_t Down = 0x101;
static constexpr uint32_t Left = 0x102;
static constexpr uint32_t Right = 0x103;
static constexpr uint32_t Select = 0x104; // trackball / rotary press
static constexpr uint32_t Back = 0x105;   // hardware back; pops the nav stack
static constexpr uint32_t Cancel = 0x106; // escape; dismisses modals

// The rotary encoder. Kept SEPARATE from Up/Down on purpose: a rotary detent
// means "move by one" with momentum, whereas Up/Down on the keyboard means
// "move by one line". Screens that scroll continuously want the former.
static constexpr uint32_t RotateCw = 0x107;
static constexpr uint32_t RotateCcw = 0x108;

static constexpr uint32_t SelectLong = 0x109;
static constexpr uint32_t UpLong = 0x10A;
static constexpr uint32_t DownLong = 0x10B;

static constexpr uint32_t Fn1 = 0x110;
static constexpr uint32_t Fn2 = 0x111;
static constexpr uint32_t Fn3 = 0x112;
static constexpr uint32_t Fn4 = 0x113;
static constexpr uint32_t Fn5 = 0x114;

inline bool isPrintable(uint32_t k)
{
    return k >= 0x20 && k <= 0x7E;
}
inline bool isNavigation(uint32_t k)
{
    return k >= 0x100;
}
} // namespace key

class Keyboard
{
  public:
    // Subscribes to InputBroker and re-asserts the rotary encoder config.
    // Called from the main task during stage 1 boot, before the UI task spawns.
    // Idempotent.
    bool begin();

    // Pull one translated key. UI TASK ONLY. Non-blocking; returns false when
    // the queue is empty.
    bool poll(uint32_t &key);

    // Keys dropped because the UI task fell far enough behind to fill the
    // queue. Surfaced in Diagnostics; nonzero means a frame took a very long
    // time, which is worth knowing about.
    uint32_t dropped() const { return mDropped; }

    uint32_t received() const { return mReceived; }

    bool ready() const { return mQueue != nullptr; }

    // InputBroker observer callback. PUBLIC ONLY so CallbackObserver can reach
    // it -- never call this yourself. RUNS ON THE MESHTASTIC MAIN TASK.
    // Returns 0 so other observers (ButtonThread, the phone API) still see the
    // event; PgrOS is a listener here, not a filter.
    int onInputEvent(const InputEvent *ev);

  private:
    // Maps an InputBroker event to a PgrOS key word. Returns key::None for
    // events the UI has no use for.
    static uint32_t translate(const InputEvent *ev);

    void *mQueue = nullptr; // QueueHandle_t of uint32_t, kept opaque
    volatile uint32_t mDropped = 0;
    volatile uint32_t mReceived = 0;
    bool mStarted = false;
};

extern Keyboard keyboard;

} // namespace pgros
