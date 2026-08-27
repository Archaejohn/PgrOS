#ifdef PGROS
//
// Status bar implementation. UI TASK ONLY.
//
// Layout, left to right:
//
//   [ screen title ..................... ] [ 3 ] [GPS] [BT] [.ıll] [87%⚡] [14:32]
//
// The title is the reason there is no title bar. Indicators are packed into a
// right-aligned flex row so they reflow when one of them hides -- a status bar
// with holes in it looks broken, and something is always off on this device.
//
// Nothing here polls. Every value arrives as an Event that the mesh task filled
// in before posting; the clock is the sole exception and reads only the RTC's
// cached epoch.

#include "StatusBar.h"
#include "Theme.h"

#include "configuration.h"
#include "gps/RTC.h"
#include <lvgl.h>

namespace pgros {

StatusBar statusBar;

namespace {

// Signal bar geometry. Four bars in 14px of width, rising 3..9px, which is
// legible at a glance and costs four tiny objects rather than a canvas.
constexpr int16_t kBarW = 2;
constexpr int16_t kBarGap = 1;
constexpr int16_t kBarH[4] = {3, 5, 7, 9};

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, Color colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

} // namespace

int16_t StatusBar::height() const
{
    return metrics::statusBarH;
}

void StatusBar::build(lv_obj_t *parent)
{
    if (mRoot)
        return;

    const Palette &p = theme.colors();

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::statusBarH);
    lv_obj_set_pos(mRoot, 0, 0);
    lv_obj_set_style_bg_color(mRoot, lv_color_hex(p.surface), 0);
    lv_obj_set_style_bg_opa(mRoot, LV_OPA_COVER, 0);
    // A single hairline under the bar, not a drop shadow. Shadows cost blend
    // passes we do not have the fill rate for on a shared SPI bus.
    lv_obj_set_style_border_color(mRoot, lv_color_hex(p.border), 0);
    lv_obj_set_style_border_width(mRoot, 1, 0);
    lv_obj_set_style_border_side(mRoot, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(mRoot, metrics::padM, 0);
    lv_obj_set_style_pad_ver(mRoot, 0, 0);
    lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(mRoot, LV_SCROLLBAR_MODE_OFF);

    // --- title ------------------------------------------------------------
    mTitle = makeLabel(mRoot, theme.fontSmall(), p.text, "PgrOS");
    lv_label_set_long_mode(mTitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(mTitle, 200); // leaves room for a full tray
    lv_obj_align(mTitle, LV_ALIGN_LEFT_MID, 0, 0);

    // --- indicator tray ---------------------------------------------------
    mTray = lv_obj_create(mRoot);
    lv_obj_remove_style_all(mTray);
    lv_obj_set_size(mTray, LV_SIZE_CONTENT, metrics::statusBarH - 2);
    lv_obj_align(mTray, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(mTray, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mTray, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mTray, metrics::padM - 2, 0);
    lv_obj_remove_flag(mTray, LV_OBJ_FLAG_SCROLLABLE);

    // Unread badge: an accent chip, because an unread message is the single
    // most important thing this device has to tell you.
    mUnreadChip = lv_obj_create(mTray);
    lv_obj_remove_style_all(mUnreadChip);
    lv_obj_set_size(mUnreadChip, LV_SIZE_CONTENT, 14);
    lv_obj_set_style_bg_color(mUnreadChip, lv_color_hex(p.accent), 0);
    lv_obj_set_style_bg_opa(mUnreadChip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mUnreadChip, 7, 0);
    lv_obj_set_style_pad_hor(mUnreadChip, 5, 0);
    lv_obj_set_style_pad_ver(mUnreadChip, 0, 0);
    lv_obj_remove_flag(mUnreadChip, LV_OBJ_FLAG_SCROLLABLE);
    mUnreadLabel = makeLabel(mUnreadChip, theme.fontSmall(), p.accentText, "0");
    lv_obj_center(mUnreadLabel);
    lv_obj_add_flag(mUnreadChip, LV_OBJ_FLAG_HIDDEN);

    mGps = makeLabel(mTray, theme.fontSmall(), p.textFaint, LV_SYMBOL_GPS);
    mRadio = makeLabel(mTray, theme.fontSmall(), p.textFaint, LV_SYMBOL_BLUETOOTH);

    // Signal bars.
    mSignalBox = lv_obj_create(mTray);
    lv_obj_remove_style_all(mSignalBox);
    lv_obj_set_size(mSignalBox, 4 * kBarW + 3 * kBarGap, 10);
    lv_obj_remove_flag(mSignalBox, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        mSignalBar[i] = lv_obj_create(mSignalBox);
        lv_obj_remove_style_all(mSignalBar[i]);
        lv_obj_set_size(mSignalBar[i], kBarW, kBarH[i]);
        lv_obj_set_pos(mSignalBar[i], i * (kBarW + kBarGap), 10 - kBarH[i]);
        lv_obj_set_style_bg_opa(mSignalBar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mSignalBar[i], lv_color_hex(p.border), 0);
        lv_obj_set_style_radius(mSignalBar[i], 1, 0);
    }

    mBattery = makeLabel(mTray, theme.fontSmall(), p.textDim, "--%");
    mClock = makeLabel(mTray, theme.fontSmall(), p.text, "--:--");

    applyUnread();
    applyGps();
    applyRadio();
    applySignal();
    applyBattery();
    applyClock(true);
}

void StatusBar::setTitle(const char *title)
{
    if (!mTitle || !title)
        return;
    lv_label_set_text(mTitle, title);
}

void StatusBar::setVisible(bool visible)
{
    mVisible = visible;
    if (!mRoot)
        return;
    if (visible)
        lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(mRoot, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

bool StatusBar::onEvent(const Event &ev)
{
    switch (ev.type) {
    case EventType::PowerChanged: {
        const bool changed = !mHavePower || mBatteryPct != ev.power.percent || mCharging != (ev.power.charging != 0) ||
                             mUsb != (ev.power.usbPowered != 0);
        mHavePower = true;
        mBatteryPct = ev.power.percent;
        mCharging = ev.power.charging != 0;
        mUsb = ev.power.usbPowered != 0;
        if (changed)
            applyBattery();
        return changed;
    }

    case EventType::GpsFix: {
        const bool fix = ev.gps.fixValid != 0;
        const bool changed = fix != mGpsFix || ev.gps.sats != mSats;
        mGpsFix = fix;
        mSats = ev.gps.sats;
        if (changed)
            applyGps();
        return changed;
    }

    case EventType::RadioState: {
        const RadioMode m = (RadioMode)ev.radio.state;
        if (m == mRadioMode)
            return false;
        mRadioMode = m;
        applyRadio();
        applySignal(); // bars grey out when no radio is up
        return true;
    }

    case EventType::BleConnection:
        // Connection state is carried by the radio glyph's colour rather than a
        // separate icon; there is no room for one.
        applyRadio();
        return true;

    case EventType::MessageReceived:
        // Only inbound messages raise the badge. Our own sends echo back
        // through the same event with outbound set.
        if (!ev.msg.outbound)
            setUnread(mUnread + 1);
        return !ev.msg.outbound;

    case EventType::ThreadRead:
        // The Messages app recomputes the true total and calls setUnread(); this
        // is the optimistic clear so the badge does not lag a frame behind the
        // user's own action.
        setUnread(0);
        return true;

    case EventType::NodeUpdated:
        // Deliberately does NOT touch the bars any more. This used to derive
        // them from the hop count of whichever node was heard last, which was a
        // weak proxy and, worse, fought the real value: the Shell pushes
        // mesh.density().bars in via setSignal() every 200 ms, so the indicator
        // flickered between two different meanings depending on which ran last.
        // Density wins -- it counts direct neighbours and relayed traffic rather
        // than one node's hop count.
        return false;

    default:
        return false;
    }
}

void StatusBar::tick()
{
    applyClock(false);
}

void StatusBar::setUnread(uint16_t count)
{
    if (count == mUnread)
        return;
    mUnread = count;
    applyUnread();
}

void StatusBar::setSignal(uint8_t bars)
{
    if (bars > 4)
        bars = 4;
    if (mHaveBars && bars == mBars)
        return;
    mHaveBars = true;
    mBars = bars;
    applySignal();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void StatusBar::applyUnread()
{
    if (!mUnreadChip)
        return;
    if (mUnread == 0) {
        lv_obj_add_flag(mUnreadChip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(mUnreadChip, LV_OBJ_FLAG_HIDDEN);
    if (mUnread > 99)
        lv_label_set_text(mUnreadLabel, "99+");
    else
        lv_label_set_text_fmt(mUnreadLabel, "%u", (unsigned)mUnread);
}

void StatusBar::applyGps()
{
    if (!mGps)
        return;
    const Palette &p = theme.colors();
    // Sat count is deliberately not shown: at 12px it is three more glyphs for
    // information the user cannot act on. Fix / no fix is the actionable bit.
    lv_obj_set_style_text_color(mGps, lv_color_hex(mGpsFix ? p.ok : p.textFaint), 0);
}

void StatusBar::applyRadio()
{
    if (!mRadio)
        return;
    const Palette &p = theme.colors();
    switch (mRadioMode) {
    case RadioMode::Bluetooth:
        lv_label_set_text(mRadio, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(mRadio, lv_color_hex(p.accent), 0);
        break;
    case RadioMode::WifiStation:
    case RadioMode::WifiAp:
        lv_label_set_text(mRadio, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(mRadio, lv_color_hex(mRadioMode == RadioMode::WifiAp ? p.warn : p.accent), 0);
        break;
    case RadioMode::Off:
    default:
        // Off is shown, not hidden. "Neither radio is up" is a state the user
        // needs to be able to see at a glance, otherwise a failed BLE pairing
        // looks like a phone problem.
        lv_label_set_text(mRadio, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(mRadio, lv_color_hex(p.textFaint), 0);
        break;
    }
}

void StatusBar::applySignal()
{
    if (!mSignalBar[0])
        return;
    const Palette &p = theme.colors();

    // Bars are mesh DENSITY, not one link's margin: distinct nodes heard at zero
    // hops, lifted a step by packet rate. The Shell pushes it in through
    // setSignal() on its periodic tick. See MeshBridge::density().
    const uint8_t bars = mHaveBars ? mBars : 0;
    const Color on = theme.signalColor(bars);

    for (int i = 0; i < 4; i++) {
        const bool lit = mHaveBars && i < bars;
        lv_obj_set_style_bg_color(mSignalBar[i], lv_color_hex(lit ? on : p.border), 0);
    }
}

void StatusBar::applyBattery()
{
    if (!mBattery)
        return;
    const Palette &p = theme.colors();

    if (!mHavePower) {
        lv_label_set_text(mBattery, "--%");
        lv_obj_set_style_text_color(mBattery, lv_color_hex(p.textFaint), 0);
        return;
    }

    if (mCharging || mUsb)
        lv_label_set_text_fmt(mBattery, "%u%% " LV_SYMBOL_CHARGE, (unsigned)mBatteryPct);
    else
        lv_label_set_text_fmt(mBattery, "%u%%", (unsigned)mBatteryPct);

    // Only the genuinely urgent case is coloured. A status bar where three
    // things are amber teaches the user to ignore amber.
    Color colour = p.textDim;
    if (mCharging || mUsb)
        colour = p.ok;
    else if (mBatteryPct <= 10)
        colour = p.error;
    else if (mBatteryPct <= 20)
        colour = p.warn;
    lv_obj_set_style_text_color(mBattery, lv_color_hex(colour), 0);
}

void StatusBar::applyClock(bool force)
{
    if (!mClock)
        return;

    // getValidTime() reads the RTC's cached epoch; it does not touch the I2C
    // RTC part, so it is safe on the UI task. Quality "Device" or better means
    // somebody has actually set the clock.
    const uint32_t now = getValidTime(RTCQualityDevice, true);
    if (now == 0) {
        if (force || mLastClockMinute != 0xFFFF) {
            mLastClockMinute = 0xFFFF;
            lv_label_set_text(mClock, "--:--");
            lv_obj_set_style_text_color(mClock, lv_color_hex(theme.colors().textFaint), 0);
        }
        return;
    }

    const uint16_t minuteOfDay = (uint16_t)((now % 86400UL) / 60UL);
    if (!force && minuteOfDay == mLastClockMinute)
        return; // repaint once a minute, not sixty times a second
    mLastClockMinute = minuteOfDay;

    lv_label_set_text_fmt(mClock, "%02u:%02u", (unsigned)(minuteOfDay / 60), (unsigned)(minuteOfDay % 60));
    lv_obj_set_style_text_color(mClock, lv_color_hex(theme.colors().text), 0);
}

} // namespace pgros

#endif // PGROS
