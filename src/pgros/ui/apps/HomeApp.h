#pragma once
//
// The launcher.
//
// A grid of large icons would be the obvious thing, and it is wrong here: this
// device has no touchscreen. Every interaction is the QWERTY keyboard or the
// rotary encoder, so the launcher is a horizontally scrolling row of tiles
// driven by left/right (or the rotary), with the selected tile enlarged and
// named underneath. That maps one-to-one onto the hardware the user actually
// has in their hands.
//
// The clock is large and always visible, because a pager is a device people
// glance at far more often than they navigate.

#include "ui/App.h"

namespace pgros {

class HomeApp : public App
{
  public:
    AppId id() const override { return AppId::Home; }
    const char *title() const override { return "Home"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;
    void onTick() override;

    // Home is the root; Back here should do nothing rather than pop.
    bool backExitsToHome() const override { return true; }

  private:
    struct Tile {
        AppId app;
        const char *label;
        const char *glyph; // LVGL built-in symbol
    };

    void buildTiles(lv_obj_t *parent);
    void applySelection(bool animate);
    void refreshSummary();
    void refreshClock(bool force);

    static constexpr uint8_t kTileCount = 6;
    static constexpr int16_t kTileW = 64;
    static constexpr int16_t kTileGap = 10;

    lv_obj_t *mClock = nullptr;
    lv_obj_t *mDate = nullptr;
    lv_obj_t *mSummary = nullptr;
    lv_obj_t *mRow = nullptr;
    lv_obj_t *mTile[kTileCount] = {nullptr};
    lv_obj_t *mTileIcon[kTileCount] = {nullptr};
    lv_obj_t *mTileBadge[kTileCount] = {nullptr};
    lv_obj_t *mSelLabel = nullptr;

    uint8_t mSelected = 0;
    uint16_t mUnread = 0;
    uint16_t mLastClockMinute = 0xFFFF;
};

extern HomeApp homeApp;

} // namespace pgros
