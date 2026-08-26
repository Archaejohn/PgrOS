#ifdef PGROS

#include "ui/apps/GpsApp.h"

#include "configuration.h"

#include "core/Policy.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include <lvgl.h>
#include <stdio.h>

namespace pgros
{

GpsApp gpsApp;

void GpsApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);
    lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_SCROLLABLE);

    // Coordinates are the reason anyone opens this screen, so they get the
    // large font and the top of the layout.
    mCoords = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mCoords, theme.fontLarge(), 0);
    lv_obj_set_style_text_color(mCoords, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(mCoords, LV_ALIGN_TOP_LEFT, metrics::padL, metrics::padM);
    lv_label_set_text(mCoords, "--");

    mNoFix = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mNoFix, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mNoFix, lv_color_hex(theme.colors().warn), 0);
    lv_obj_align(mNoFix, LV_ALIGN_TOP_LEFT, metrics::padL, metrics::padM + 4);
    lv_label_set_text(mNoFix, "Searching for satellites");

    // Secondary figures on one row: altitude, satellites, fix age.
    mAlt = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mAlt, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mAlt, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(mAlt, LV_ALIGN_TOP_LEFT, metrics::padL, 74);

    mSats = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mSats, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mSats, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(mSats, LV_ALIGN_TOP_LEFT, 150, 74);

    mAge = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mAge, theme.fontBody(), 0);
    lv_obj_set_style_text_color(mAge, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(mAge, LV_ALIGN_TOP_LEFT, 290, 74);

    mShare = lv_label_create(mRoot);
    lv_obj_set_style_text_font(mShare, theme.fontSmall(), 0);
    lv_obj_align(mShare, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -metrics::padM);

    lv_obj_t *hint = lv_label_create(mRoot);
    lv_obj_set_style_text_font(hint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(hint, "s  toggle sharing");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -metrics::padL, -metrics::padM);
}

void GpsApp::onShow(const AppArgs &args)
{
    (void)args;
    refresh();
}

bool GpsApp::onEvent(const Event &ev)
{
    if (ev.type != EventType::GpsFix)
        return false;

    mSatCount = ev.gps.sats;
    if (ev.gps.fixValid) {
        mLatI = ev.gps.latI;
        mLonI = ev.gps.lonI;
        mAltM = ev.gps.altM;
        mHaveFix = true;
        mFixAtMs = ev.atMs;
    } else {
        // A lost fix does not erase the last known position -- that is usually
        // still the most useful thing on the screen. It goes stale via the age
        // readout instead.
        mHaveFix = false;
    }
    refresh();
    return true;
}

bool GpsApp::onKey(uint32_t k)
{
    if (k == 's' || k == 'S') {
        Policy &p = policy.get();
        p.shareLocationOnMesh = !p.shareLocationOnMesh;
        policy.markDirty();
        policy.save();
        shell.toast(p.shareLocationOnMesh ? "Sharing location on mesh" : "Location sharing off");
        refresh();
        return true;
    }
    return false;
}

void GpsApp::onTick()
{
    // Only the age readout changes without an event.
    if (mFixAtMs)
        refresh();
}

void GpsApp::refresh()
{
    char buf[64];

    const bool everHadFix = mFixAtMs != 0;

    if (everHadFix) {
        // 5 decimal places is about a metre, which is well past what this
        // receiver delivers and comfortably readable.
        snprintf(buf, sizeof(buf), "%.5f, %.5f", mLatI / 1e7, mLonI / 1e7);
        lv_label_set_text(mCoords, buf);
        lv_obj_remove_flag(mCoords, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mNoFix, LV_OBJ_FLAG_HIDDEN);

        // A stale position is shown dimmed, so it never reads as current.
        lv_obj_set_style_text_color(mCoords, lv_color_hex(mHaveFix ? theme.colors().text : theme.colors().textDim), 0);

        snprintf(buf, sizeof(buf), "%ld m", (long)mAltM);
        lv_label_set_text(mAlt, buf);

        const uint32_t ageS = (millis() - mFixAtMs) / 1000;
        if (!mHaveFix && ageS >= 60)
            snprintf(buf, sizeof(buf), "%lum ago", (unsigned long)(ageS / 60));
        else if (!mHaveFix)
            snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)ageS);
        else
            snprintf(buf, sizeof(buf), "live");
        lv_label_set_text(mAge, buf);
    } else {
        lv_obj_add_flag(mCoords, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mNoFix, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(mAlt, "");
        lv_label_set_text(mAge, "");
    }

    snprintf(buf, sizeof(buf), "%u sats", (unsigned)mSatCount);
    lv_label_set_text(mSats, buf);

    const bool sharing = policy.get().shareLocationOnMesh;
    lv_label_set_text(mShare, sharing ? "Position shared on mesh" : "Position not shared");
    lv_obj_set_style_text_color(mShare, lv_color_hex(sharing ? theme.colors().ok : theme.colors().textFaint), 0);
}

} // namespace pgros

#endif // PGROS
