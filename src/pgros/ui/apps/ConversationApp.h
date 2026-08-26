#pragma once
//
// One conversation: bubbles above, composer below.
//
//   +-----------------------------------------------------------+  y=0
//   |                    3m                                     |
//   |  KE4                                                      |
//   |  [ heading back now, eta 20                ]              |
//   |                          [ copy that, see you ]  (v)(v)   |
//   |                                                           |
//   +-----------------------------------------------------------+  y=166
//   | on my way|                                       9/233    |  composer, 34px
//   +-----------------------------------------------------------+  y=200
//
// Four things here are load-bearing and easy to break:
//
//   1. SENDER IDENTITY ON CHANNELS. On a broadcast thread every inbound bubble
//      names who sent it, in a colour derived from the sender's node number, so
//      a busy channel is readable rather than an undifferentiated wall. On a DM
//      the sender is implicit and the label is omitted -- repeating one of two
//      names on every bubble is noise, not information.
//
//   2. THE WIDGET POOL. kMaxRows rows are created once, in onCreate(), and
//      recycled. Display order is controlled with lv_obj_move_to_index() rather
//      than by creating and destroying objects, so paging older history costs a
//      reorder, not an allocation storm.
//
//   3. SCROLL STICKINESS. The view follows the newest message ONLY while the
//      user is already at the bottom. Yanking someone back down mid-scroll
//      because a channel is busy is the single most irritating thing a chat UI
//      can do.
//
//   4. SENDING IS NOT DIRECT. Enter posts an Intent through Service; it never
//      calls MeshBridge::send(). See core/Service.h for why that is a
//      correctness requirement rather than a layering preference.

#include "core/EventBus.h"
#include "store/ChatStore.h"
#include "ui/App.h"
#include <stdint.h>

namespace pgros {

class ConversationApp : public App
{
  public:
    AppId id() const override { return AppId::Conversation; }

    // Points at mTitle, which lives as long as this object does. App.h asks for
    // a stable string, not a literal, and the Shell copies it into a label.
    const char *title() const override { return mTitle; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    void onHide() override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;
    void onTick() override;

  private:
    // How many bubbles stay resident. Roughly six screens of history, which is
    // as far back as anyone scrolls without going looking for something.
    static constexpr uint8_t kMaxRows = 28;

    // Messages fetched per store read. Sized so the scratch buffer stays a few
    // KB rather than the ~9 KB a full kMaxRows read would need -- the UI task
    // has a 12 KB stack, so this buffer is static, not automatic.
    static constexpr uint8_t kPageSize = 8;

    // Consecutive messages from the same sender inside this window share one
    // timestamp header.
    static constexpr uint32_t kGroupWindowS = 300;

    // Caret blink period, in onTick() units (onTick runs at ~5 Hz).
    static constexpr uint8_t kCaretTicks = 3;

    struct RowView {
        lv_obj_t *wrap = nullptr;   // full-width, holds header + bubble + status
        lv_obj_t *head = nullptr;   // group header: relative time
        lv_obj_t *bubble = nullptr; // the bubble itself
        lv_obj_t *sender = nullptr; // coloured sender name, channel threads only
        lv_obj_t *text = nullptr;   // message body
        lv_obj_t *meta = nullptr;   // delivery state, outbound only
    };

    // Everything the UI needs about a rendered message that is not already in
    // its labels. Deliberately small: keeping a ChatMessage per row would be
    // ~9 KB of static RAM for text we have already handed to LVGL.
    struct RowMeta {
        uint32_t packetId = 0;
        uint32_t from = 0;
        uint32_t rxTime = 0;
        uint32_t uptimeMs = 0;
        uint8_t status = 0;
        bool outbound = false;
    };

    void buildList(lv_obj_t *parent);
    void buildComposer(lv_obj_t *parent);
    void buildEmptyState(lv_obj_t *parent);

    // --- row pool ---------------------------------------------------------
    void clearRows();
    int16_t claimSlot();
    void bindRow(uint8_t slot, const ChatMessage &m);
    void appendMessage(const ChatMessage &m);
    void prependMessages(const ChatMessage *msgs, size_t count);
    void reindex();
    void applyGrouping();
    void applyRowStatus(uint8_t slot);

    // --- data -------------------------------------------------------------
    void loadTail();
    void loadOlder();

    // --- view -------------------------------------------------------------
    bool atBottom() const;
    void scrollToBottom(bool animate);
    void setEmptyVisible(bool visible);

    // --- composer ---------------------------------------------------------
    void refreshComposer();
    void clearDraft();
    void sendDraft();
    bool retryNewestFailed();

    ThreadId mThread;
    ThreadRef mThreadRef{0, 0, 0};
    bool mIsChannel = false;
    char mTitle[48] = {0};

    lv_obj_t *mList = nullptr;
    lv_obj_t *mEmpty = nullptr;
    lv_obj_t *mComposer = nullptr;
    lv_obj_t *mField = nullptr;      // horizontally scrolling clip window
    lv_obj_t *mDraftLabel = nullptr; // draft text + caret
    lv_obj_t *mPlaceholder = nullptr;
    lv_obj_t *mCounter = nullptr;

    RowView mRows[kMaxRows];
    RowMeta mMeta[kMaxRows];
    bool mUsed[kMaxRows] = {false};
    uint8_t mOrder[kMaxRows] = {0}; // display position -> pool slot
    uint8_t mCount = 0;

    char mDraft[kMaxTextLen + 1] = {0};
    uint16_t mDraftLen = 0;
    uint16_t mMaxLen = kMaxTextLen;

    bool mStick = true;      // follow the newest message
    bool mExhausted = false; // no more history above
    bool mCaretOn = true;
    uint8_t mCaretPhase = 0;
};

extern ConversationApp conversationApp;

} // namespace pgros
