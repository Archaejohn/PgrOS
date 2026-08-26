#ifdef PGROS
//
// Thread list implementation. UI TASK ONLY.
//
// The two things worth reading before changing anything here:
//
//   1. The widget pool. kMaxThreads rows are created in onCreate() and never
//      destroyed. reload() only rebinds text and toggles LV_OBJ_FLAG_HIDDEN.
//      Creating rows on show would put an LVGL allocation storm on the exact
//      frame the user is waiting for.
//
//   2. lastActivity is ambiguous by design. ChatStore fills it with rxTime when
//      the clock was set at receive time, and with uptimeMs otherwise, and
//      nothing distinguishes the two at the struct level. So relativeTime()
//      tests the magnitude: anything past 2001 is an epoch, anything below it
//      is a millis() value and the honest answer is a dash, not a wrong "3m".

#include "ui/apps/MessagesApp.h"

#include "configuration.h"

#include "core/MeshBridge.h"
#include "hal/Keyboard.h"
#include "core/Policy.h"
#include "core/Service.h"
#include "store/ChatStore.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include "gps/RTC.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace pgros
{

MessagesApp messagesApp;

namespace
{

// Anything after 2001-09-09 is an epoch second. A millis() uptime would have to
// run for 34 years to reach this, so the test cannot false-positive in the life
// of the device.
constexpr uint32_t kEpochFloor = 1000000000UL;

constexpr int16_t kAvatarX = metrics::padS;
constexpr int16_t kTextX = metrics::avatarS + metrics::padM + metrics::padS;
constexpr int16_t kRightCol = 62; // width reserved for time + badge

// Relative time, sized for a 40px column: "now", "7m", "3h", "2d". Writes at
// most `outLen` bytes and always NUL-terminates.
void relativeTime(uint32_t stamp, char *out, size_t outLen)
{
    if (!out || !outLen)
        return;
    out[0] = 0;

    if (stamp < kEpochFloor) {
        // Either never, or recorded while the clock was unset. Saying nothing is
        // better than saying something false.
        snprintf(out, outLen, "%s", stamp ? "-" : "");
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

// Two-character chip text. Channels get a hash glyph plus their index so two
// channels never collapse to the same chip; DMs get the first two characters of
// the peer's short name, upper-cased.
void initialsFor(const ThreadSummary &t, char *out, size_t outLen)
{
    if (!out || outLen < 3)
        return;

    if (!t.id.direct) {
        snprintf(out, outLen, "#%u", (unsigned)t.id.channel);
        return;
    }

    // title is the peer's long name; lastSenderShort is only set once they have
    // actually said something, so prefer the title and fall back.
    const char *src = t.title[0] ? t.title : t.lastSenderShort;
    size_t n = 0;
    char buf[3] = {0, 0, 0};
    for (size_t i = 0; src && src[i] && n < 2; i++) {
        const char c = src[i];
        if (c == '!' || c == ' ')
            continue;
        buf[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (!n)
        buf[n++] = '?';
    buf[n] = 0;
    snprintf(out, outLen, "%s", buf);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, Color colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void MessagesApp::onCreate(lv_obj_t *parent)
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

void MessagesApp::buildList(lv_obj_t *parent)
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

    for (uint8_t i = 0; i < kMaxThreads; i++) {
        RowView &r = mRows[i];

        r.obj = lv_obj_create(mList);
        lv_obj_remove_style_all(r.obj);
        theme.styleListRow(r.obj);
        lv_obj_set_width(r.obj, metrics::screenW);
        // styleListRow pads 4px top and bottom; on a 38px row that leaves 28px
        // for two lines of text, which is two pixels short. Take it back.
        lv_obj_set_style_pad_ver(r.obj, metrics::padXs, 0);
        lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);

        // Avatar chip. A filled circle with initials reads as an identity at a
        // glance and costs two objects instead of a bitmap per contact.
        r.avatar = lv_obj_create(r.obj);
        lv_obj_remove_style_all(r.avatar);
        lv_obj_set_size(r.avatar, metrics::avatarS, metrics::avatarS);
        lv_obj_set_style_radius(r.avatar, metrics::avatarS / 2, 0);
        lv_obj_set_style_bg_color(r.avatar, lv_color_hex(p.surfaceAlt), 0);
        lv_obj_set_style_bg_opa(r.avatar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(r.avatar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r.avatar, LV_ALIGN_LEFT_MID, kAvatarX, 0);

        r.initials = makeLabel(r.avatar, theme.fontSmall(), p.text, "");
        lv_obj_center(r.initials);

        r.title = makeLabel(r.obj, theme.fontBody(), p.text, "");
        lv_label_set_long_mode(r.title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.title, metrics::screenW - kTextX - kRightCol - metrics::padM * 2);
        lv_obj_align(r.title, LV_ALIGN_TOP_LEFT, kTextX, 0);

        r.preview = makeLabel(r.obj, theme.fontSmall(), p.textDim, "");
        lv_label_set_long_mode(r.preview, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.preview, metrics::screenW - kTextX - kRightCol - metrics::padM * 2);
        lv_obj_align(r.preview, LV_ALIGN_TOP_LEFT, kTextX, 17);

        r.time = makeLabel(r.obj, theme.fontSmall(), p.textDim, "");
        lv_obj_set_style_text_align(r.time, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r.time, LV_ALIGN_TOP_RIGHT, 0, 0);

        r.badge = lv_obj_create(r.obj);
        lv_obj_remove_style_all(r.badge);
        lv_obj_set_size(r.badge, LV_SIZE_CONTENT, 14);
        lv_obj_set_style_min_width(r.badge, 14, 0);
        lv_obj_set_style_radius(r.badge, 7, 0);
        lv_obj_set_style_bg_color(r.badge, lv_color_hex(p.accent), 0);
        lv_obj_set_style_bg_opa(r.badge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r.badge, metrics::padS, 0);
        lv_obj_set_style_pad_ver(r.badge, 0, 0);
        lv_obj_remove_flag(r.badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r.badge, LV_ALIGN_BOTTOM_RIGHT, 0, -1);
        lv_obj_add_flag(r.badge, LV_OBJ_FLAG_HIDDEN);

        r.badgeLabel = makeLabel(r.badge, theme.fontSmall(), p.accentText, "");
        lv_obj_center(r.badgeLabel);
    }
}

void MessagesApp::buildEmptyState(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    // An empty screen with nothing on it reads as a bug. This says what has
    // happened and what the one useful key is.
    mEmpty = lv_obj_create(parent);
    lv_obj_remove_style_all(mEmpty);
    lv_obj_set_size(mEmpty, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mEmpty, 0, 0);
    lv_obj_remove_flag(mEmpty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *icon = makeLabel(mEmpty, theme.fontLarge(), p.textFaint, LV_SYMBOL_ENVELOPE);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -34);

    lv_obj_t *head = makeLabel(mEmpty, theme.fontBody(), p.text, "No conversations yet");
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *hint = makeLabel(mEmpty, theme.fontSmall(), p.textDim,
                               "Channel messages appear here as they arrive.");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *cta = makeLabel(mEmpty, theme.fontSmall(), p.accent, "Press N to start a direct message");
    lv_obj_align(cta, LV_ALIGN_CENTER, 0, 40);
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void MessagesApp::onShow(const AppArgs &args)
{
    (void)args;
    reload();
}

void MessagesApp::reload()
{
    // Remember what the user was looking at, so a refresh triggered by an
    // incoming message does not move the selection out from under them.
    ThreadId wasOn;
    bool hadSelection = false;
    if (mCount && mSelected < mCount) {
        wasOn = mThreads[mSelected].id;
        hadSelection = true;
    }

    const size_t n = mesh.listThreads(mThreads, kMaxThreads);
    mCount = n > kMaxThreads ? kMaxThreads : (uint8_t)n;

    if (hadSelection) {
        mSelected = 0;
        for (uint8_t i = 0; i < mCount; i++) {
            if (mThreads[i].id == wasOn) {
                mSelected = i;
                break;
            }
        }
    } else if (mSelected >= mCount) {
        mSelected = 0;
    }

    for (uint8_t i = 0; i < kMaxThreads; i++) {
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

void MessagesApp::bindRow(uint8_t index)
{
    if (index >= kMaxThreads || index >= mCount)
        return;

    const Palette &p = theme.colors();
    const ThreadSummary &t = mThreads[index];
    RowView &r = mRows[index];
    const bool unread = t.unread > 0;

    // --- avatar -----------------------------------------------------------
    char chip[4];
    initialsFor(t, chip, sizeof(chip));
    lv_label_set_text(r.initials, chip);

    // An unread thread gets an accent chip. This is the first thing the eye
    // lands on and it is doing the same job as the badge on the right, on
    // purpose: at a glance from a metre away only one of the two is legible.
    lv_obj_set_style_bg_color(r.avatar, lv_color_hex(unread ? p.accent : p.surfaceAlt), 0);
    lv_obj_set_style_text_color(r.initials, lv_color_hex(unread ? p.accentText : p.textDim), 0);

    // --- title ------------------------------------------------------------
    // t.title comes from ChatStore's snapshot, which for a DM is a stored name
    // rather than a live one. Nothing here touches NodeDB.
    lv_label_set_text(r.title, t.title[0] ? t.title : "(untitled)");
    lv_obj_set_style_text_color(r.title, lv_color_hex(unread ? p.text : p.textDim), 0);

    // --- preview ----------------------------------------------------------
    // Channel previews are prefixed with who said it; DM previews are not,
    // because in a DM there are only two candidates and one of them is you.
    char preview[96];
    if (!t.preview[0]) {
        snprintf(preview, sizeof(preview), "%s", t.id.direct ? "No messages yet" : "No messages on this channel yet");
        lv_obj_set_style_text_color(r.preview, lv_color_hex(p.textFaint), 0);
    } else {
        if (t.lastWasOutbound)
            snprintf(preview, sizeof(preview), "You: %s", t.preview);
        else if (!t.id.direct && t.lastSenderShort[0])
            snprintf(preview, sizeof(preview), "%s: %s", t.lastSenderShort, t.preview);
        else
            snprintf(preview, sizeof(preview), "%s", t.preview);
        lv_obj_set_style_text_color(r.preview, lv_color_hex(unread ? p.text : p.textDim), 0);
    }
    // ChatStore::preview is a fixed char[64] and is NUL-terminated by the store,
    // but this row is also fed by a flash read that could be torn, so the
    // snprintf above is the bound and the terminator below is the belt.
    preview[sizeof(preview) - 1] = 0;
    lv_label_set_text(r.preview, preview);

    // --- time -------------------------------------------------------------
    char when[12];
    relativeTime(t.lastActivity, when, sizeof(when));
    lv_label_set_text(r.time, when);
    lv_obj_set_style_text_color(r.time, lv_color_hex(unread ? p.accent : p.textDim), 0);

    // --- unread badge -----------------------------------------------------
    if (unread) {
        lv_obj_remove_flag(r.badge, LV_OBJ_FLAG_HIDDEN);
        if (t.unread > 99)
            lv_label_set_text(r.badgeLabel, "99+");
        else
            lv_label_set_text_fmt(r.badgeLabel, "%u", (unsigned)t.unread);
    } else {
        lv_obj_add_flag(r.badge, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void MessagesApp::applySelection(bool scroll)
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

void MessagesApp::moveSelection(int8_t delta)
{
    if (!mCount)
        return;

    int32_t next = (int32_t)mSelected + delta;
    // Clamp rather than wrap. In a list the user is scanning downwards, wrapping
    // from the bottom back to the top is disorienting; in the tile row on Home,
    // where the whole set is visible at once, it is not.
    if (next < 0)
        next = 0;
    if (next >= (int32_t)mCount)
        next = mCount - 1;
    if ((uint8_t)next == mSelected)
        return;

    mSelected = (uint8_t)next;
    applySelection(true);
}

void MessagesApp::openSelected()
{
    if (!mCount || mSelected >= mCount)
        return;

    AppArgs args = {};
    args.thread = toThreadRef(mThreads[mSelected].id);
    shell.push(AppId::Conversation, args);
}

// ---------------------------------------------------------------------------
// Input and events
// ---------------------------------------------------------------------------

bool MessagesApp::onKey(uint32_t k)
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

    // 'n' for a new direct message. A dedicated key rather than a list row,
    // because "New message" as row zero would cost a fifth of the screen for
    // something that is one keystroke away.
    if (k == 'n' || k == 'N') {
        shell.push(AppId::Contacts);
        return true;
    }

    // Back is deliberately not consumed: the Shell pops us to Home.
    return false;
}

bool MessagesApp::onEvent(const Event &ev)
{
    switch (ev.type) {
    case EventType::MessageReceived:
    case EventType::MessageStatus:
    case EventType::ThreadRead:
    case EventType::ChannelsChanged:
        // A full reload is a bounded tail read per thread, and it only happens
        // on an event, never on a tick. Trying to patch a single row in place
        // would have to duplicate ChatStore's unread and preview logic here.
        reload();
        return true;

    default:
        return false;
    }
}

} // namespace pgros

#endif // PGROS
