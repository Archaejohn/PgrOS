#pragma once
//
// The thread list: every enabled channel, plus every DM that has history.
//
// This is the screen a user lands on when the device buzzes, so it optimises
// for one question -- "who wants me?" -- and answers it in the leftmost 40px
// and the rightmost 40px of each row. The middle is context.
//
// Row anatomy, 480 x 38:
//
//   +----+---------------------------------------------+---------+
//   | AB | Mesh                                        |      3m |
//   |    | KE4: heading back now                       |     (2) |
//   +----+---------------------------------------------+---------+
//     ^                    ^                                ^
//   avatar chip     title + preview            relative time + unread badge
//
// Rows are built ONCE in onCreate() and recycled. onShow() rebinds text into
// them. There is no per-row lv_obj_create() on navigation, which is what keeps
// opening this screen instant no matter how deep the history gets.
//
// Threading: UI task only. The data comes from MeshBridge::listThreads(), which
// reads ChatStore summaries -- a bounded tail read per thread, done on show and
// on a message event, never per frame.

#include "core/EventBus.h"
#include "store/ChatStore.h"
#include "ui/App.h"
#include <stdint.h>

namespace pgros {

class MessagesApp : public App
{
  public:
    AppId id() const override { return AppId::Messages; }
    const char *title() const override { return "Messages"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;

  private:
    // Five rows fit on screen; sixteen is a comfortable ceiling for how many
    // conversations a pager realistically carries, and it bounds the widget
    // pool built at boot.
    static constexpr uint8_t kMaxThreads = 16;

    struct RowView {
        lv_obj_t *obj = nullptr;
        lv_obj_t *avatar = nullptr;
        lv_obj_t *initials = nullptr;
        lv_obj_t *title = nullptr;
        lv_obj_t *preview = nullptr;
        lv_obj_t *time = nullptr;
        lv_obj_t *badge = nullptr;
        lv_obj_t *badgeLabel = nullptr;
    };

    void buildList(lv_obj_t *parent);
    void buildEmptyState(lv_obj_t *parent);

    void reload();
    void bindRow(uint8_t index);
    void applySelection(bool scroll);
    void moveSelection(int8_t delta);
    void openSelected();

    lv_obj_t *mList = nullptr;
    lv_obj_t *mEmpty = nullptr;

    RowView mRows[kMaxThreads];
    ThreadSummary mThreads[kMaxThreads];
    uint8_t mCount = 0;
    uint8_t mSelected = 0;
};

extern MessagesApp messagesApp;

} // namespace pgros
