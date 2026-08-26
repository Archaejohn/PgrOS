#ifdef PGROS

#include "ui/apps/SettingsApp.h"

#include "configuration.h"

#include "FSCommon.h"
#include "core/MeshBridge.h"
#include "core/Policy.h"
#include "core/Service.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
#include "hal/Silence.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace pgros
{

SettingsApp settingsApp;

// ---------------------------------------------------------------------------
// Row descriptors
//
// A flat table rather than nested menus. Each row knows only what it is; the
// value it renders and the effect of changing it live in the three switches
// below, keyed by Id. Keeping the table free of behaviour is what lets the row
// order be rearranged without touching any logic.
// ---------------------------------------------------------------------------

enum class Kind : uint8_t {
    Section, // non-selectable header
    Toggle,  // bool
    Choice,  // small enum, stepped
    Number,  // integer with min/max/step
    Info,    // read-only value
    Action,  // does something on enter
};

enum class Id : uint8_t {
    None = 0,
    BootChime,
    KeyClick,
    KeyHaptic,
    MessageAlert,
    DmAlert,
    Volume,
    Brightness,
    KbBrightness,
    ScreenTimeout,
    SleepTimeout,
    ThemeMode_,
    ShortNames,
    RelativeTime,
    ReadReceipts,
    History,
    ShareLocation,
    StoreTrack,
    InfoVersion,
    InfoNode,
    InfoUptime,
    InfoMemory,
    InfoStorage,
    ResetDefaults,
};

struct RowDesc {
    Kind kind;
    Id id;
    const char *label;
    int16_t lo, hi, step; // Number rows only
};

static const RowDesc kTable[] = {
    {Kind::Section, Id::None, "Sound & haptics", 0, 0, 0},
    {Kind::Toggle, Id::BootChime, "Startup sound", 0, 0, 0},
    {Kind::Toggle, Id::KeyClick, "Key click", 0, 0, 0},
    {Kind::Toggle, Id::KeyHaptic, "Key vibration", 0, 0, 0},
    {Kind::Choice, Id::MessageAlert, "Message alert", 0, 0, 0},
    {Kind::Choice, Id::DmAlert, "Direct message alert", 0, 0, 0},
    {Kind::Number, Id::Volume, "Volume", 0, 10, 1},

    {Kind::Section, Id::None, "Display", 0, 0, 0},
    {Kind::Number, Id::Brightness, "Brightness", 20, 255, 15},
    {Kind::Number, Id::KbBrightness, "Keyboard backlight", 0, 255, 32},
    {Kind::Choice, Id::ScreenTimeout, "Screen timeout", 0, 0, 0},
    {Kind::Choice, Id::SleepTimeout, "Sleep after", 0, 0, 0},
    {Kind::Choice, Id::ThemeMode_, "Theme", 0, 0, 0},

    {Kind::Section, Id::None, "Messaging", 0, 0, 0},
    {Kind::Toggle, Id::ShortNames, "Show short names", 0, 0, 0},
    {Kind::Toggle, Id::RelativeTime, "Relative timestamps", 0, 0, 0},
    {Kind::Toggle, Id::ReadReceipts, "Send read receipts", 0, 0, 0},
    {Kind::Number, Id::History, "History per thread", 50, 1000, 50},

    {Kind::Section, Id::None, "Privacy", 0, 0, 0},
    {Kind::Toggle, Id::ShareLocation, "Share location on mesh", 0, 0, 0},
    {Kind::Toggle, Id::StoreTrack, "Record GPS track", 0, 0, 0},

    {Kind::Section, Id::None, "About", 0, 0, 0},
    {Kind::Info, Id::InfoVersion, "Firmware", 0, 0, 0},
    {Kind::Info, Id::InfoNode, "This node", 0, 0, 0},
    {Kind::Info, Id::InfoUptime, "Uptime", 0, 0, 0},
    {Kind::Info, Id::InfoMemory, "Memory", 0, 0, 0},
    {Kind::Info, Id::InfoStorage, "Storage", 0, 0, 0},
    {Kind::Action, Id::ResetDefaults, "Reset all settings", 0, 0, 0},
};

static constexpr uint8_t kTableLen = sizeof(kTable) / sizeof(kTable[0]);
static_assert(kTableLen <= 40, "kTable exceeds SettingsApp::kMaxRows");

// Choice vocabularies.
static const char *kAlertNames[] = {"Off", "Vibrate", "Sound", "Both"};
static const uint16_t kTimeouts[] = {0, 15, 30, 60, 120, 300};
static const char *kTimeoutNames[] = {"Never", "15s", "30s", "1m", "2m", "5m"};
static const uint16_t kSleeps[] = {0, 60, 300, 900, 1800};
static const char *kSleepNames[] = {"Never", "1m", "5m", "15m", "30m"};
static const char *kThemeNames[] = {"Dark", "Light", "Auto"};

template <typename T, size_t N> static uint8_t indexOf(const T (&arr)[N], T value)
{
    for (uint8_t i = 0; i < N; ++i)
        if (arr[i] == value)
            return i;
    return 0;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void SettingsApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);
    lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_SCROLLABLE);

    buildList(mRoot);

    // A persistent hint bar. On a device whose only controls are a rotary and a
    // press, the difference between "turning moves" and "turning changes" is not
    // discoverable, so the screen says which one is live.
    mHint = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mHint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(mHint, lv_color_hex(theme.colors().textFaint), 0);
    lv_obj_set_style_bg_color(mHint, lv_color_hex(theme.colors().bg), 0);
    lv_obj_set_style_bg_opa(mHint, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(mHint, metrics::padS, 0);
    lv_label_set_text(mHint, "turn to move        press to change");
    lv_obj_align(mHint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(mHint);

    buildConfirm(mRoot);
}

void SettingsApp::buildList(lv_obj_t *parent)
{
    mList = lv_obj_create(parent);
    lv_obj_remove_style_all(mList);
    lv_obj_set_size(mList, metrics::screenW, metrics::contentH);
    lv_obj_set_scroll_dir(mList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(mList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mList, 1, 0);
    lv_obj_set_style_pad_all(mList, metrics::padS, 0);

    for (uint8_t i = 0; i < kTableLen; ++i) {
        const RowDesc &d = kTable[i];

        lv_obj_t *row = lv_obj_create(mList);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (d.kind == Kind::Section) {
            // Headers are shorter than rows and carry no value column.
            lv_obj_set_size(row, metrics::screenW - metrics::padM, 20);
            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, theme.fontSmall(), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(theme.colors().accent), 0);
            lv_label_set_text(lbl, d.label);
            lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, metrics::padS, -2);

            mRows[i].obj = row;
            mRows[i].label = lbl;
            mRows[i].value = nullptr;
        } else {
            theme.styleListRow(row);
            lv_obj_set_size(row, metrics::screenW - metrics::padM, 30);

            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, theme.fontBody(), 0);
            lv_obj_set_style_text_color(
                lbl, lv_color_hex(d.kind == Kind::Action ? theme.colors().error : theme.colors().text), 0);
            lv_label_set_text(lbl, d.label);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, metrics::padM, 0);

            lv_obj_t *val = lv_label_create(row);
            lv_obj_set_style_text_font(val, theme.fontBody(), 0);
            lv_obj_set_style_text_color(val, lv_color_hex(theme.colors().textDim), 0);
            lv_label_set_text(val, "");
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, -metrics::padM, 0);

            mRows[i].obj = row;
            mRows[i].label = lbl;
            mRows[i].value = val;
        }
    }
    mRowCount = kTableLen;

    // Start on the first selectable row, never a header.
    mSelected = 1;
}

void SettingsApp::buildConfirm(lv_obj_t *parent)
{
    mConfirm = lv_obj_create(parent);
    lv_obj_remove_style_all(mConfirm);
    lv_obj_set_size(mConfirm, metrics::screenW, metrics::contentH);
    lv_obj_set_style_bg_color(mConfirm, lv_color_hex(theme.colors().bg), 0);
    lv_obj_set_style_bg_opa(mConfirm, LV_OPA_COVER, 0);
    lv_obj_add_flag(mConfirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mConfirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(msg, theme.fontBody(), 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(theme.colors().text), 0);
    lv_label_set_text(msg, "Reset all PgrOS settings to defaults?");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -24);

    lv_obj_t *note = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(note, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(note, lv_color_hex(theme.colors().textFaint), 0);
    // Worth stating: people reasonably fear that "reset" wipes their messages.
    lv_label_set_text(note, "Messages, contacts and mesh config are not affected.");
    lv_obj_align(note, LV_ALIGN_CENTER, 0, -4);

    mConfirmCancel = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(mConfirmCancel, theme.fontBody(), 0);
    lv_label_set_text(mConfirmCancel, "Cancel");
    lv_obj_align(mConfirmCancel, LV_ALIGN_CENTER, -60, 30);

    mConfirmOk = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(mConfirmOk, theme.fontBody(), 0);
    lv_label_set_text(mConfirmOk, "Reset");
    lv_obj_align(mConfirmOk, LV_ALIGN_CENTER, 60, 30);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void SettingsApp::refreshRow(uint8_t i)
{
    if (i >= mRowCount || !mRows[i].value)
        return;

    const RowDesc &d = kTable[i];
    const Policy &p = policy.get();
    char buf[48];

    switch (d.id) {
    case Id::BootChime:
        lv_label_set_text(mRows[i].value, p.bootChime ? "On" : "Off");
        break;
    case Id::KeyClick:
        lv_label_set_text(mRows[i].value, p.keyClick ? "On" : "Off");
        break;
    case Id::KeyHaptic:
        lv_label_set_text(mRows[i].value, p.keyHaptic ? "On" : "Off");
        break;
    case Id::MessageAlert:
        lv_label_set_text(mRows[i].value, kAlertNames[(uint8_t)p.messageAlert & 3]);
        break;
    case Id::DmAlert:
        lv_label_set_text(mRows[i].value, kAlertNames[(uint8_t)p.dmAlert & 3]);
        break;
    case Id::Volume:
        snprintf(buf, sizeof(buf), "%u", (unsigned)p.volume);
        lv_label_set_text(mRows[i].value, buf);
        break;
    case Id::Brightness:
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)(p.brightness * 100 / 255));
        lv_label_set_text(mRows[i].value, buf);
        break;
    case Id::KbBrightness:
        if (p.kbBrightness == 0)
            lv_label_set_text(mRows[i].value, "Off");
        else {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)(p.kbBrightness * 100 / 255));
            lv_label_set_text(mRows[i].value, buf);
        }
        break;
    case Id::ScreenTimeout:
        lv_label_set_text(mRows[i].value, kTimeoutNames[indexOf(kTimeouts, p.screenTimeoutS)]);
        break;
    case Id::SleepTimeout:
        lv_label_set_text(mRows[i].value, kSleepNames[indexOf(kSleeps, p.sleepTimeoutS)]);
        break;
    case Id::ThemeMode_:
        lv_label_set_text(mRows[i].value, kThemeNames[(uint8_t)p.theme % 3]);
        break;
    case Id::ShortNames:
        lv_label_set_text(mRows[i].value, p.showNodeShortNames ? "Short" : "Full");
        break;
    case Id::RelativeTime:
        lv_label_set_text(mRows[i].value, p.relativeTimestamps ? "On" : "Off");
        break;
    case Id::ReadReceipts:
        lv_label_set_text(mRows[i].value, p.sendReadReceipts ? "On" : "Off");
        break;
    case Id::History:
        snprintf(buf, sizeof(buf), "%u", (unsigned)p.historyPerThread);
        lv_label_set_text(mRows[i].value, buf);
        break;
    case Id::ShareLocation:
        lv_label_set_text(mRows[i].value, p.shareLocationOnMesh ? "On" : "Off");
        break;
    case Id::StoreTrack:
        lv_label_set_text(mRows[i].value, p.storeGpsTrack ? "On" : "Off");
        break;

    case Id::InfoVersion:
        lv_label_set_text(mRows[i].value, "PgrOS / " xstr(APP_VERSION));
        break;
    case Id::InfoNode:
        lv_label_set_text(mRows[i].value, mNodeLine);
        break;
    case Id::InfoUptime: {
        const uint32_t up = millis() / 1000;
        if (up >= 3600)
            snprintf(buf, sizeof(buf), "%luh %lum", (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
        else
            snprintf(buf, sizeof(buf), "%lum", (unsigned long)(up / 60));
        lv_label_set_text(mRows[i].value, buf);
        break;
    }
    case Id::InfoMemory:
        snprintf(buf, sizeof(buf), "%luK / %luK PSRAM", (unsigned long)(ESP.getFreeHeap() / 1024),
                 (unsigned long)(ESP.getFreePsram() / 1024));
        lv_label_set_text(mRows[i].value, buf);
        break;
    case Id::InfoStorage:
        snprintf(buf, sizeof(buf), "%lu / %lu K", (unsigned long)(mFsUsed / 1024), (unsigned long)(mFsTotal / 1024));
        lv_label_set_text(mRows[i].value, buf);
        break;

    case Id::ResetDefaults:
        lv_label_set_text(mRows[i].value, LV_SYMBOL_REFRESH);
        break;

    default:
        break;
    }
}

void SettingsApp::refreshAll()
{
    for (uint8_t i = 0; i < mRowCount; ++i)
        refreshRow(i);
    applySelection(false);
}

void SettingsApp::refreshAbout()
{
    for (uint8_t i = 0; i < mRowCount; ++i) {
        const Id id = kTable[i].id;
        if (id == Id::InfoUptime || id == Id::InfoMemory)
            refreshRow(i);
    }
}

void SettingsApp::applySelection(bool scroll)
{
    for (uint8_t i = 0; i < mRowCount; ++i) {
        if (!mRows[i].obj || kTable[i].kind == Kind::Section)
            continue;
        const bool sel = (i == mSelected);
        lv_obj_set_style_bg_color(mRows[i].obj,
                                  lv_color_hex(sel ? theme.colors().surfaceAlt : theme.colors().surface), 0);
        // A thicker accent border marks the row being edited, so it is obvious
        // that the rotary is now changing a value rather than moving down the
        // list.
        lv_obj_set_style_border_width(mRows[i].obj, sel ? (mEditing ? 2 : 1) : 0, 0);
        lv_obj_set_style_border_color(mRows[i].obj, lv_color_hex(theme.colors().accent), 0);

        if (mRows[i].value) {
            const bool hot = sel && mEditing;
            lv_obj_set_style_text_color(mRows[i].value,
                                        lv_color_hex(hot ? theme.colors().accent : theme.colors().textDim), 0);
        }
    }

    if (mHint) {
        if (mEditing)
            lv_label_set_text(mHint, "turn to change      press to finish");
        else
            lv_label_set_text(mHint, "turn to move        press to change");
    }

    if (scroll && mSelected < mRowCount && mRows[mSelected].obj)
        lv_obj_scroll_to_view(mRows[mSelected].obj, LV_ANIM_ON);
}

void SettingsApp::moveSelection(int8_t delta)
{
    int i = (int)mSelected;
    // Skip section headers so the selection never lands on something inert.
    do {
        i += delta;
        if (i < 0 || i >= (int)mRowCount)
            return;
    } while (kTable[i].kind == Kind::Section);

    mSelected = (uint8_t)i;
    applySelection(true);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

static int16_t clampStep(int16_t cur, int8_t dir, const RowDesc &d)
{
    int32_t next = (int32_t)cur + (int32_t)dir * d.step;
    if (next < d.lo)
        next = d.lo;
    if (next > d.hi)
        next = d.hi;
    return (int16_t)next;
}

template <typename T, size_t N> static T cycle(const T (&arr)[N], T cur, int8_t dir)
{
    int idx = (int)indexOf(arr, cur) + dir;
    if (idx < 0)
        idx = (int)N - 1;
    if (idx >= (int)N)
        idx = 0;
    return arr[idx];
}

bool SettingsApp::adjust(uint8_t i, int8_t dir)
{
    if (i >= mRowCount)
        return false;

    const RowDesc &d = kTable[i];
    Policy &p = policy.get();
    bool changed = true;

    switch (d.id) {
    case Id::BootChime:
        p.bootChime = !p.bootChime;
        break;
    case Id::KeyClick:
        p.keyClick = !p.keyClick;
        break;
    case Id::KeyHaptic:
        p.keyHaptic = !p.keyHaptic;
        break;

    case Id::MessageAlert:
        p.messageAlert = (AlertMode)((((uint8_t)p.messageAlert + (dir > 0 ? 1 : 3)) % 4));
        break;
    case Id::DmAlert:
        p.dmAlert = (AlertMode)((((uint8_t)p.dmAlert + (dir > 0 ? 1 : 3)) % 4));
        break;

    case Id::Volume:
        p.volume = (uint8_t)clampStep(p.volume, dir, d);
        break;

    case Id::Brightness:
        p.brightness = (uint8_t)clampStep(p.brightness, dir, d);
        // Live preview: a brightness slider you cannot see the effect of is
        // guesswork.
        display.setBrightness(p.brightness);
        break;

    case Id::KbBrightness:
        p.kbBrightness = (uint8_t)clampStep(p.kbBrightness, dir, d);
        Silence::applyPolicy();
        break;

    case Id::ScreenTimeout:
        p.screenTimeoutS = cycle(kTimeouts, p.screenTimeoutS, dir);
        break;
    case Id::SleepTimeout:
        p.sleepTimeoutS = cycle(kSleeps, p.sleepTimeoutS, dir);
        break;

    case Id::ThemeMode_: {
        const uint8_t next = (uint8_t)(((uint8_t)p.theme + (dir > 0 ? 1 : 2)) % 3);
        p.theme = (ThemeMode)next;
        theme.setDark(p.theme != ThemeMode::Light);
        break;
    }

    case Id::ShortNames:
        p.showNodeShortNames = !p.showNodeShortNames;
        break;
    case Id::RelativeTime:
        p.relativeTimestamps = !p.relativeTimestamps;
        break;
    case Id::ReadReceipts:
        p.sendReadReceipts = !p.sendReadReceipts;
        break;
    case Id::History:
        p.historyPerThread = (uint16_t)clampStep((int16_t)p.historyPerThread, dir, d);
        break;

    case Id::ShareLocation:
        p.shareLocationOnMesh = !p.shareLocationOnMesh;
        break;
    case Id::StoreTrack:
        p.storeGpsTrack = !p.storeGpsTrack;
        break;

    default:
        changed = false;
        break;
    }

    if (changed) {
        markDirty();
        refreshRow(i);
        // Sound settings take effect immediately so the user can hear what they
        // just enabled rather than having to guess.
        if (d.id == Id::KeyClick || d.id == Id::KeyHaptic || d.id == Id::MessageAlert || d.id == Id::DmAlert) {
            Silence::applyPolicy();
            Silence::keyFeedback();
        }
    }
    return changed;
}

bool SettingsApp::activate(uint8_t i)
{
    if (i >= mRowCount)
        return false;

    if (kTable[i].id == Id::ResetDefaults) {
        mConfirmChoice = 0;
        showConfirm(true);
        return true;
    }

    switch (kTable[i].kind) {
    case Kind::Toggle:
        // Two states: pressing is unambiguous, so act at once rather than
        // making the user enter and leave an edit mode to flip a boolean.
        return adjust(i, +1);

    case Kind::Choice:
    case Kind::Number:
        // More than two states, and no arrow keys on this hardware, so hand the
        // rotary over to the value until the user presses again.
        mEditing = true;
        applySelection(false);
        return true;

    default:
        return false; // Info rows and section headers do nothing
    }
}

void SettingsApp::showConfirm(bool show)
{
    mConfirmVisible = show;
    if (show)
        lv_obj_remove_flag(mConfirm, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(mConfirm, LV_OBJ_FLAG_HIDDEN);

    if (show) {
        lv_obj_set_style_text_color(mConfirmCancel,
                                    lv_color_hex(mConfirmChoice == 0 ? theme.colors().text : theme.colors().textFaint),
                                    0);
        lv_obj_set_style_text_color(mConfirmOk,
                                    lv_color_hex(mConfirmChoice == 1 ? theme.colors().error : theme.colors().textFaint),
                                    0);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void SettingsApp::markDirty()
{
    policy.markDirty();
    mDirtySinceMs = millis();
    if (!mDirtySinceMs)
        mDirtySinceMs = 1; // 0 means "nothing pending"
}

void SettingsApp::flushIfDirty(bool force)
{
    if (!mDirtySinceMs)
        return;
    if (!force && (millis() - mDirtySinceMs) < kSaveIdleMs)
        return;

    mDirtySinceMs = 0;
    // Flash writes are synchronous, so they go through the service task rather
    // than stalling a frame.
    service_.savePolicy();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SettingsApp::onShow(const AppArgs &args)
{
    (void)args;

    // Sampled once: usedBytes() walks LittleFS metadata behind the shared SPI
    // lock, which is not something to do per frame.
    mFsUsed = fsUsedBytes();
    mFsTotal = fsTotalBytes();

    const SenderIdentity me = mesh.me();
    snprintf(mNodeLine, sizeof(mNodeLine), "%s  !%08x", me.shortName, (unsigned)me.num);

    mEditing = false;
    showConfirm(false);
    refreshAll();
}

void SettingsApp::onHide()
{
    // Leaving the screen commits immediately; the user has finished fiddling.
    flushIfDirty(true);
}

void SettingsApp::onTick()
{
    flushIfDirty(false);

    const uint32_t now = millis();
    if (now - mLastAboutMs >= 1000) {
        mLastAboutMs = now;
        refreshAbout();
    }
}

bool SettingsApp::onKey(uint32_t k)
{
    if (mConfirmVisible) {
        switch (k) {
        case key::Left:
        case key::Right:
        case key::Up:
        case key::Down:
        case key::RotateCw:
        case key::RotateCcw:
            mConfirmChoice = mConfirmChoice ? 0 : 1;
            showConfirm(true);
            return true;
        case key::Enter:
        case key::Select:
            if (mConfirmChoice) {
                policy.reset();
                theme.setDark(policy.get().theme != ThemeMode::Light);
                display.setBrightness(policy.get().brightness);
                Silence::applyPolicy();
                shell.toast("Settings reset");
                refreshAll();
            }
            showConfirm(false);
            return true;
        case key::Back:
        case key::Cancel:
            showConfirm(false);
            return true;
        default:
            return true; // modal
        }
    }

    // ---- editing a value ------------------------------------------------
    //
    // This device has no arrow keys: the tap map is letters, symbols, Enter,
    // Tab, Backspace, Esc and space. The rotary encoder is the only continuous
    // control, and it reports Up/Down. So a row is "entered" for editing and the
    // rotary then changes the value; without this there is no way to decrease
    // anything at all.
    if (mEditing) {
        switch (k) {
        case key::Down:
        case key::RotateCw:
        case key::Right:
            adjust(mSelected, +1);
            return true;

        case key::Up:
        case key::RotateCcw:
        case key::Left:
            adjust(mSelected, -1);
            return true;

        case key::Enter:
        case key::Select:
        case key::Back:
        case key::Cancel:
            mEditing = false;
            applySelection(false);
            flushIfDirty(true); // commit as soon as they stop fiddling
            return true;

        default:
            return true; // stay in edit mode; stray keys must not escape it
        }
    }

    // ---- moving around --------------------------------------------------
    switch (k) {
    case key::Up:
    case key::RotateCcw:
        moveSelection(-1);
        return true;

    case key::Down:
    case key::RotateCw:
        moveSelection(1);
        return true;

    // Kept for completeness; no key on this board produces them today.
    case key::Left:
        adjust(mSelected, -1);
        return true;

    case key::Right:
        adjust(mSelected, +1);
        return true;

    case key::Enter:
    case key::Select:
        activate(mSelected);
        return true;

    default:
        return false; // Back falls through to the Shell
    }
}

} // namespace pgros

#endif // PGROS
