#ifdef PGROS
//
// The launcher. UI TASK ONLY.
//
// Layout of the 480x200 content area:
//
//   +-------------------------------------------------------------+
//   | 14:32                                                       |  clock, 20px
//   | Tue 26 Aug                                                  |  date, 12px
//   |                                                             |
//   |    [MSG] [CON] (GPS) [NET] [SET] [DIA]                      |  tile row
//   |                 Messages                                    |  selected name
//   |                                                             |
//   | 3 unread  .  12 nodes  .  87%                               |  summary
//   +-------------------------------------------------------------+
//
// The tile row is a flex row inside a horizontally scrollable container. Six
// 64px tiles plus gaps come to 434px, so today nothing actually has to scroll,
// but the selected tile grows and lv_obj_scroll_to_view() keeps it on screen if
// a later build adds a seventh app. No scrolling here is ever driven by a
// pointer; there is none.
//
// Nothing on this screen polls. The clock repaints once a minute rather than
// sixty times a second, the unread count is recomputed only when a message
// event says it moved, and the node count and battery come from the mesh
// snapshot and the power event respectively.

#include "ui/apps/HomeApp.h"

#include "configuration.h"

#include "core/MeshBridge.h"
#include "hal/Keyboard.h"
#include "store/ChatStore.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include "gps/RTC.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace pgros
{

HomeApp homeApp;

namespace
{

constexpr int16_t kTileSelGrow = 10; // the selected tile is this much larger
constexpr int16_t kRowY = 58;
constexpr int16_t kRowH = 84;

// Battery arrives as an event, but HomeApp.h is a fixed contract with nowhere
// to park it. There is exactly one HomeApp, so file scope is the honest place
// for this rather than pretending it belongs to something else.
uint8_t gBatteryPct = 0;
bool gHaveBattery = false;
bool gCharging = false;

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, Color colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

// The tile table.
//
// HomeApp::Tile is a private nested type and this is a free function, so the
// table is described by a file-local twin of it rather than by widening the
// header's access. Same three fields, same order; the static_assert in
// onCreate() keeps the count honest.
struct TileDef {
    AppId app;
    const char *label;
    const char *glyph; // LVGL built-in symbol
};

constexpr uint8_t kTileN = 6;

const TileDef *tiles()
{
    static const TileDef kTiles[kTileN] = {
        {AppId::Messages, "Messages", LV_SYMBOL_ENVELOPE},
        {AppId::Contacts, "Contacts", LV_SYMBOL_LIST},
        {AppId::Gps, "GPS", LV_SYMBOL_GPS},
        {AppId::Network, "Network", LV_SYMBOL_WIFI},
        {AppId::Settings, "Settings", LV_SYMBOL_SETTINGS},
        {AppId::Diagnostics, "Diagnostics", LV_SYMBOL_CHARGE},
    };
    return kTiles;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void HomeApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    // The header owns the tile count; the table in this file has to match it.
    static_assert(kTileN == kTileCount, "tile table and HomeApp::kTileCount disagree");

    const Palette &p = theme.colors();

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);

    // --- clock ------------------------------------------------------------
    // Large and top left, because glancing at the time is the single most
    // common thing anyone does with a device shaped like this.
    mClock = makeLabel(mRoot, theme.fontLarge(), p.text, "--:--");
    lv_obj_align(mClock, LV_ALIGN_TOP_LEFT, metrics::padL, metrics::padS);

    mDate = makeLabel(mRoot, theme.fontSmall(), p.textDim, "");
    lv_obj_align(mDate, LV_ALIGN_TOP_LEFT, metrics::padL + 2, 34);

    buildTiles(mRoot);

    // Name of the selected tile, under the row. One label that changes, not six
    // captions under six tiles: at 12px, six captions across 480px is noise.
    mSelLabel = makeLabel(mRoot, theme.fontBody(), p.text, "");
    lv_obj_set_style_text_align(mSelLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(mSelLabel, metrics::screenW);
    lv_obj_align(mSelLabel, LV_ALIGN_TOP_MID, 0, kRowY + kRowH + 1);

    // --- summary ----------------------------------------------------------
    mSummary = makeLabel(mRoot, theme.fontSmall(), p.textDim, "");
    lv_obj_align(mSummary, LV_ALIGN_BOTTOM_LEFT, metrics::padL, -metrics::padS);

    applySelection(false);
    refreshClock(true);
    buildMesh(mRoot);
    refreshSummary();
    refreshMesh();
}

void HomeApp::buildTiles(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    mRow = lv_obj_create(parent);
    lv_obj_remove_style_all(mRow);
    lv_obj_set_size(mRow, metrics::screenW, kRowH);
    lv_obj_set_pos(mRow, 0, kRowY);
    lv_obj_set_style_pad_hor(mRow, metrics::padL, 0);
    lv_obj_set_style_pad_column(mRow, kTileGap, 0);
    lv_obj_set_flex_flow(mRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(mRow, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(mRow, LV_SCROLLBAR_MODE_OFF);

    const TileDef *t = tiles();
    for (uint8_t i = 0; i < kTileCount; i++) {
        lv_obj_t *tile = lv_obj_create(mRow);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, kTileW, kTileW);
        lv_obj_set_style_bg_color(tile, lv_color_hex(p.surface), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, metrics::radiusM, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(p.border), 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        mTile[i] = tile;

        mTileIcon[i] = makeLabel(tile, theme.fontLarge(), p.textDim, t[i].glyph);
        lv_obj_center(mTileIcon[i]);

        // Unread badge. Built for every tile so the widget tree is uniform and
        // never has to be rebuilt; only Messages ever unhides it.
        lv_obj_t *badge = lv_obj_create(tile);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, 14);
        lv_obj_set_style_min_width(badge, 14, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(p.accent), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 7, 0);
        lv_obj_set_style_pad_hor(badge, metrics::padS, 0);
        lv_obj_set_style_pad_ver(badge, 0, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(p.bg), 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 3, -4);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *bl = makeLabel(badge, theme.fontSmall(), p.accentText, "0");
        lv_obj_center(bl);
        mTileBadge[i] = badge;
    }
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void HomeApp::onShow(const AppArgs &args)
{
    (void)args;

    // Summarising threads walks the tail of each thread file. A few KB of flash
    // reads is fine here, once, on navigation. It would not be fine in onTick().
    mUnread = 0;
    ThreadSummary summaries[8];
    const size_t n = chatStore.listThreads(summaries, 8);
    for (size_t i = 0; i < n; i++) {
        const uint32_t total = (uint32_t)mUnread + summaries[i].unread;
        mUnread = total > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)total;
    }

    applySelection(false);
    refreshClock(true);
    refreshSummary();
    refreshMesh();
}

bool HomeApp::onEvent(const Event &ev)
{
    switch (ev.type) {
    case EventType::MessageReceived:
        if (ev.msg.outbound)
            return false;
        if (mUnread < 0xFFFF)
            mUnread++;
        refreshSummary();
        applySelection(false); // repaints the badge
        return true;

    case EventType::ThreadRead:
        // Recomputing the true total costs a flash walk. The badge is allowed
        // to be optimistic for the moment until the next onShow().
        mUnread = 0;
        refreshSummary();
        applySelection(false);
        return true;

    case EventType::PowerChanged:
        gHaveBattery = true;
        gBatteryPct = ev.power.percent;
        gCharging = ev.power.charging || ev.power.usbPowered;
        refreshSummary();
        return true;

    case EventType::NodeUpdated:
        refreshSummary();
        return true;

    default:
        return false;
    }
}

bool HomeApp::onKey(uint32_t k)
{
    // Left/right and the rotary both move along the row. Up/down map to the
    // same thing rather than being ignored: with one row of tiles, "any
    // direction key moves the selection" is what a hand expects.
    switch (k) {
    case key::Left:
    case key::Up:
    case key::RotateCcw:
        // Wrap. A six-item row is a loop, and wrapping saves five presses.
        mSelected = mSelected == 0 ? (uint8_t)(kTileCount - 1) : (uint8_t)(mSelected - 1);
        applySelection(true);
        return true;

    case key::Right:
    case key::Down:
    case key::RotateCw:
        mSelected = (uint8_t)((mSelected + 1) % kTileCount);
        applySelection(true);
        return true;

    case key::Enter:
    case key::Select:
        shell.push(tiles()[mSelected].app);
        return true;

    default:
        break;
    }

    // Type-ahead. There is a full QWERTY under this screen; making the user
    // arrow across the row when they already know where they are going wastes
    // the one input advantage this device has over a phone.
    if (key::isPrintable(k)) {
        const char c = (char)((k >= 'A' && k <= 'Z') ? k + 32 : k);
        const TileDef *t = tiles();
        for (uint8_t i = 0; i < kTileCount; i++) {
            char first = t[i].label[0];
            if (first >= 'A' && first <= 'Z')
                first = (char)(first + 32);
            if (first == c) {
                mSelected = i;
                applySelection(false);
                shell.push(t[i].app);
                return true;
            }
        }
    }
    return false;
}

void HomeApp::onTick()
{
    refreshClock(false);

    // Mesh coverage changes as you move, so it has to track the tick rather than
    // waiting for an event. refreshMesh() returns immediately unless the reading
    // actually changed, so this costs a comparison in the common case.
    refreshMesh();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void HomeApp::applySelection(bool animate)
{
    if (!mRow)
        return;

    const Palette &p = theme.colors();
    const TileDef *t = tiles();

    for (uint8_t i = 0; i < kTileCount; i++) {
        if (!mTile[i])
            continue;
        const bool sel = (i == mSelected);

        // Selection has to read without a hover state and without a cursor, so
        // it is carried by three things at once: size, fill and an accent edge.
        const int32_t side = sel ? (int32_t)(kTileW + kTileSelGrow) : (int32_t)kTileW;
        lv_obj_set_size(mTile[i], side, side);
        lv_obj_set_style_bg_color(mTile[i], lv_color_hex(sel ? p.surfaceAlt : p.surface), 0);
        lv_obj_set_style_border_color(mTile[i], lv_color_hex(sel ? p.accent : p.border), 0);
        lv_obj_set_style_border_width(mTile[i], sel ? 2 : 1, 0);
        if (mTileIcon[i])
            lv_obj_set_style_text_color(mTileIcon[i], lv_color_hex(sel ? p.accent : p.textDim), 0);

        // Badge: Messages only, and only when it has something to say.
        if (!mTileBadge[i])
            continue;
        if (t[i].app == AppId::Messages && mUnread > 0) {
            lv_obj_remove_flag(mTileBadge[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *lbl = lv_obj_get_child(mTileBadge[i], 0);
            if (lbl) {
                if (mUnread > 99)
                    lv_label_set_text(lbl, "99+");
                else
                    lv_label_set_text_fmt(lbl, "%u", (unsigned)mUnread);
            }
        } else {
            lv_obj_add_flag(mTileBadge[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (mSelLabel)
        lv_label_set_text(mSelLabel, t[mSelected].label);

    if (mTile[mSelected])
        lv_obj_scroll_to_view(mTile[mSelected], animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// Mesh coverage
//
// The status bar already carries these bars, but at 22 px they are for glancing
// at when you happen to look. Walking out of range is the one thing worth
// noticing WITHOUT looking for it, so Home states it at size: four bars, and
// words rather than a number, because "no mesh" reads faster than "0".
// ---------------------------------------------------------------------------

void HomeApp::buildMesh(lv_obj_t *parent)
{
    mMeshBox = lv_obj_create(parent);
    lv_obj_remove_style_all(mMeshBox);
    lv_obj_set_size(mMeshBox, 150, 26);
    lv_obj_align(mMeshBox, LV_ALIGN_TOP_RIGHT, -metrics::padL, metrics::padM);
    lv_obj_remove_flag(mMeshBox, LV_OBJ_FLAG_SCROLLABLE);

    // Ascending bars, drawn as plain rectangles. A font glyph would be at the
    // mercy of whatever Montserrat subset is compiled in.
    for (uint8_t i = 0; i < 4; ++i) {
        mMeshBar[i] = lv_obj_create(mMeshBox);
        lv_obj_remove_style_all(mMeshBar[i]);
        const int16_t h = (int16_t)(6 + i * 5);
        lv_obj_set_size(mMeshBar[i], 5, h);
        lv_obj_set_pos(mMeshBar[i], i * 8, (int16_t)(22 - h));
        lv_obj_set_style_radius(mMeshBar[i], 1, 0);
        lv_obj_set_style_bg_opa(mMeshBar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mMeshBar[i], lv_color_hex(theme.colors().border), 0);
    }

    mMeshLabel = lv_label_create(mMeshBox);
    lv_obj_set_style_text_font(mMeshLabel, theme.fontBody(), 0);
    lv_obj_align(mMeshLabel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(mMeshLabel, "");
}

void HomeApp::refreshMesh()
{
    if (!mMeshBox)
        return;

    const MeshBridge::MeshDensity d = mesh.density();

    // Only touch LVGL when something actually changed; this runs on every tick.
    if (d.bars == mMeshBars && d.activeNeighbours == mMeshDirect)
        return;
    mMeshBars = d.bars;
    mMeshDirect = d.activeNeighbours;

    const Color lit = theme.signalColor(d.bars);
    for (uint8_t i = 0; i < 4; ++i)
        lv_obj_set_style_bg_color(mMeshBar[i], lv_color_hex(i < d.bars ? lit : theme.colors().border), 0);

    // Words, not a bare count. Zero neighbours is the state that matters and it
    // should not have to be inferred from four grey bars.
    char txt[24];
    if (d.activeNeighbours == 0)
        snprintf(txt, sizeof(txt), "no mesh");
    else if (d.activeNeighbours == 1)
        snprintf(txt, sizeof(txt), "1 near");
    else
        snprintf(txt, sizeof(txt), "%u near", (unsigned)d.activeNeighbours);

    lv_label_set_text(mMeshLabel, txt);
    lv_obj_set_style_text_color(mMeshLabel,
                                lv_color_hex(d.activeNeighbours ? theme.colors().textDim : theme.colors().error), 0);
}

void HomeApp::refreshSummary()
{
    if (!mSummary)
        return;

    // Three facts, in the order a user asks for them: is anyone talking to me,
    // is the mesh alive, will it last.
    char line[80];
    int n = 0;

    if (mUnread == 0)
        n = snprintf(line, sizeof(line), "No unread");
    else if (mUnread == 1)
        n = snprintf(line, sizeof(line), "1 unread");
    else
        n = snprintf(line, sizeof(line), "%u unread", (unsigned)mUnread);
    if (n < 0 || n >= (int)sizeof(line))
        n = (int)sizeof(line) - 1;

    const unsigned nodes = (unsigned)mesh.nodeCount();
    int m = snprintf(line + n, sizeof(line) - (size_t)n, "  .  %u node%s", nodes, nodes == 1 ? "" : "s");
    if (m > 0 && n + m < (int)sizeof(line))
        n += m;

    if (gHaveBattery)
        snprintf(line + n, sizeof(line) - (size_t)n, "  .  %u%%%s", (unsigned)gBatteryPct,
                 gCharging ? " " LV_SYMBOL_CHARGE : "");
    else
        snprintf(line + n, sizeof(line) - (size_t)n, "  .  battery --");

    lv_label_set_text(mSummary, line);

    // The unread half is the actionable half, so it is the only part that ever
    // gets colour. A summary line where everything is amber says nothing.
    lv_obj_set_style_text_color(mSummary, lv_color_hex(mUnread ? theme.colors().accent : theme.colors().textDim), 0);
}

void HomeApp::refreshClock(bool force)
{
    if (!mClock)
        return;

    // getValidTime() reads the RTC's cached epoch and does not touch the I2C
    // part, so it is safe on the UI task. local=true has already applied the
    // configured timezone, which is why gmtime_r below is the correct call on
    // this value and localtime_r would double-apply the offset.
    const uint32_t now = getValidTime(RTCQualityDevice, true);
    if (now == 0) {
        if (force || mLastClockMinute != 0xFFFF) {
            mLastClockMinute = 0xFFFF;
            lv_label_set_text(mClock, "--:--");
            lv_obj_set_style_text_color(mClock, lv_color_hex(theme.colors().textFaint), 0);
            if (mDate)
                lv_label_set_text(mDate, "clock not set");
        }
        return;
    }

    const uint16_t minuteOfDay = (uint16_t)((now % 86400UL) / 60UL);
    if (!force && minuteOfDay == mLastClockMinute)
        return; // once a minute, not once a frame
    mLastClockMinute = minuteOfDay;

    lv_label_set_text_fmt(mClock, "%02u:%02u", (unsigned)(minuteOfDay / 60), (unsigned)(minuteOfDay % 60));
    lv_obj_set_style_text_color(mClock, lv_color_hex(theme.colors().text), 0);

    if (mDate) {
        const time_t t = (time_t)now;
        struct tm tmv;
        char buf[24];
        buf[0] = 0;
        if (gmtime_r(&t, &tmv)) {
            // "Tue 26 Aug". strftime keeps the day and month name tables out of
            // this file and out of every locale argument about them.
            if (strftime(buf, sizeof(buf), "%a %d %b", &tmv) == 0)
                buf[0] = 0;
        }
        lv_label_set_text(mDate, buf);
    }
}

} // namespace pgros

#endif // PGROS
