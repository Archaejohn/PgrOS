#pragma once
//
// Settings: the one screen that edits core/Policy.
//
// Layout is a single vertically scrolling list of fixed-height rows, grouped by
// section headers. There is no nested menu: on a 200px-tall content area a
// submenu costs a whole screen of context for one extra level of hierarchy, and
// the rotary encoder makes a long flat list cheap to traverse.
//
// Interaction model (deliberately modeless -- there is no "press to edit, press
// again to commit" state to get lost in):
//
//     rotary / up / down   move the selection, skipping section headers
//     left / right         change the value of the selected row
//     enter                toggle a switch, step an enum, or run an action
//     back                 leave (and flush any pending save)
//
// PERSISTENCE IS DEBOUNCED. PolicyStore::save() writes flash synchronously, and
// a rotary encoder can emit dozens of ticks a second; saving per tick would both
// stall the UI task and burn erase cycles for values the user is still scrubbing
// through. Instead every edit stamps mDirtySinceMs and the actual write happens
// from onTick() once the user has been idle for kSaveIdleMs, or immediately in
// onHide() when they leave the screen -- whichever comes first.
//
// Threading: everything here is UI task only. The only calls that touch flash
// are policy.save() (one small file, debounced) and the storage figures in the
// About section, which are sampled once per onShow() and cached rather than
// recomputed per frame -- LittleFS usedBytes() walks metadata and takes the
// shared SPI lock, so it is not a per-frame call.

#include "core/EventBus.h"
#include "ui/App.h"
#include <stdint.h>

namespace pgros {

class SettingsApp : public App
{
  public:
    AppId id() const override { return AppId::Settings; }
    const char *title() const override { return "Settings"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    void onHide() override;
    bool onKey(uint32_t key) override;
    void onTick() override;

  private:
    // Upper bound on rows; the descriptor table in the .cpp is checked against
    // this with a static_assert so the two can never drift apart.
    static constexpr uint8_t kMaxRows = 40;

    // Idle time before a changed setting is written to flash.
    static constexpr uint32_t kSaveIdleMs = 1500;

    struct RowView {
        lv_obj_t *obj = nullptr;   // the row container (or the section header)
        lv_obj_t *label = nullptr; // left-hand title
        lv_obj_t *value = nullptr; // right-hand value, null on section headers
    };

    void buildList(lv_obj_t *parent);
    void buildConfirm(lv_obj_t *parent);

    void refreshRow(uint8_t index);
    void refreshAll();
    void refreshAbout(); // only the live figures (uptime, heap)
    void applySelection(bool scroll);
    void moveSelection(int8_t delta);

    // Returns true if the row consumed the change.
    bool adjust(uint8_t index, int8_t dir); // left/right
    bool activate(uint8_t index);           // enter

    void markDirty();
    void flushIfDirty(bool force);

    void showConfirm(bool show);

    lv_obj_t *mList = nullptr;
    lv_obj_t *mConfirm = nullptr;
    lv_obj_t *mConfirmCancel = nullptr;
    lv_obj_t *mConfirmOk = nullptr;

    RowView mRows[kMaxRows];
    uint8_t mRowCount = 0;
    uint8_t mSelected = 0;

    // The pager has no arrow keys -- the only navigation is the rotary (Up/Down)
    // and Select. So a row is entered for editing, and the rotary then changes
    // the value instead of moving the selection. See onKey().
    bool mEditing = false;
    lv_obj_t *mHint = nullptr;

    bool mConfirmVisible = false;
    uint8_t mConfirmChoice = 0; // 0 = cancel, 1 = reset

    uint32_t mDirtySinceMs = 0; // 0 == nothing pending
    uint32_t mLastAboutMs = 0;

    // Sampled once per onShow(); see the note about LittleFS above.
    uint32_t mFsUsed = 0;
    uint32_t mFsTotal = 0;

    char mNodeLine[48] = {0};
};

extern SettingsApp settingsApp;

} // namespace pgros
