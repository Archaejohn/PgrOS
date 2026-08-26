#pragma once
//
// The 22px status bar.
//
// There is no title bar in PgrOS. On a 222px-tall panel a second strip of
// chrome costs a fifth of the usable height, so the screen title lives at the
// left of the status bar and the indicators are pushed right. That buys back a
// whole list row, which is 20% more content on every screen in the firmware.
//
// The bar is EVENT DRIVEN. It never reads NodeDB, the power monitor or the GPS.
// Those live on the mesh task and are being mutated while we render; the UI
// task learns about them only through Event PODs that were filled in at post
// time. The one exception is the clock, which reads the RTC's cached epoch --
// a plain integer read with no bus traffic behind it.
//
// UI TASK ONLY. Every method here calls LVGL.

#include "core/EventBus.h"
#include <stdint.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace pgros {

class StatusBar
{
  public:
    // Builds the bar as a child of `parent` (the active screen). Once only.
    void build(lv_obj_t *parent);

    // The current screen's title, shown at the left. Must outlive the call or
    // be a literal -- App::title() guarantees a static string.
    void setTitle(const char *title);

    // Hidden for fullscreen apps. The content area is resized by the Shell.
    void setVisible(bool visible);
    bool visible() const { return mVisible; }

    // Fold one system event into the indicators. Returns true if anything
    // actually changed, so the caller can skip a redraw.
    bool onEvent(const Event &ev);

    // Cheap periodic refresh; only the clock needs it. Call every ~200 ms.
    void tick();

    // Unread badge. The Messages app owns the real count and pushes it here
    // when it recomputes; events only nudge it.
    void setUnread(uint16_t count);
    uint16_t unread() const { return mUnread; }

    // Mesh link quality, 0..4 bars. Exposed as a setter because nothing on the
    // event bus carries RSSI/SNR today; see the note in applySignal().
    void setSignal(uint8_t bars);

    lv_obj_t *root() const { return mRoot; }
    int16_t height() const;

  private:
    void applyBattery();
    void applyGps();
    void applyRadio();
    void applySignal();
    void applyUnread();
    void applyClock(bool force);

    lv_obj_t *mRoot = nullptr;
    lv_obj_t *mTitle = nullptr;
    lv_obj_t *mTray = nullptr; // right-aligned flex row
    lv_obj_t *mUnreadChip = nullptr;
    lv_obj_t *mUnreadLabel = nullptr;
    lv_obj_t *mGps = nullptr;
    lv_obj_t *mRadio = nullptr;
    lv_obj_t *mSignalBox = nullptr;
    lv_obj_t *mSignalBar[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t *mBattery = nullptr;
    lv_obj_t *mClock = nullptr;

    // Last known state, all pushed in from events.
    uint16_t mUnread = 0;
    uint8_t mBatteryPct = 0;
    bool mCharging = false;
    bool mUsb = false;
    bool mHavePower = false;
    bool mGpsFix = false;
    uint8_t mSats = 0;
    RadioMode mRadioMode = RadioMode::Off;
    uint8_t mBars = 0;
    bool mHaveBars = false;
    uint16_t mLastClockMinute = 0xFFFF;
    bool mVisible = true;
};

extern StatusBar statusBar;

} // namespace pgros
