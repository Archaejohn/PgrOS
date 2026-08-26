#pragma once
//
// PgrOS app framework.
//
// An "app" is one full-screen view with a lifecycle, owning a single LVGL
// container. The Shell keeps a navigation stack of them; hardware Back pops.
//
// Everything here runs on the UI task and only on the UI task. Apps must never
// block: no I2C, no SPI, no synchronous file writes bigger than a few KB, no
// RadioCoex::request(). Work that can block is handed to the service task and
// comes back as an Event.
//
// Apps are allocated once at startup and reused. onCreate() builds the widget
// tree; onShow()/onHide() only bind and unbind data. Rebuilding an LVGL tree on
// every navigation is the usual reason a small-screen UI feels sluggish, so we
// pay that cost once, at a moment when the user is not waiting.

#include "core/EventBus.h"
#include <stdint.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace pgros {

// Stable identifiers so navigation can be expressed without pointers.
enum class AppId : uint8_t {
    None = 0,
    Home,
    Messages,     // thread list
    Conversation, // one thread
    Compose,      // new message: pick recipient
    Contacts,     // node list
    NodeDetail,
    Map,
    Gps,
    Settings,
    Network, // wifi / bluetooth
    Portal,  // AP + web portal status
    Gallery,
    Diagnostics,
    Count
};

// Arguments passed when navigating. Trivially copyable POD; no pointers into
// caller memory, for the same reason events carry no pointers.
struct AppArgs {
    ThreadRef thread; // for Conversation
    uint32_t nodeNum; // for NodeDetail
    uint32_t u32;     // app-specific
};

class Shell;

class App
{
  public:
    virtual ~App() = default;

    virtual AppId id() const = 0;

    // Short title for the status bar. Must be a static string.
    virtual const char *title() const = 0;

    // Build the widget tree under `parent`. Called exactly once, off the
    // critical boot path. Store the root in mRoot.
    virtual void onCreate(lv_obj_t *parent) = 0;

    // Becoming visible / going away. Bind data here, not in onCreate.
    virtual void onShow(const AppArgs &args) {}
    virtual void onHide() {}

    // One system event. Return true if consumed. Called only while visible.
    virtual bool onEvent(const Event &ev) { return false; }

    // A key from the physical keyboard. Return true if consumed; if false, the
    // Shell applies its default handling (Back pops the stack, etc.).
    virtual bool onKey(uint32_t key) { return false; }

    // Periodic tick while visible, roughly every 200 ms. Keep it cheap; this
    // runs inside the render loop.
    virtual void onTick() {}

    // True if this app wants the status bar hidden (e.g. a full-screen gallery).
    virtual bool fullscreen() const { return false; }

    // True if Back should exit to Home rather than popping one level.
    virtual bool backExitsToHome() const { return false; }

    lv_obj_t *root() const { return mRoot; }
    bool created() const { return mRoot != nullptr; }

  protected:
    lv_obj_t *mRoot = nullptr;
    Shell *mShell = nullptr;
    friend class Shell;
};

} // namespace pgros
