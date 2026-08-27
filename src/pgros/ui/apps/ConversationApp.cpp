#ifdef PGROS
//
// Conversation implementation. UI TASK ONLY.
//
// ---------------------------------------------------------------------------
// Bubble sizing
// ---------------------------------------------------------------------------
// Theme::styleBubble caps a bubble at metrics::bubbleMaxPct of the parent, but
// a label whose width is LV_SIZE_CONTENT never wraps, and a label with a fixed
// width makes "ok" as wide as a paragraph. So the body text is measured with
// lv_text_get_size() against the maximum content width and the label is then
// set to the width of its own longest line. Bubbles hug their text and still
// wrap at 70%, which is what every phone does and what nothing does by
// accident.
//
// ---------------------------------------------------------------------------
// Sender colours
// ---------------------------------------------------------------------------
// Derived, not tabulated. The accent is converted to HSV and its hue rotated by
// a hash of the node number, keeping the accent's saturation and value. Every
// sender colour is therefore as legible against the bubble as the brand colour
// is, in either palette, and there is no hard-coded colour list to fall out of
// step with Theme.
//
// ---------------------------------------------------------------------------
// What this file must never do
// ---------------------------------------------------------------------------
// Call mesh.send(). Sending goes through service_.sendText(), which queues an
// Intent for the main task. generatePacketId() and NodeDB are not thread-safe
// and this code runs on the UI task. Same for retry.

#include "ui/apps/ConversationApp.h"

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

ConversationApp conversationApp;

namespace
{

// Anything past 2001 is an epoch second; a millis() uptime cannot reach it in
// the life of the hardware. ChatMessage carries both and distinguishes neither.
constexpr uint32_t kEpochFloor = 1000000000UL;

// Geometry. The list owns everything above the composer.
constexpr int16_t kListH = metrics::contentH - metrics::composerH;
constexpr int16_t kListPadH = metrics::padM;

// Widest a bubble's inner text may be: 70% of the list's content width, less
// the bubble's own horizontal padding.
constexpr int16_t kBubbleMaxText =
    (int16_t)(((metrics::screenW - kListPadH * 2) * metrics::bubbleMaxPct) / 100 - metrics::padM * 2);

// Width of the composer's counter column.
constexpr int16_t kCounterW = 62;

// Scratch for store reads. STATIC, not automatic: the UI task has a 12 KB stack
// and a ChatMessage is ~300 bytes, so eight of them on the stack would be a
// quarter of it in one call. loadTail() static_asserts that kPageSize fits.
constexpr size_t kScratchN = 8;
ChatMessage gScratch[kScratchN];

// Grouping window in seconds. A file-scope copy of ConversationApp's constant,
// because the helper below is not a member and the constant is private.
constexpr uint32_t kGroupWindow = 300;

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, Color colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

// Relative timestamp for a group header. Wider vocabulary than the thread list
// uses, because here it is a header with room rather than a 40px column.
void relativeTime(uint32_t rxTime, char *out, size_t outLen)
{
    if (!out || !outLen)
        return;
    out[0] = 0;

    if (rxTime < kEpochFloor) {
        snprintf(out, outLen, "earlier");
        return;
    }

    const uint32_t now = getValidTime(RTCQualityDevice);
    if (now < kEpochFloor || now < rxTime) {
        snprintf(out, outLen, "earlier");
        return;
    }

    const uint32_t age = now - rxTime;
    if (age < 45)
        snprintf(out, outLen, "now");
    else if (age < 3600)
        snprintf(out, outLen, "%um", (unsigned)(age / 60));
    else if (age < 86400)
        snprintf(out, outLen, "%uh", (unsigned)(age / 3600));
    else if (age < 2UL * 86400UL)
        snprintf(out, outLen, "yesterday");
    else if (age < 7UL * 86400UL)
        snprintf(out, outLen, "%ud", (unsigned)(age / 86400));
    else
        snprintf(out, outLen, "%uw", (unsigned)(age / (7UL * 86400UL)));
}

// A stable, well-spread colour per sender.
//
// No hard-coded palette: the accent is pulled apart into HSV and its hue
// rotated by a hash of the node number, so every result shares the accent's
// saturation and value and is therefore exactly as readable against the bubble
// as the brand colour is -- in dark and in light, without a second table to
// maintain. Twelve steps of 30 degrees is as fine as anyone can discriminate at
// 12px anyway.
lv_color_t senderColour(uint32_t nodeNum)
{
    const Color base = theme.colors().accent;
    lv_color_hsv_t hsv =
        lv_color_rgb_to_hsv((uint8_t)((base >> 16) & 0xFF), (uint8_t)((base >> 8) & 0xFF), (uint8_t)(base & 0xFF));

    // A cheap avalanche mix, so neighbouring node numbers (which are common on
    // a mesh seeded from sequential MACs) do not land on neighbouring hues.
    uint32_t h = nodeNum ? nodeNum : 1u;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;

    const uint16_t hue = (uint16_t)((hsv.h + (h % 12u) * 30u) % 360u);
    return lv_color_hsv_to_rgb(hue, hsv.s, hsv.v);
}

// True if the second message continues the group the first one started.
bool groupsWith(uint32_t aFrom, bool aOut, uint32_t aRx, uint32_t aUp, uint32_t bFrom, bool bOut, uint32_t bRx,
                uint32_t bUp)
{
    if (aFrom != bFrom || aOut != bOut)
        return false;

    // Prefer wall-clock when both records have it; fall back to uptime, which is
    // monotonic within a boot. If the two disagree about which base to use, do
    // not group -- a wrong group header is worse than an extra one.
    if (aRx >= kEpochFloor && bRx >= kEpochFloor) {
        const uint32_t d = bRx > aRx ? bRx - aRx : aRx - bRx;
        return d <= kGroupWindow;
    }
    if (aRx < kEpochFloor && bRx < kEpochFloor) {
        const uint32_t d = bUp > aUp ? bUp - aUp : aUp - bUp;
        return d <= kGroupWindow * 1000UL;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void ConversationApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    snprintf(mTitle, sizeof(mTitle), "Messages");

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);

    buildList(mRoot);
    buildEmptyState(mRoot);
    buildComposer(mRoot);
}

void ConversationApp::buildList(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    mList = lv_obj_create(parent);
    lv_obj_remove_style_all(mList);
    lv_obj_set_size(mList, metrics::screenW, kListH);
    lv_obj_set_pos(mList, 0, 0);
    lv_obj_set_style_bg_color(mList, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(mList, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(mList, kListPadH, 0);
    lv_obj_set_style_pad_ver(mList, metrics::padS, 0);
    lv_obj_set_style_pad_row(mList, metrics::padS, 0);
    lv_obj_set_flex_flow(mList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(mList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mList, LV_SCROLLBAR_MODE_OFF);

    for (uint8_t i = 0; i < kMaxRows; i++) {
        RowView &r = mRows[i];

        r.wrap = lv_obj_create(mList);
        lv_obj_remove_style_all(r.wrap);
        lv_obj_set_width(r.wrap, LV_PCT(100));
        lv_obj_set_height(r.wrap, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(r.wrap, 0, 0);
        lv_obj_set_style_pad_row(r.wrap, 1, 0);
        lv_obj_set_flex_flow(r.wrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(r.wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.wrap, LV_OBJ_FLAG_HIDDEN);

        r.head = makeLabel(r.wrap, theme.fontSmall(), p.textFaint, "");
        lv_obj_add_flag(r.head, LV_OBJ_FLAG_HIDDEN);

        r.bubble = lv_obj_create(r.wrap);
        lv_obj_remove_style_all(r.bubble);
        theme.styleBubble(r.bubble, false);
        lv_obj_set_flex_flow(r.bubble, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r.bubble, 1, 0);
        lv_obj_remove_flag(r.bubble, LV_OBJ_FLAG_SCROLLABLE);

        r.sender = makeLabel(r.bubble, theme.fontSmall(), p.accent, "");
        lv_obj_add_flag(r.sender, LV_OBJ_FLAG_HIDDEN);

        r.text = lv_label_create(r.bubble);
        lv_label_set_long_mode(r.text, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(r.text, "");

        r.meta = makeLabel(r.wrap, theme.fontSmall(), p.textDim, "");
        lv_obj_add_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
    }
}

void ConversationApp::buildEmptyState(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    mEmpty = lv_obj_create(parent);
    lv_obj_remove_style_all(mEmpty);
    lv_obj_set_size(mEmpty, metrics::screenW, kListH);
    lv_obj_set_pos(mEmpty, 0, 0);
    lv_obj_remove_flag(mEmpty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *head = makeLabel(mEmpty, theme.fontBody(), p.text, "No messages yet");
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *hint = makeLabel(mEmpty, theme.fontSmall(), p.textDim, "Type below and press Enter to send.");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 12);
}

void ConversationApp::buildComposer(lv_obj_t *parent)
{
    const Palette &p = theme.colors();

    // A flat bar with one hairline on top, matching the status bar at the other
    // end of the screen. A floating rounded input would cost 8px of the 34 we
    // have, and there is nothing to float it above.
    mComposer = lv_obj_create(parent);
    lv_obj_remove_style_all(mComposer);
    lv_obj_set_size(mComposer, metrics::screenW, metrics::composerH);
    lv_obj_set_pos(mComposer, 0, kListH);
    lv_obj_set_style_bg_color(mComposer, lv_color_hex(p.surface), 0);
    lv_obj_set_style_bg_opa(mComposer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mComposer, lv_color_hex(p.border), 0);
    lv_obj_set_style_border_width(mComposer, 1, 0);
    lv_obj_set_style_border_side(mComposer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_hor(mComposer, metrics::padM, 0);
    lv_obj_set_style_pad_ver(mComposer, 0, 0);
    lv_obj_remove_flag(mComposer, LV_OBJ_FLAG_SCROLLABLE);

    // The draft can be 233 characters and the bar is 480px wide, so the field is
    // a clipping window scrolled to its right edge. Letting LVGL do the
    // scrolling avoids hand-measuring a visible tail on every keystroke, and it
    // keeps the caret on screen for free.
    mField = lv_obj_create(mComposer);
    lv_obj_remove_style_all(mField);
    lv_obj_set_size(mField, metrics::screenW - metrics::padM * 2 - kCounterW, 20);
    lv_obj_align(mField, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(mField, 0, 0);
    lv_obj_set_scroll_dir(mField, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(mField, LV_SCROLLBAR_MODE_OFF);

    mDraftLabel = makeLabel(mField, theme.fontBody(), p.text, "");
    lv_label_set_long_mode(mDraftLabel, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(mDraftLabel, LV_ALIGN_LEFT_MID, 0, 0);

    // Placeholder is a separate label rather than recoloured inline text: LVGL
    // v9 has no lv_label_set_recolor, and two labels is cheaper than a span.
    mPlaceholder = makeLabel(mComposer, theme.fontBody(), p.textFaint, "");
    lv_obj_align(mPlaceholder, LV_ALIGN_LEFT_MID, metrics::padM, 0);

    mCounter = makeLabel(mComposer, theme.fontSmall(), p.textDim, "");
    lv_obj_set_style_text_align(mCounter, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(mCounter, LV_ALIGN_RIGHT_MID, 0, 0);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ConversationApp::onShow(const AppArgs &args)
{
    mThreadRef = args.thread;
    mThread = toThreadId(mThreadRef);
    mIsChannel = !mThread.direct;

    // Title for the status bar. threadTitle() reads Channels or resolves a peer
    // name; both are reads of mesh-owned state, done once per navigation rather
    // than per frame, and the result is copied into our own buffer immediately.
    mesh.threadTitle(mThread, mTitle, sizeof(mTitle));
    if (!mTitle[0])
        snprintf(mTitle, sizeof(mTitle), "Conversation");

    mMaxLen = mesh.maxTextLen(mThread);
    if (mMaxLen == 0 || mMaxLen > kMaxTextLen)
        mMaxLen = kMaxTextLen; // a nonsense cap must not make the composer unusable

    mStick = true;
    mExhausted = false;
    clearDraft();

    loadTail();

    // Clearing the badge is a write, so it goes through the service task like
    // every other write. The status bar clears optimistically on ThreadRead.
    service_.markRead(mThread);
}

void ConversationApp::onHide()
{
    // The picker is a transient mode, not part of the draft. Leaving it open
    // would put the grid back on screen next time in place of the messages the
    // user came to read.
    if (mEmojiOpen)
        showEmojiPicker(false);

    // The draft is deliberately kept: coming back to a half-typed message is the
    // behaviour every phone has, and losing it to an accidental Back is the
    // behaviour nobody wants. It is only cleared on send or on an explicit Back
    // with a non-empty draft.
}

// ---------------------------------------------------------------------------
// Row pool
// ---------------------------------------------------------------------------

void ConversationApp::clearRows()
{
    for (uint8_t i = 0; i < kMaxRows; i++) {
        mUsed[i] = false;
        mMeta[i] = RowMeta();
        if (mRows[i].wrap)
            lv_obj_add_flag(mRows[i].wrap, LV_OBJ_FLAG_HIDDEN);
    }
    mCount = 0;
}

int16_t ConversationApp::claimSlot()
{
    for (uint8_t i = 0; i < kMaxRows; i++)
        if (!mUsed[i]) {
            mUsed[i] = true;
            return (int16_t)i;
        }
    return -1;
}

void ConversationApp::bindRow(uint8_t slot, const ChatMessage &m)
{
    if (slot >= kMaxRows)
        return;

    const Palette &p = theme.colors();
    RowView &r = mRows[slot];
    const bool outbound = (m.flags & kFlagOutbound) != 0;

    mMeta[slot].packetId = m.packetId;
    mMeta[slot].from = m.from;
    mMeta[slot].rxTime = m.rxTime;
    mMeta[slot].uptimeMs = m.uptimeMs;
    mMeta[slot].status = (uint8_t)m.status;
    mMeta[slot].outbound = outbound;

    // --- side -------------------------------------------------------------
    // Everything in the wrap follows the bubble to its side of the screen.
    lv_obj_set_flex_align(r.wrap, LV_FLEX_ALIGN_START, outbound ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    // --- bubble skin ------------------------------------------------------
    // The two bubble styles are mutually exclusive, and Theme does not expose
    // the style objects to remove one selectively, so the slate is wiped and
    // re-applied. styleBubble sets a complete skin, so nothing is left behind.
    lv_obj_remove_style_all(r.bubble);
    theme.styleBubble(r.bubble, outbound);
    lv_obj_set_flex_flow(r.bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r.bubble, 1, 0);
    lv_obj_remove_flag(r.bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(r.bubble, LV_SCROLLBAR_MODE_OFF);

    // --- body -------------------------------------------------------------
    // textLen comes off flash and could be anything if a record was torn, so it
    // is never trusted: the text is bounded by the field, terminated by hand,
    // and only then handed to LVGL.
    char body[kMaxTextLen + 1];
    size_t len = m.textLen;
    if (len > kMaxTextLen)
        len = kMaxTextLen;
    memcpy(body, m.text, len);
    body[len] = 0;
    // A record whose length field survived but whose text did not would render
    // as trailing garbage; stop at the first NUL either way.
    len = strnlen(body, kMaxTextLen);
    body[len] = 0;
    if (!len)
        snprintf(body, sizeof(body), "%s", "(empty message)");

    lv_obj_set_style_text_color(r.text, lv_color_hex(outbound ? p.accentText : p.text), 0);
    lv_obj_set_style_text_font(r.text, theme.fontBody(), 0);
    lv_label_set_text(r.text, body);

    // Hug the text. See the header comment: measure against the 70% cap, then
    // set the label to the width of its own longest line so short messages get
    // short bubbles and long ones still wrap at 70%.
    lv_point_t size;
    lv_text_get_size(&size, body, theme.fontBody(), 0, 0, kBubbleMaxText, LV_TEXT_FLAG_NONE);
    int32_t w = size.x;
    if (w > kBubbleMaxText)
        w = kBubbleMaxText;
    if (w < 8)
        w = 8;
    lv_obj_set_width(r.text, w);
    lv_obj_set_height(r.text, LV_SIZE_CONTENT);

    // --- sender -----------------------------------------------------------
    // The product requirement: on a channel, every inbound bubble says who sent
    // it. On a DM it does not, because there are two participants and one of
    // them is holding the device.
    if (mIsChannel && !outbound) {
        const bool useShort = policy.get().showNodeShortNames;
        char who[kMaxLongName];
        const char *src = useShort ? m.senderShort : m.senderLong;
        snprintf(who, sizeof(who), "%s", src && src[0] ? src : "unknown");
        who[sizeof(who) - 1] = 0;

        lv_label_set_text(r.sender, who);
        lv_obj_set_style_text_color(r.sender, senderColour(m.from), 0);
        lv_obj_remove_flag(r.sender, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r.sender, LV_OBJ_FLAG_HIDDEN);
    }

    applyRowStatus(slot);
    lv_obj_remove_flag(r.wrap, LV_OBJ_FLAG_HIDDEN);
}

void ConversationApp::applyRowStatus(uint8_t slot)
{
    if (slot >= kMaxRows)
        return;

    RowView &r = mRows[slot];
    const RowMeta &meta = mMeta[slot];

    // Delivery state is an outbound concept. An inbound bubble arriving is its
    // own proof of delivery.
    if (!meta.outbound) {
        lv_obj_add_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const char *text = "";
    switch ((MsgStatus)meta.status) {
    case MsgStatus::Composing:
        text = "writing";
        break;
    case MsgStatus::Queued:
        text = "queued";
        break;
    case MsgStatus::Sent:
        text = LV_SYMBOL_OK;
        break;
    case MsgStatus::Delivered:
        text = LV_SYMBOL_OK LV_SYMBOL_OK;
        break;
    case MsgStatus::Read:
        // Distinct from delivered: the accent colour from statusColor() carries
        // the difference, so the glyph stays the same width and the column does
        // not jitter as a message moves from delivered to read.
        text = LV_SYMBOL_OK LV_SYMBOL_OK;
        break;
    case MsgStatus::Failed:
        // Failure has to be unmissable and has to say what to do about it. The
        // retry key is Enter on an empty composer; the composer placeholder
        // says so too, so the instruction is never more than one glance away.
        text = LV_SYMBOL_WARNING " failed";
        break;
    default:
        text = "";
        break;
    }

    if (!text[0]) {
        lv_obj_add_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(r.meta, text);
    // statusColor keeps the queued/sent/delivered/failed mapping in exactly one
    // place, and the label sits on the screen background rather than on the
    // amber bubble, where green would have nothing to work against.
    lv_obj_set_style_text_color(r.meta, lv_color_hex(theme.statusColor(meta.status)), 0);
    lv_obj_remove_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
}

void ConversationApp::reindex()
{
    for (uint8_t d = 0; d < mCount; d++) {
        const uint8_t slot = mOrder[d];
        if (slot < kMaxRows && mRows[slot].wrap)
            lv_obj_move_to_index(mRows[slot].wrap, d);
    }
}

void ConversationApp::appendMessage(const ChatMessage &m)
{
    uint8_t slot;
    if (mCount >= kMaxRows) {
        // Full. Recycle the oldest displayed row: this is a scrollback, not an
        // archive, and readBefore() can always fetch what scrolled off.
        slot = mOrder[0];
        memmove(mOrder, mOrder + 1, (size_t)(kMaxRows - 1) * sizeof(mOrder[0]));
        mCount--;
        mExhausted = false; // there is history above us again
    } else {
        const int16_t got = claimSlot();
        if (got < 0)
            return;
        slot = (uint8_t)got;
    }

    mOrder[mCount++] = slot;
    bindRow(slot, m);
    reindex();
    applyGrouping();
    setEmptyVisible(mCount == 0);
}

void ConversationApp::prependMessages(const ChatMessage *msgs, size_t count)
{
    if (!msgs || !count)
        return;

    const size_t room = (size_t)(kMaxRows - mCount);
    if (count > room) {
        // Keep the NEWEST of the older page: those are the ones adjacent to what
        // is already on screen, so the history stays contiguous.
        msgs += (count - room);
        count = room;
    }
    if (!count)
        return;

    memmove(mOrder + count, mOrder, (size_t)mCount * sizeof(mOrder[0]));
    for (size_t i = 0; i < count; i++) {
        const int16_t got = claimSlot();
        if (got < 0) {
            // Cannot happen -- room was computed from mCount -- but a half-done
            // memmove would corrupt the order array, so repair and stop.
            memmove(mOrder + i, mOrder + count, (size_t)mCount * sizeof(mOrder[0]));
            count = i;
            break;
        }
        mOrder[i] = (uint8_t)got;
        bindRow((uint8_t)got, msgs[i]);
    }
    mCount = (uint8_t)(mCount + count);

    reindex();
    applyGrouping();
    setEmptyVisible(mCount == 0);
}

void ConversationApp::applyGrouping()
{
    // Consecutive messages from the same sender, close together in time, share
    // one header and one sender label. Repeating "KE4  3m" above every line of a
    // four-line burst wastes a third of a 200px screen saying nothing new.
    for (uint8_t d = 0; d < mCount; d++) {
        const uint8_t slot = mOrder[d];
        if (slot >= kMaxRows)
            continue;
        const RowMeta &cur = mMeta[slot];

        bool startsGroup = true;
        if (d > 0) {
            const uint8_t prevSlot = mOrder[d - 1];
            if (prevSlot < kMaxRows) {
                const RowMeta &prev = mMeta[prevSlot];
                startsGroup = !groupsWith(prev.from, prev.outbound, prev.rxTime, prev.uptimeMs, cur.from, cur.outbound,
                                          cur.rxTime, cur.uptimeMs);
            }
        }

        if (startsGroup) {
            char when[16];
            relativeTime(cur.rxTime, when, sizeof(when));
            lv_label_set_text(mRows[slot].head, when);
            lv_obj_remove_flag(mRows[slot].head, LV_OBJ_FLAG_HIDDEN);
            // A row can become a group start after it was a continuation --
            // recycling the oldest slot drops whatever used to be above it --
            // so the sender label has to be restored, not just hidden below.
            if (mIsChannel && !cur.outbound)
                lv_obj_remove_flag(mRows[slot].sender, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mRows[slot].head, LV_OBJ_FLAG_HIDDEN);
            // Mid-group bubbles drop the sender name too; the group header
            // established who is speaking.
            if (mIsChannel && !cur.outbound)
                lv_obj_add_flag(mRows[slot].sender, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void ConversationApp::loadTail()
{
    static_assert(kPageSize <= kScratchN, "gScratch is too small for one page");
    clearRows();

    const size_t n = chatStore.readTail(mThread, gScratch, kPageSize);
    for (size_t i = 0; i < n; i++) {
        const int16_t slot = claimSlot();
        if (slot < 0)
            break;
        mOrder[mCount++] = (uint8_t)slot;
        bindRow((uint8_t)slot, gScratch[i]);
    }

    reindex();
    applyGrouping();
    setEmptyVisible(mCount == 0);
    scrollToBottom(false);
    mStick = true;
    refreshComposer();
}

void ConversationApp::loadOlder()
{
    if (mExhausted || !mCount || mCount >= kMaxRows)
        return;

    const uint8_t oldestSlot = mOrder[0];
    if (oldestSlot >= kMaxRows)
        return;

    // readBefore() pages on uptimeMs, which is monotonic within a boot and is
    // filled in by the store even when the clock was never set.
    const uint32_t before = mMeta[oldestSlot].uptimeMs;
    const size_t n = chatStore.readBefore(mThread, before, gScratch, kPageSize);
    if (!n) {
        mExhausted = true;
        return;
    }

    // Keep the reading position steady across the insert. Without this the view
    // jumps by the height of everything just prepended, which is exactly the
    // moment the user was reading.
    lv_obj_update_layout(mList);
    const int32_t beforeTop = lv_obj_get_scroll_top(mList);

    prependMessages(gScratch, n);

    lv_obj_update_layout(mList);
    const int32_t afterTop = lv_obj_get_scroll_top(mList);
    if (afterTop > beforeTop)
        lv_obj_scroll_to_y(mList, afterTop - beforeTop, LV_ANIM_OFF);

    if (n < kPageSize)
        mExhausted = true;
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

bool ConversationApp::atBottom() const
{
    if (!mList)
        return true;
    // A couple of pixels of slack: LVGL's scroll maths and a fractional row
    // height can leave 1px behind, and treating that as "scrolled up" would
    // silently disable auto-scroll forever.
    return lv_obj_get_scroll_bottom(mList) <= 2;
}

void ConversationApp::scrollToBottom(bool animate)
{
    if (!mList)
        return;
    lv_obj_update_layout(mList);
    const int32_t target = lv_obj_get_scroll_y(mList) + lv_obj_get_scroll_bottom(mList);
    lv_obj_scroll_to_y(mList, target, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

void ConversationApp::setEmptyVisible(bool visible)
{
    if (!mEmpty || !mList)
        return;
    if (visible) {
        lv_obj_remove_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mList, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mList, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Emoji picker
//
// A mode, not a popup. Double-tapping SYM swaps the message list for a grid of
// emoji and swaps it back, so the gesture reads the same way the emoji key on a
// phone keyboard does. The composer stays visible underneath throughout: you are
// still writing a message, and watching the draft grow is the whole point.
//
// The grid is built on first use and then kept. Around 125 cells is a few tens
// of kilobytes of LVGL objects, which is worth paying once for someone who uses
// emoji and not at all for someone who does not.
// ---------------------------------------------------------------------------

void ConversationApp::buildEmojiPicker(lv_obj_t *parent)
{
    if (mEmoji)
        return;

    const Palette &p = theme.colors();

    mEmoji = lv_obj_create(parent);
    lv_obj_remove_style_all(mEmoji);
    lv_obj_set_size(mEmoji, metrics::screenW, kListH);
    lv_obj_set_pos(mEmoji, 0, 0);
    lv_obj_set_style_bg_color(mEmoji, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(mEmoji, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(mEmoji, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mEmoji, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(mEmoji, LV_OBJ_FLAG_HIDDEN);

    mEmojiCount = emoji::count();
    if (mEmojiCount > kMaxEmojiCells)
        mEmojiCount = kMaxEmojiCells;

    for (uint16_t i = 0; i < mEmojiCount; ++i) {
        const uint16_t row = i / kEmojiCols;
        const uint16_t col = i % kEmojiCols;

        lv_obj_t *cell = lv_obj_create(mEmoji);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, kEmojiCellW, kEmojiCellH);
        lv_obj_set_pos(cell, (int16_t)(metrics::padS + col * kEmojiCellW), (int16_t)(row * kEmojiCellH));
        lv_obj_set_style_radius(cell, metrics::radiusS, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        // fontBody() carries the emoji fallback, so the glyph resolves here the
        // same way it does inside a message bubble. One code path, so the picker
        // can never offer something the bubbles cannot draw.
        lv_obj_t *lbl = lv_label_create(cell);
        lv_obj_set_style_text_font(lbl, theme.fontBody(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(p.text), 0);
        lv_label_set_text(lbl, emoji::text(i));
        lv_obj_center(lbl);

        mEmojiCell[i] = cell;
    }

    // Nothing to show is worth saying out loud; a silent empty grid looks like a
    // hang. This only happens if the upstream emote table is compiled out.
    if (mEmojiCount == 0) {
        lv_obj_t *none = lv_label_create(mEmoji);
        lv_obj_set_style_text_font(none, theme.fontBody(), 0);
        lv_obj_set_style_text_color(none, lv_color_hex(p.textFaint), 0);
        lv_label_set_text(none, "No emoji available");
        lv_obj_center(none);
    }
}

void ConversationApp::refreshEmojiSel()
{
    const Palette &p = theme.colors();
    for (uint16_t i = 0; i < mEmojiCount; ++i) {
        if (!mEmojiCell[i])
            continue;
        const bool sel = (i == mEmojiSel);
        lv_obj_set_style_bg_color(mEmojiCell[i], lv_color_hex(p.surfaceAlt), 0);
        lv_obj_set_style_bg_opa(mEmojiCell[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }

    if (mEmojiSel < mEmojiCount && mEmojiCell[mEmojiSel])
        lv_obj_scroll_to_view(mEmojiCell[mEmojiSel], LV_ANIM_ON);
}

void ConversationApp::moveEmojiSel(int16_t delta)
{
    if (!mEmojiCount)
        return;

    const int32_t next = (int32_t)mEmojiSel + delta;
    // Clamp rather than wrap. Wrapping a grid puts the cursor a screen away from
    // where the thumb expected it, and there is no visual cue that it happened.
    if (next < 0 || next >= (int32_t)mEmojiCount)
        return;

    mEmojiSel = (uint16_t)next;
    refreshEmojiSel();
}

void ConversationApp::showEmojiPicker(bool on)
{
    if (on) {
        buildEmojiPicker(mRoot);
        if (!mEmoji)
            return;
        mEmojiOpen = true;
        lv_obj_add_flag(mList, LV_OBJ_FLAG_HIDDEN);
        if (mEmpty)
            lv_obj_add_flag(mEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mEmoji, LV_OBJ_FLAG_HIDDEN);
        refreshEmojiSel();
    } else {
        mEmojiOpen = false;
        if (mEmoji)
            lv_obj_add_flag(mEmoji, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mList, LV_OBJ_FLAG_HIDDEN);
        // setEmptyVisible() owns whether the empty state belongs on screen; the
        // picker must not decide that for it.
        setEmptyVisible(mCount == 0);
    }
}

void ConversationApp::insertEmoji(uint16_t idx)
{
    const char *utf8 = emoji::text(idx);
    const size_t n = strlen(utf8);
    if (!n)
        return;

    // mMaxLen counts BYTES, because that is what the mesh payload counts. A
    // four-byte emoji costs four of the 233, and saying so honestly here beats
    // letting the send path truncate a multi-byte sequence into mojibake.
    if (mDraftLen + n > mMaxLen || mDraftLen + n > kMaxTextLen) {
        shell.toast("Message is full", 1);
        return;
    }

    memcpy(mDraft + mDraftLen, utf8, n);
    mDraftLen += (uint16_t)n;
    mDraft[mDraftLen] = 0;
    refreshComposer();
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

void ConversationApp::clearDraft()
{
    mDraft[0] = 0;
    mDraftLen = 0;
    refreshComposer();
}

void ConversationApp::refreshComposer()
{
    if (!mDraftLabel)
        return;

    const Palette &p = theme.colors();

    // The caret is a character rather than a positioned bar: measuring text to
    // place a 2px rectangle costs a text pass per keystroke, and a blinking
    // glyph reads as a cursor to everyone who has used a terminal.
    char shown[kMaxTextLen + 2];
    size_t n = mDraftLen;
    if (n > kMaxTextLen)
        n = kMaxTextLen;
    memcpy(shown, mDraft, n);
    shown[n] = mCaretOn ? '|' : ' ';
    shown[n + 1] = 0;
    lv_label_set_text(mDraftLabel, shown);

    // Placeholder, and the retry affordance. When the composer is empty and
    // something failed to send, Enter means "try that again" -- so the empty
    // composer is where that gets said.
    if (mDraftLen == 0) {
        bool anyFailed = false;
        for (uint8_t d = 0; d < mCount && !anyFailed; d++) {
            const uint8_t slot = mOrder[d];
            if (slot < kMaxRows && mMeta[slot].outbound && mMeta[slot].status == (uint8_t)MsgStatus::Failed)
                anyFailed = true;
        }

        char hint[72];
        if (anyFailed)
            snprintf(hint, sizeof(hint), "Enter to retry the failed message");
        else
            snprintf(hint, sizeof(hint), "Message %s", mTitle);
        hint[sizeof(hint) - 1] = 0;
        lv_label_set_text(mPlaceholder, hint);
        lv_obj_set_style_text_color(mPlaceholder, lv_color_hex(anyFailed ? p.error : p.textFaint), 0);
        // Sits just past the caret so the two never overlap.
        lv_obj_align(mPlaceholder, LV_ALIGN_LEFT_MID, metrics::padM + 10, 0);
        lv_obj_remove_flag(mPlaceholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mPlaceholder, LV_OBJ_FLAG_HIDDEN);
    }

    // Counter. Silent until it is worth knowing about: a "0/233" on an empty
    // composer is chrome, and there is no room on this screen for chrome.
    if (mDraftLen == 0) {
        lv_label_set_text(mCounter, "");
    } else {
        lv_label_set_text_fmt(mCounter, "%u/%u", (unsigned)mDraftLen, (unsigned)mMaxLen);
        const uint16_t left = mMaxLen > mDraftLen ? (uint16_t)(mMaxLen - mDraftLen) : 0;
        Color colour = p.textDim;
        if (left == 0)
            colour = p.error;
        else if (left <= 20)
            colour = p.warn;
        lv_obj_set_style_text_color(mCounter, lv_color_hex(colour), 0);
    }

    // Keep the tail of a long draft in view.
    lv_obj_update_layout(mComposer);
    lv_obj_scroll_to_x(mField, lv_obj_get_scroll_x(mField) + lv_obj_get_scroll_right(mField), LV_ANIM_OFF);
}

void ConversationApp::sendDraft()
{
    if (mDraftLen == 0) {
        if (!retryNewestFailed())
            shell.toast("Nothing to send", 0);
        return;
    }

    // NOT mesh.send(). service_.sendText() copies the text into an Intent and
    // hands it to the main task, which is the only task allowed to mint a packet
    // id or touch NodeDB. See core/Service.h.
    if (!service_.sendText(mThread, mDraft)) {
        // A dropped intent is a real failure the user must hear about: unlike a
        // dropped status event, nothing else will ever retry it.
        shell.toast("Send queue full, try again", 2);
        return;
    }

    // The bubble is not drawn here. MeshBridge appends the record and posts
    // MessageReceived with outbound set, and onEvent() renders it from the
    // store -- so what is on screen is always what is actually persisted.
    clearDraft();
    mStick = true;
}

bool ConversationApp::retryNewestFailed()
{
    for (int16_t d = (int16_t)mCount - 1; d >= 0; d--) {
        const uint8_t slot = mOrder[d];
        if (slot >= kMaxRows)
            continue;
        if (!mMeta[slot].outbound || mMeta[slot].status != (uint8_t)MsgStatus::Failed)
            continue;

        if (service_.retrySend(mThread, mMeta[slot].packetId))
            shell.toast("Retrying", 0);
        else
            shell.toast("Send queue full, try again", 2);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool ConversationApp::onKey(uint32_t k)
{
    // Double tap of SYM. A toggle in both directions, so the same gesture that
    // reached the emoji grid is the one that leaves it.
    if (k == key::Emoji) {
        showEmojiPicker(!mEmojiOpen);
        return true;
    }

    // While the grid is up it owns navigation and Enter. Everything below --
    // scrolling history, sending -- belongs to the conversation and would be
    // wrong here.
    if (mEmojiOpen) {
        switch (k) {
        case key::Left:
            moveEmojiSel(-1);
            return true;
        case key::Right:
            moveEmojiSel(1);
            return true;
        case key::Up:
            moveEmojiSel(-(int16_t)kEmojiCols);
            return true;
        case key::Down:
            moveEmojiSel(kEmojiCols);
            return true;
        case key::RotateCcw:
            moveEmojiSel(-1);
            return true;
        case key::RotateCw:
            moveEmojiSel(1);
            return true;

        case key::Enter:
        case key::Select:
            insertEmoji(mEmojiSel);
            // Deliberately stays open. Emoji arrive in runs far more often than
            // singly, and closing after each one would make ":) :) :)" four
            // gestures instead of one.
            return true;

        case key::Back:
        case key::Cancel:
            showEmojiPicker(false);
            return true;

        case key::Backspace:
            // Backspace belongs to the draft even here, so a mistyped emoji is
            // undone without leaving the grid. One UTF-8 character, not one
            // byte, or the remains of a four-byte sequence become mojibake.
            if (mDraftLen) {
                do {
                    mDraftLen--;
                } while (mDraftLen && ((uint8_t)mDraft[mDraftLen] & 0xC0) == 0x80);
                mDraft[mDraftLen] = 0;
                refreshComposer();
            }
            return true;

        default:
            break;
        }

        // Reaching for a letter means reaching for the keyboard. Close and type
        // it, rather than silently swallowing the keystroke.
        if (key::isPrintable(k)) {
            showEmojiPicker(false);
            // fall through to the normal insert path below
        } else {
            return true;
        }
    }

    switch (k) {
    case key::Enter:
    case key::Select:
        sendDraft();
        return true;

    case key::Backspace:
        if (mDraftLen) {
            // One UTF-8 character. An emoji is up to four bytes and deleting one
            // of them leaves an invalid sequence that renders as a placeholder
            // box and goes out over the mesh as garbage.
            do {
                mDraftLen--;
            } while (mDraftLen && ((uint8_t)mDraft[mDraftLen] & 0xC0) == 0x80);
            mDraft[mDraftLen] = 0;
            refreshComposer();
        }
        // Consumed either way: Backspace on an empty composer must not fall
        // through to the Shell and pop the screen.
        return true;

    case key::Up:
    case key::RotateCcw:
        if (mList) {
            if (lv_obj_get_scroll_top(mList) <= 0) {
                loadOlder();
            } else {
                lv_obj_scroll_by(mList, 0, metrics::listRowH, LV_ANIM_ON);
            }
            // Any deliberate move upwards means the user is reading, so stop
            // following the newest message until they come back down.
            mStick = atBottom();
        }
        return true;

    case key::Down:
    case key::RotateCw:
        if (mList) {
            lv_obj_scroll_by(mList, 0, -metrics::listRowH, LV_ANIM_ON);
            mStick = atBottom();
        }
        return true;

    case key::Back:
    case key::Cancel:
        // A non-empty draft absorbs the first Back. Losing a typed message to a
        // mis-hit key is a much worse outcome than one extra keypress.
        if (mDraftLen) {
            clearDraft();
            shell.toast("Draft cleared", 0);
            return true;
        }
        return false; // let the Shell pop

    default:
        break;
    }

    if (key::isPrintable(k)) {
        if (mDraftLen >= mMaxLen) {
            // Say why the keystroke did nothing. A composer that silently stops
            // accepting characters reads as a broken keyboard.
            shell.toast("Message is full", 1);
            return true;
        }
        // Bounded twice: by mMaxLen above, and by the buffer itself here.
        if (mDraftLen < kMaxTextLen) {
            mDraft[mDraftLen++] = (char)k;
            mDraft[mDraftLen] = 0;
            refreshComposer();
        }
        return true;
    }

    return false;
}

void ConversationApp::onTick()
{
    // The only per-tick work on this screen: blink the caret. No store reads, no
    // mesh reads, no layout.
    if (++mCaretPhase < kCaretTicks)
        return;
    mCaretPhase = 0;
    mCaretOn = !mCaretOn;

    if (!mDraftLabel)
        return;
    char shown[kMaxTextLen + 2];
    size_t n = mDraftLen;
    if (n > kMaxTextLen)
        n = kMaxTextLen;
    memcpy(shown, mDraft, n);
    shown[n] = mCaretOn ? '|' : ' ';
    shown[n + 1] = 0;
    lv_label_set_text(mDraftLabel, shown);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

bool ConversationApp::onEvent(const Event &ev)
{
    switch (ev.type) {

    case EventType::MessageReceived: {
        const ThreadId evThread = toThreadId(ev.msg.thread);
        if (!(evThread == mThread))
            return false;

        // ONE bubble, not a reload. Re-reading the whole tail on every message
        // would make a busy channel visibly stutter, and it would throw away
        // the paged-in history above.
        const bool wasAtBottom = mStick && atBottom();

        if (chatStore.readTail(mThread, gScratch, 1) == 1)
            appendMessage(gScratch[0]);

        if (wasAtBottom)
            scrollToBottom(true);

        // A message arriving while the composer is empty may be the failed one
        // coming back, and the placeholder advertises retry.
        if (mDraftLen == 0)
            refreshComposer();
        return true;
    }

    case EventType::MessageStatus: {
        const ThreadId evThread = toThreadId(ev.msg.thread);
        if (!(evThread == mThread))
            return false;

        // Patch exactly one glyph. Nothing is re-read and nothing is re-laid
        // out beyond the one label that changed.
        for (uint8_t d = 0; d < mCount; d++) {
            const uint8_t slot = mOrder[d];
            if (slot >= kMaxRows || mMeta[slot].packetId != ev.msg.packetId)
                continue;
            mMeta[slot].status = ev.msg.status;
            applyRowStatus(slot);
            if (mDraftLen == 0)
                refreshComposer(); // the retry hint may have just appeared
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

} // namespace pgros

#endif // PGROS
