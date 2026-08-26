#ifdef PGROS

#include "ui/apps/NetworkApp.h"

#include "configuration.h"

#include "core/Service.h"
#include "hal/Keyboard.h"
#include "net/Portal.h"
#include "net/RadioCoex.h"
#include "net/WifiManager.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace pgros
{

NetworkApp networkApp;

static const char *kModeNames[] = {"Off", "Bluetooth", "Join WiFi", "WiFi Hotspot"};
static const char *kModeBlurb[] = {"Both radios off. Longest battery life.",
                                   "Pair with the Meshtastic phone app.",
                                   "Connect to an access point.",
                                   "Serve the chatroom and gallery."};

// Row helper shared by the modes and scan lists, so the two feel identical.
static lv_obj_t *makeRow(lv_obj_t *parent, int16_t y, int16_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    theme.styleListRow(row);
    lv_obj_set_size(row, metrics::screenW - metrics::padL * 2, h);
    lv_obj_set_pos(row, metrics::padL, y);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void markSelected(lv_obj_t *row, bool sel)
{
    if (!row)
        return;
    lv_obj_set_style_bg_color(row, lv_color_hex(sel ? theme.colors().surfaceAlt : theme.colors().surface), 0);
    lv_obj_set_style_border_width(row, sel ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(theme.colors().accent), 0);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void NetworkApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);
    lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_SCROLLABLE);

    buildModes(mRoot);
    buildScan(mRoot);
    buildPassphrase(mRoot);
    buildHotspot(mRoot);
    buildConfirm(mRoot);

    showPane(Pane::Modes);
}

void NetworkApp::buildModes(lv_obj_t *parent)
{
    mModes = lv_obj_create(parent);
    lv_obj_remove_style_all(mModes);
    lv_obj_set_size(mModes, metrics::screenW, metrics::contentH);
    lv_obj_remove_flag(mModes, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < kModeRows; ++i) {
        mModeRow[i] = makeRow(mModes, 2 + i * 40, 38);

        mModeLabel[i] = lv_label_create(mModeRow[i]);
        lv_obj_set_style_text_font(mModeLabel[i], theme.fontBody(), 0);
        lv_obj_set_style_text_color(mModeLabel[i], lv_color_hex(theme.colors().text), 0);
        lv_label_set_text(mModeLabel[i], kModeNames[i]);
        lv_obj_align(mModeLabel[i], LV_ALIGN_LEFT_MID, metrics::padM, -7);

        lv_obj_t *blurb = lv_label_create(mModeRow[i]);
        lv_obj_set_style_text_font(blurb, theme.fontSmall(), 0);
        lv_obj_set_style_text_color(blurb, lv_color_hex(theme.colors().textFaint), 0);
        lv_label_set_text(blurb, kModeBlurb[i]);
        lv_obj_align(blurb, LV_ALIGN_LEFT_MID, metrics::padM, 8);

        // Filled dot marks the active mode.
        mModeMark[i] = lv_label_create(mModeRow[i]);
        lv_obj_set_style_text_font(mModeMark[i], theme.fontBody(), 0);
        lv_obj_set_style_text_color(mModeMark[i], lv_color_hex(theme.colors().ok), 0);
        lv_label_set_text(mModeMark[i], "");
        lv_obj_align(mModeMark[i], LV_ALIGN_RIGHT_MID, -metrics::padM, 0);
    }

    mModeStatus = lv_label_create(mModes);
    lv_obj_set_style_text_font(mModeStatus, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(mModeStatus, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(mModeStatus, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -2);
}

void NetworkApp::buildScan(lv_obj_t *parent)
{
    mScan = lv_obj_create(parent);
    lv_obj_remove_style_all(mScan);
    lv_obj_set_size(mScan, metrics::screenW, metrics::contentH);
    lv_obj_add_flag(mScan, LV_OBJ_FLAG_HIDDEN);

    mScanList = lv_obj_create(mScan);
    lv_obj_remove_style_all(mScanList);
    lv_obj_set_size(mScanList, metrics::screenW, metrics::contentH - 16);
    lv_obj_set_scroll_dir(mScanList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mScanList, LV_SCROLLBAR_MODE_OFF);

    for (uint8_t i = 0; i < kScanRows; ++i) {
        mScanRow[i] = makeRow(mScanList, 2 + i * 32, 30);
        lv_obj_add_flag(mScanRow[i], LV_OBJ_FLAG_HIDDEN);

        mScanSsid[i] = lv_label_create(mScanRow[i]);
        lv_obj_set_style_text_font(mScanSsid[i], theme.fontBody(), 0);
        lv_obj_set_style_text_color(mScanSsid[i], lv_color_hex(theme.colors().text), 0);
        lv_label_set_long_mode(mScanSsid[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(mScanSsid[i], 300);
        lv_obj_align(mScanSsid[i], LV_ALIGN_LEFT_MID, metrics::padM, 0);

        mScanMeta[i] = lv_label_create(mScanRow[i]);
        lv_obj_set_style_text_font(mScanMeta[i], theme.fontSmall(), 0);
        lv_obj_set_style_text_color(mScanMeta[i], lv_color_hex(theme.colors().textDim), 0);
        lv_obj_align(mScanMeta[i], LV_ALIGN_RIGHT_MID, -metrics::padM, 0);
    }

    mScanSpinner = lv_spinner_create(mScan);
    lv_obj_set_size(mScanSpinner, 28, 28);
    lv_obj_align(mScanSpinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_arc_color(mScanSpinner, lv_color_hex(theme.colors().accent), LV_PART_INDICATOR);

    mScanEmpty = lv_label_create(mScan);
    lv_obj_set_style_text_font(mScanEmpty, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mScanEmpty, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(mScanEmpty, "No networks found");
    lv_obj_center(mScanEmpty);
    lv_obj_add_flag(mScanEmpty, LV_OBJ_FLAG_HIDDEN);
}

void NetworkApp::buildPassphrase(lv_obj_t *parent)
{
    mPass = lv_obj_create(parent);
    lv_obj_remove_style_all(mPass);
    lv_obj_set_size(mPass, metrics::screenW, metrics::contentH);
    lv_obj_add_flag(mPass, LV_OBJ_FLAG_HIDDEN);

    mPassSsid = lv_label_create(mPass);
    lv_obj_set_style_text_font(mPassSsid, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mPassSsid, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(mPassSsid, LV_ALIGN_TOP_LEFT, metrics::padL, 20);

    lv_obj_t *field = lv_obj_create(mPass);
    lv_obj_remove_style_all(field);
    theme.styleTextInput(field);
    lv_obj_set_size(field, metrics::screenW - metrics::padL * 2, 34);
    lv_obj_set_pos(field, metrics::padL, 56);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);

    mPassField = lv_label_create(field);
    lv_obj_set_style_text_font(mPassField, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mPassField, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(mPassField, LV_ALIGN_LEFT_MID, metrics::padM, 0);
    lv_label_set_text(mPassField, "");

    mPassHint = lv_label_create(mPass);
    lv_obj_set_style_text_font(mPassHint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(mPassHint, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(mPassHint, "Enter to join   Tab to reveal   Back to cancel");
    lv_obj_align(mPassHint, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -6);
}

void NetworkApp::buildHotspot(lv_obj_t *parent)
{
    mHotspot = lv_obj_create(parent);
    lv_obj_remove_style_all(mHotspot);
    lv_obj_set_size(mHotspot, metrics::screenW, metrics::contentH);
    lv_obj_add_flag(mHotspot, LV_OBJ_FLAG_HIDDEN);

    // People retype these into a phone, so they get the large font.
    lv_obj_t *l1 = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(l1, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(l1, lv_color_hex(theme.colors().textDim), 0);
    lv_label_set_text(l1, "Network");
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, metrics::padL, 10);

    mApSsid = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(mApSsid, theme.fontLarge(), 0);
    lv_obj_set_style_text_color(mApSsid, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(mApSsid, LV_ALIGN_TOP_LEFT, metrics::padL, 26);

    lv_obj_t *l2 = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(l2, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(l2, lv_color_hex(theme.colors().textDim), 0);
    lv_label_set_text(l2, "Password");
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, metrics::padL, 66);

    mApPass = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(mApPass, theme.fontLarge(), 0);
    lv_obj_set_style_text_color(mApPass, lv_color_hex(theme.colors().accent), 0);
    lv_obj_align(mApPass, LV_ALIGN_TOP_LEFT, metrics::padL, 82);

    mApUrl = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(mApUrl, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mApUrl, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(mApUrl, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -24);

    mApClients = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(mApClients, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(mApClients, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(mApClients, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -6);

    lv_obj_t *hint = lv_label_create(mHotspot);
    lv_obj_set_style_text_font(hint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(hint, "r  new password");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -metrics::padL, -6);
}

void NetworkApp::buildConfirm(lv_obj_t *parent)
{
    mConfirm = lv_obj_create(parent);
    lv_obj_remove_style_all(mConfirm);
    lv_obj_set_size(mConfirm, metrics::screenW, metrics::contentH);
    lv_obj_set_style_bg_color(mConfirm, lv_color_hex(theme.colors().bg), 0);
    lv_obj_set_style_bg_opa(mConfirm, LV_OPA_COVER, 0);
    lv_obj_add_flag(mConfirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mConfirm, LV_OBJ_FLAG_SCROLLABLE);

    mConfirmText = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(mConfirmText, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mConfirmText, lv_color_hex(theme.colors().text), 0);
    lv_label_set_long_mode(mConfirmText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mConfirmText, metrics::screenW - metrics::padL * 2);
    lv_obj_set_style_text_align(mConfirmText, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mConfirmText, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *hint = lv_label_create(mConfirm);
    lv_obj_set_style_text_font(hint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(hint, "Enter to restart      Back to cancel");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 34);
}

// ---------------------------------------------------------------------------
// Panes
// ---------------------------------------------------------------------------

void NetworkApp::showPane(Pane p)
{
    mPane = p;
    lv_obj_t *panes[] = {mModes, mScan, mPass, mHotspot, mConfirm};
    for (uint8_t i = 0; i < 5; ++i) {
        if (!panes[i])
            continue;
        if ((uint8_t)p == i)
            lv_obj_remove_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
    }
    mSelected = 0;

    switch (p) {
    case Pane::Modes:
        refreshModes();
        break;
    case Pane::Scan:
        refreshScan();
        break;
    case Pane::Passphrase:
        refreshPassphrase();
        break;
    case Pane::Hotspot:
        refreshHotspot();
        break;
    default:
        break;
    }
}

void NetworkApp::onShow(const AppArgs &args)
{
    (void)args;
    showPane(Pane::Modes);
}

void NetworkApp::refreshModes()
{
    const uint8_t active = (uint8_t)coex.mode();
    for (uint8_t i = 0; i < kModeRows; ++i) {
        markSelected(mModeRow[i], i == mSelected);
        lv_label_set_text(mModeMark[i], i == active ? LV_SYMBOL_OK : "");
    }

    char buf[64];
    if (coex.mode() == CoexMode::WifiStation && wifi.connected())
        snprintf(buf, sizeof(buf), "Connected to %s  %s", wifi.currentSsid(), wifi.ipAddress());
    else if (coex.mode() == CoexMode::WifiAp)
        snprintf(buf, sizeof(buf), "Hotspot running  %s", wifi.ipAddress());
    else
        snprintf(buf, sizeof(buf), "Current: %s", coex.modeName());
    lv_label_set_text(mModeStatus, buf);
}

void NetworkApp::refreshScan()
{
    const bool scanning = wifi.scanning();
    mResultCount = (uint8_t)wifi.scanResults(mResults, kScanRows);

    if (scanning) {
        lv_obj_remove_flag(mScanSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mScanEmpty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mScanSpinner, LV_OBJ_FLAG_HIDDEN);
        if (!mResultCount)
            lv_obj_remove_flag(mScanEmpty, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(mScanEmpty, LV_OBJ_FLAG_HIDDEN);
    }

    for (uint8_t i = 0; i < kScanRows; ++i) {
        if (i >= mResultCount) {
            lv_obj_add_flag(mScanRow[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(mScanRow[i], LV_OBJ_FLAG_HIDDEN);
        markSelected(mScanRow[i], i == mSelected);

        lv_label_set_text(mScanSsid[i], mResults[i].ssid);

        // Lock glyph for secured, a dot for saved. Both matter when choosing.
        char meta[24];
        snprintf(meta, sizeof(meta), "%s%s%u/4", mResults[i].security == WifiSecurity::Open ? "" : LV_SYMBOL_EJECT " ",
                 mResults[i].saved ? LV_SYMBOL_OK " " : "", (unsigned)mResults[i].bars());
        lv_label_set_text(mScanMeta[i], meta);
        lv_obj_set_style_text_color(mScanMeta[i], lv_color_hex(theme.signalColor(mResults[i].bars())), 0);
    }
}

void NetworkApp::refreshPassphrase()
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Password for %s", mPendingSsid);
    lv_label_set_text(mPassSsid, buf);

    // Masked by default. Typing a WPA key on a 31-key thumb keyboard goes wrong
    // often enough that reveal is worth having.
    if (mPassReveal) {
        lv_label_set_text(mPassField, mPassBuf);
    } else {
        char mask[kMaxPassphrase];
        uint8_t i = 0;
        for (; i < mPassLen && i < sizeof(mask) - 1; ++i)
            mask[i] = '*';
        mask[i] = '\0';
        lv_label_set_text(mPassField, mask);
    }
}

void NetworkApp::refreshHotspot()
{
    lv_label_set_text(mApSsid, wifi.apSsid());
    lv_label_set_text(mApPass, wifi.apPassphrase());

    char buf[64];
    snprintf(buf, sizeof(buf), "http://%s", wifi.ipAddress());
    lv_label_set_text(mApUrl, buf);

    snprintf(buf, sizeof(buf), "%u connected   %u photos", (unsigned)wifi.apClientCount(),
             (unsigned)portal.stats().galleryItems);
    lv_label_set_text(mApClients, buf);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void NetworkApp::moveSelection(int8_t delta)
{
    uint8_t count = 0;
    switch (mPane) {
    case Pane::Modes:
        count = kModeRows;
        break;
    case Pane::Scan:
        count = mResultCount;
        break;
    case Pane::Confirm:
        mConfirmChoice = mConfirmChoice ? 0 : 1;
        return;
    default:
        return;
    }
    if (!count)
        return;

    const int next = (int)mSelected + delta;
    if (next < 0 || next >= (int)count)
        return; // stop at the ends rather than wrapping; wrapping on a short
                // list is disorienting when you cannot see the whole thing
    mSelected = (uint8_t)next;

    if (mPane == Pane::Modes)
        refreshModes();
    else
        refreshScan();
}

void NetworkApp::requestMode(uint8_t coexMode)
{
    if ((CoexMode)coexMode == coex.mode()) {
        shell.toast("Already active");
        return;
    }

    if (coex.requiresReboot((CoexMode)coexMode)) {
        // Explain the restart before it happens. This is the whole reason the
        // confirm pane exists: a device that reboots itself with no warning
        // looks broken, even when it is doing exactly the right thing.
        mPendingMode = coexMode;
        mConfirmChoice = 1;
        char buf[220];
        snprintf(buf, sizeof(buf),
                 "Switching to %s needs a restart.\n\n"
                 "Bluetooth and WiFi share one radio, and this chip cannot take the Bluetooth "
                 "controller back without rebooting.\n\nPgrOS restarts quickly.",
                 kModeNames[coexMode]);
        lv_label_set_text(mConfirmText, buf);
        showPane(Pane::Confirm);
        return;
    }

    // Live transition.
    service_.setRadioMode(coexMode);
    shell.showBusy("Switching radio");
}

void NetworkApp::commitPendingMode()
{
    shell.showBusy("Restarting");
    service_.setRadioMode(mPendingMode);
}

void NetworkApp::activate()
{
    switch (mPane) {

    case Pane::Modes:
        if (mSelected == (uint8_t)CoexMode::WifiStation) {
            // Joining needs a network chosen first.
            service_.wifiScan();
            showPane(Pane::Scan);
        } else if (mSelected == (uint8_t)CoexMode::WifiAp) {
            requestMode((uint8_t)CoexMode::WifiAp);
            showPane(Pane::Hotspot);
        } else {
            requestMode(mSelected);
        }
        break;

    case Pane::Scan: {
        if (mSelected >= mResultCount)
            return;
        const ScanResult &r = mResults[mSelected];
        strncpy(mPendingSsid, r.ssid, sizeof(mPendingSsid) - 1);
        mPendingSsid[sizeof(mPendingSsid) - 1] = '\0';

        if (r.security == WifiSecurity::Open || r.saved) {
            // Open, or we already hold credentials: join straight away.
            service_.wifiJoin(mPendingSsid, "");
            shell.showBusy("Joining");
            showPane(Pane::Modes);
        } else {
            mPassLen = 0;
            mPassBuf[0] = '\0';
            mPassReveal = false;
            showPane(Pane::Passphrase);
        }
        break;
    }

    case Pane::Passphrase:
        if (mPassLen < 8) {
            shell.toast("Password too short", 1);
            return;
        }
        service_.wifiJoin(mPendingSsid, mPassBuf);
        shell.showBusy("Joining");
        // Do not leave the key sitting in RAM once it has been handed over.
        memset(mPassBuf, 0, sizeof(mPassBuf));
        mPassLen = 0;
        showPane(Pane::Modes);
        break;

    case Pane::Confirm:
        if (mConfirmChoice)
            commitPendingMode();
        else
            showPane(Pane::Modes);
        break;

    default:
        break;
    }
}

bool NetworkApp::onKey(uint32_t k)
{
    // Passphrase entry swallows printable keys before anything else.
    if (mPane == Pane::Passphrase) {
        if (key::isPrintable(k)) {
            if (mPassLen < sizeof(mPassBuf) - 1) {
                mPassBuf[mPassLen++] = (char)k;
                mPassBuf[mPassLen] = '\0';
                refreshPassphrase();
            }
            return true;
        }
        if (k == key::Backspace) {
            if (mPassLen) {
                mPassBuf[--mPassLen] = '\0';
                refreshPassphrase();
            }
            return true;
        }
        if (k == key::Tab) {
            mPassReveal = !mPassReveal;
            refreshPassphrase();
            return true;
        }
        if (k == key::Enter || k == key::Select) {
            activate();
            return true;
        }
        if (k == key::Back || k == key::Cancel) {
            memset(mPassBuf, 0, sizeof(mPassBuf));
            mPassLen = 0;
            showPane(Pane::Scan);
            return true;
        }
        return true; // stay modal
    }

    switch (k) {
    case key::Up:
    case key::RotateCcw:
        moveSelection(-1);
        return true;

    case key::Down:
    case key::RotateCw:
        moveSelection(1);
        return true;

    case key::Left:
        if (mPane == Pane::Confirm) {
            mConfirmChoice = 0;
            return true;
        }
        break;

    case key::Right:
        if (mPane == Pane::Confirm) {
            mConfirmChoice = 1;
            return true;
        }
        break;

    case key::Enter:
    case key::Select:
        activate();
        return true;

    case key::Back:
    case key::Cancel:
        if (mPane != Pane::Modes) {
            showPane(Pane::Modes);
            return true;
        }
        return false; // let the Shell pop us

    default:
        break;
    }

    if (mPane == Pane::Scan && (k == 'r' || k == 'R')) {
        service_.wifiScan();
        refreshScan();
        return true;
    }
    if (mPane == Pane::Hotspot && (k == 'r' || k == 'R')) {
        wifi.setApCredentials(nullptr, nullptr); // regenerate
        refreshHotspot();
        shell.toast("New hotspot password");
        return true;
    }
    return false;
}

bool NetworkApp::onEvent(const Event &ev)
{
    switch (ev.type) {
    case EventType::WifiScanDone:
        if (mPane == Pane::Scan)
            refreshScan();
        return true;

    case EventType::WifiState:
        shell.hideBusy();
        if (mPane == Pane::Modes)
            refreshModes();
        else if (mPane == Pane::Hotspot)
            refreshHotspot();
        return true;

    case EventType::RadioState:
        shell.hideBusy();
        refreshModes();
        return true;

    default:
        return false;
    }
}

void NetworkApp::onTick()
{
    const uint32_t now = millis();
    if (now - mLastTickMs < 1000)
        return;
    mLastTickMs = now;

    if (mPane == Pane::Scan && wifi.scanning())
        refreshScan();
    else if (mPane == Pane::Hotspot)
        refreshHotspot();
}

} // namespace pgros

#endif // PGROS
