#pragma once
//
// Diagnostics.
//
// A pager in a field has no serial console, so anything worth debugging has to
// be visible on the device itself. This screen exists for the moment when
// something is wrong and the user is nowhere near a laptop.
//
// The numbers chosen are the ones that actually predict trouble on this board:
// dropped UI events (the render loop falling behind), flush duration (the
// display starving the LoRa stack on the shared SPI bus), minimum-ever free
// heap (fragmentation creeping up), and the crash/boot-loop record.

#include "ui/App.h"
#include <stdint.h>

namespace pgros {

class DiagnosticsApp : public App
{
  public:
    AppId id() const override { return AppId::Diagnostics; }
    const char *title() const override { return "Diagnostics"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onKey(uint32_t key) override;
    void onTick() override;

  private:
    // One label pair per line, built once and only ever re-texted.
    static constexpr uint8_t kMaxLines = 16;

    void addLine(lv_obj_t *parent, const char *label);
    void setValue(uint8_t index, const char *fmt, ...);
    void refresh();

    lv_obj_t *mList = nullptr;
    lv_obj_t *mKey[kMaxLines] = {nullptr};
    lv_obj_t *mVal[kMaxLines] = {nullptr};
    uint8_t mCount = 0;

    lv_obj_t *mCrash = nullptr; // crash banner, hidden when clean

    uint32_t mLastRefreshMs = 0;
};

extern DiagnosticsApp diagnosticsApp;

} // namespace pgros
