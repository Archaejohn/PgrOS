#ifdef PGROS
//
// Node list implementation. UI TASK ONLY.
//
// Two things worth reading before changing anything here:
//
//   1. NOTHING HERE TOUCHES NodeDB. The rows come from MeshBridge::listNodes(),
//      which copies out of a snapshot the mesh task publishes under a seqlock.
//      Walking meshNodes from this task is the exact data race MeshBridge
//      exists to prevent: the vector reallocates on insert.
//
//   2. The widget pool. kMaxNodes rows are created in onCreate() and never
//      destroyed; reload() only rebinds text and toggles LV_OBJ_FLAG_HIDDEN.
//      This screen is refreshed by NodeUpdated events, which on a busy mesh
//      arrive as fast as the throttle allows, so a reload has to be cheap.
//
// The layout, the colours and the keys deliberately mirror MessagesApp. These
// are the only two lists on the device, and learning one should be learning
// both.

#include "ui/apps/ContactsApp.h"

#include "configuration.h"

#include "core/MeshBridge.h"
#include "hal/Keyboard.h"
#include "core/Service.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include "gps/RTC.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace pgros
{

ContactsApp contactsApp;

namespace
{

// Anything past 2001 is an epoch second. last_heard is zero for a node we hold
// a record of but have never actually heard from.
constexpr uint32_t kEpochFloor = 1000000000UL;

constexpr int16_t kAvatarX = metrics::padS;
constexpr int16_t kTextX = metrics::avatarS + metrics::padM + metrics::padS;
constexpr int16_t kRightCol = 62; // width reserved for the time + signal column

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, Color colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

// Relative time, sized for a 40px column: "now", "7m", "3h", "2d". Identical
// vocabulary to the thread list, on purpose.
void relativeTime(uint32_t stamp, char *out, size_t outLen)
{
    if (!out || !outLen)
        return;
    out[0] = 0;

    if (stamp < kEpochFloor) {
        // Never heard, or heard before the clock was set. Both are honestly
        // unknown, and a dash says so without inventing a number.
        snprintf(out, outLen, "-");
        return;
    }

    const uint32_t now = getValidTime(RTCQualityDevice);
    if (now < kEpochFloor || now < stamp) {
        snprintf(out, outLen, "-");
        return;
    }

    const uint32_t age = now - stamp;
    if (age < 60)
        snprintf(out, outLen, "now");
    else if (age < 3600)
        snprintf(out, outLen, "%um", (unsigned)(age / 60));
    else if (age < 86400)
        snprintf(out, outLen, "%uh", (unsigned)(age / 3600));
    else if (age < 7UL * 86400UL)
        snprintf(out, outLen, "%ud", (unsigned)(age / 86400));
    else
        snprintf(out, outLen, "%uw", (unsigned)(age / (7UL * 86400UL)));
}

// SNR to a 0..4 bar count, so Theme::signalColor does the colouring and the
// mapping from dB to "is this link any good" lives in one place.
//
// The thresholds are LoRa thresholds, not WiFi ones: at the spreading factors
// this radio uses a packet decodes well below 0 dB SNR, so -10 dB is a working
// link and only below about -15 dB does it start failing. Colouring -5 dB as a
// problem would paint most of a healthy mesh amber.
uint8_t barsForSnr(float snr)
{
    if (snr >= 5.0f)
        return 4;
    if (snr >= 0.0f)
        return 3;
    if (snr >= -7.0f)
        return 2;
    if (snr >= -14.0f)
        return 1;
    return 0;
}

// Up to four characters of the short name, upper-cased, for the chip. Bounded
// by the destination, never by the source: shortName arrives in a snapshot that
// was itself filled from a NodeDB field of a different width.
void chipTextFor(const NodeBrief &n, char *out, size_t outLen)
{
    if (!out || outLen < 2)
        return;

    size_t w = 0;
    for (size_t i = 0; i < sizeof(n.shortName) && n.shortName[i] && w + 1 < outLen && w < 4; i++) {
        const char c = n.shortName[i];
        if (c == ' ')
            continue;
        out[w++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (!w)
        out[w++] = '?';
    out[w] = 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void ContactsApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);

    buildList(mRoot);
    buildEmptyState(mRoot);
}

void ContactsApp::buildList(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    mList = lv_obj_create(parent);
    lv_obj_remove_style_all(mList);
    lv_obj_set_size(mList, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mList, 0, 0);
    lv_obj_set_style_bg_color(mList, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(mList, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(mList, 0, 0);
    lv_obj_set_style_pad_row(mList, 0, 0);
    lv_obj_set_flex_flow(mList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(mList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mList, LV_SCROLLBAR_MODE_OFF);

    for (uint8_t i = 0; i < kMaxNodes; i++) {
        RowView &r = mRows[i];

        r.obj = lv_obj_create(mList);
        lv_obj_remove_style_all(r.obj);
        theme.styleListRow(r.obj);
        lv_obj_set_width(r.obj, metrics::screenW);
        // styleListRow pads 4px top and bottom; on a 38px row that leaves two
        // lines of text two pixels short. Take it back, same as MessagesApp.
        lv_obj_set_style_pad_ver(r.obj, metrics::padXs, 0);
        lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);

        r.avatar = lv_obj_create(r.obj);
        lv_obj_remove_style_all(r.avatar);
        lv_obj_set_size(r.avatar, metrics::avatarS, metrics::avatarS);
        lv_obj_set_style_radius(r.avatar, metrics::avatarS / 2, 0);
        lv_obj_set_style_bg_color(r.avatar, lv_color_hex(p.surfaceAlt), 0);
        lv_obj_set_style_bg_opa(r.avatar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(r.avatar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r.avatar, LV_ALIGN_LEFT_MID, kAvatarX, 0);

        r.initials = makeLabel(r.avatar, theme.fontSmall(), p.textDim, "");
        lv_obj_center(r.initials);

        r.name = makeLabel(r.obj, theme.fontBody(), p.text, "");
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.name, metrics::screenW - kTextX - kRightCol - metrics::padM * 2);
        lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, kTextX, 0);

        r.detail = makeLabel(r.obj, theme.fontSmall(), p.textDim, "");
        lv_label_set_long_mode(r.detail, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.detail, metrics::screenW - kTextX - kRightCol - metrics::padM * 2);
        lv_obj_align(r.detail, LV_ALIGN_TOP_LEFT, kTextX, 17);

        r.time = makeLabel(r.obj, theme.fontSmall(), p.textDim, "");
        lv_obj_set_style_text_align(r.time, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r.time, LV_ALIGN_TOP_RIGHT, 0, 0);

        r.signal = makeLabel(r.obj, theme.fontSmall(), p.textDim, "");
        lv_obj_set_style_text_align(r.signal, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r.signal, LV_ALIGN_TOP_RIGHT, 0, 17);
    }
}

void ContactsApp::buildEmptyState(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    // A blank screen reads as a crash. This says what has happened, why, and
    // that it is expected rather than broken.
    mEmpty = lv_obj_create(parent);
    lv_obj_remove_style_all(mEmpty);
    lv_obj_set_size(mEmpty, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mEmpty, 0, 0);
    lv_obj_remove_flag(mEmpty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *icon = makeLabel(mEmpty, theme.fontLarge(), p.textFaint, LV_SYMBOL_LIST);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -34);

    lv_obj_t *head = makeLabel(mEmpty, theme.fontBody(), p.text, "No nodes heard yet");
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *hint =
        makeLabel(mEmpty, theme.fontSmall(), p.textDim, "Nodes appear here as soon as they are heard on the mesh.");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *cta = makeLabel(mEmpty, theme.fontSmall(), p.textFaint, "This can take a few minutes after boot.");
    lv_obj_align(cta, LV_ALIGN_CENTER, 0, 40);
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void ContactsApp::onShow(const AppArgs &args)
{
    (void)args;
    mLastReloadMs = 0; // an explicit navigation always gets fresh data
    reload();
}

void ContactsApp::reload()
{
    mLastReloadMs = millis();

    // Keep the user on the node they were looking at across a refresh. A list
    // that reorders under a moving selection is unusable when the sort key is
    // "most recently heard".
    uint32_t wasOn = 0;
    if (mCount && mSelected < mCount)
        wasOn = mNodes[mSelected].num;

    // A copy of the mesh task's snapshot, already sorted most-recently-heard
    // first by refreshNodes(). Nothing below dereferences live mesh state.
    //
    // Copied straight into the member array rather than through a local: a
    // NodeBrief[48] is ~3 KB, and the UI task has a 12 KB stack that LVGL
    // rendering already recurses through.
    const size_t got = mesh.listNodes(mNodes, kMaxNodes);

    // Drop ourselves, compacting in place. "Message yourself" is not a feature
    // this device has, and our own record is always the freshest, so it would
    // otherwise sit at the top of the list forever.
    mCount = 0;
    for (size_t i = 0; i < got && i < kMaxNodes; i++) {
        if (mNodes[i].flags & kNodeSelf)
            continue;
        if (mCount != i)
            mNodes[mCount] = mNodes[i];
        mCount++;
    }

    if (wasOn) {
        mSelected = 0;
        for (uint8_t i = 0; i < mCount; i++) {
            if (mNodes[i].num == wasOn) {
                mSelected = i;
                break;
            }
        }
    } else if (mSelected >= mCount) {
        mSelected = 0;
    }

    for (uint8_t i = 0; i < kMaxNodes; i++) {
        if (i < mCount) {
            bindRow(i);
            lv_obj_remove_flag(mRows[i].obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mRows[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (mCount) {
        lv_obj_remove_flag(mList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);
    }

    applySelection(true);
}

void ContactsApp::bindRow(uint8_t index)
{
    if (index >= kMaxNodes || index >= mCount)
        return;

    const Palette &p = theme.colors();
    const NodeBrief &n = mNodes[index];
    RowView &r = mRows[index];

    // --- chip -------------------------------------------------------------
    char chip[6];
    chipTextFor(n, chip, sizeof(chip));
    lv_label_set_text(r.initials, chip);

    // A favourite gets the accent chip. It is the only per-node flag the user
    // sets deliberately, so it is the only one worth a colour.
    const bool fav = (n.flags & kNodeFavorite) != 0;
    lv_obj_set_style_bg_color(r.avatar, lv_color_hex(fav ? p.accent : p.surfaceAlt), 0);
    lv_obj_set_style_text_color(r.initials, lv_color_hex(fav ? p.accentText : p.textDim), 0);

    // --- name -------------------------------------------------------------
    char name[kMaxLongName];
    snprintf(name, sizeof(name), "%s", n.longName[0] ? n.longName : n.shortName);
    name[sizeof(name) - 1] = 0;
    if (!name[0])
        snprintf(name, sizeof(name), "!%08x", (unsigned)n.num);
    lv_label_set_text(r.name, name);

    // --- detail line ------------------------------------------------------
    // Hops first, because on a mesh it is the single most useful fact about
    // someone: it tells you whether a DM to them will actually get through.
    // Only shown when NodeDB actually knows -- has_hops_away being false means
    // "not measured", and rendering that as "direct" would be a lie.
    char detail[72];
    int w = 0;
    if (n.flags & kNodeHopsKnown) {
        if (n.hopsAway == 0)
            w = snprintf(detail, sizeof(detail), "direct");
        else
            w = snprintf(detail, sizeof(detail), "%u hop%s", (unsigned)n.hopsAway, n.hopsAway == 1 ? "" : "s");
    } else {
        w = snprintf(detail, sizeof(detail), "hops unknown");
    }
    if (w < 0 || w >= (int)sizeof(detail))
        w = (int)sizeof(detail) - 1;

    if (n.flags & kNodeViaMqtt) {
        // Via MQTT is not a radio link at all, so it changes what every other
        // figure on this row means. Worth the characters.
        const int m = snprintf(detail + w, sizeof(detail) - (size_t)w, "  .  via MQTT");
        if (m > 0 && w + m < (int)sizeof(detail))
            w += m;
    } else {
        const int m = snprintf(detail + w, sizeof(detail) - (size_t)w, "  .  !%08x", (unsigned)n.num);
        if (m > 0 && w + m < (int)sizeof(detail))
            w += m;
    }
    detail[sizeof(detail) - 1] = 0;
    lv_label_set_text(r.detail, detail);

    // --- last heard -------------------------------------------------------
    char when[12];
    relativeTime(n.lastHeard, when, sizeof(when));
    lv_label_set_text(r.time, when);
    lv_obj_set_style_text_color(r.time, lv_color_hex(p.textDim), 0);

    // --- signal -----------------------------------------------------------
    if (n.flags & kNodeSnrKnown) {
        const uint8_t bars = barsForSnr(n.snr);
        // Rounded to whole dB: a decimal is false precision at LoRa SNRs and
        // costs two more glyphs in a 62px column.
        const int db = (int)(n.snr >= 0.0f ? n.snr + 0.5f : n.snr - 0.5f);
        lv_label_set_text_fmt(r.signal, "%d dB", db);
        lv_obj_set_style_text_color(r.signal, lv_color_hex(theme.signalColor(bars)), 0);
    } else {
        lv_label_set_text(r.signal, "--");
        lv_obj_set_style_text_color(r.signal, lv_color_hex(p.textFaint), 0);
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void ContactsApp::applySelection(bool scroll)
{
    for (uint8_t i = 0; i < mCount; i++) {
        if (!mRows[i].obj)
            continue;
        if (i == mSelected)
            lv_obj_add_state(mRows[i].obj, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(mRows[i].obj, LV_STATE_CHECKED);
    }

    if (scroll && mCount && mSelected < mCount && mRows[mSelected].obj)
        lv_obj_scroll_to_view(mRows[mSelected].obj, LV_ANIM_OFF);
}

void ContactsApp::moveSelection(int8_t delta)
{
    if (!mCount)
        return;

    int32_t next = (int32_t)mSelected + delta;
    // Clamp rather than wrap, same as the thread list: wrapping from the bottom
    // of a long list back to the top is disorienting when you are scanning.
    if (next < 0)
        next = 0;
    if (next >= (int32_t)mCount)
        next = mCount - 1;
    if ((uint8_t)next == mSelected)
        return;

    mSelected = (uint8_t)next;
    applySelection(true);
}

void ContactsApp::openSelected()
{
    if (!mCount || mSelected >= mCount)
        return;

    const uint32_t peer = mNodes[mSelected].num;
    if (!peer) {
        shell.toast("That node has no address", 1);
        return;
    }

    // Opening the conversation is all this does. The thread may have no history
    // at all -- ConversationApp handles that with its empty state -- so there is
    // nothing to create here and nothing to write.
    AppArgs args = {};
    args.thread = toThreadRef(ThreadId::dm(peer));
    shell.push(AppId::Conversation, args);
}

// ---------------------------------------------------------------------------
// Input and events
// ---------------------------------------------------------------------------

bool ContactsApp::onKey(uint32_t k)
{
    switch (k) {
    case key::Up:
    case key::RotateCcw:
        moveSelection(-1);
        return true;

    case key::Down:
    case key::RotateCw:
        moveSelection(1);
        return true;

    case key::Enter:
    case key::Select:
        openSelected();
        return true;

    default:
        break;
    }

    // Back is deliberately not consumed: the Shell pops us.
    return false;
}

bool ContactsApp::onEvent(const Event &ev)
{
    if (ev.type != EventType::NodeUpdated)
        return false;

    // The mesh task republishes its snapshot before posting this, so a reload
    // here always sees fresh data. Throttled anyway: the event is already
    // rate-limited at the source, and this is the second line of defence.
    const uint32_t now = millis();
    if (mLastReloadMs && (uint32_t)(now - mLastReloadMs) < kReloadThrottleMs)
        return false;

    reload();
    return true;
}

} // namespace pgros

#endif // PGROS
