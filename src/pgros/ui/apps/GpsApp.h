#pragma once
//
// Location.
//
// Everything shown here arrives on the event bus as EventType::GpsFix. The UI
// task never reads Meshtastic's GPS object directly: GPS::probe() runs on the
// main loop and contains hundreds of milliseconds of delay() while it tries
// baud rates, and touching it from here would drag that stall onto the render
// path.
//
// A GPS that is still searching should look like it is working. The satellite
// count is shown even with no fix, because "0 satellites" and "6 satellites, no
// fix yet" mean very different things to someone deciding whether to walk into
// the open.

#include "ui/App.h"

namespace pgros {

class GpsApp : public App
{
  public:
    AppId id() const override { return AppId::Gps; }
    const char *title() const override { return "Location"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;
    void onTick() override;

  private:
    void refresh();

    lv_obj_t *mCoords = nullptr;   // lat/lon, the headline
    lv_obj_t *mNoFix = nullptr;    // searching state
    lv_obj_t *mAlt = nullptr;
    lv_obj_t *mSats = nullptr;
    lv_obj_t *mAge = nullptr;
    lv_obj_t *mShare = nullptr;    // whether we broadcast position on the mesh

    int32_t mLatI = 0; // degrees * 1e7
    int32_t mLonI = 0;
    int32_t mAltM = 0;
    uint8_t mSatCount = 0;
    bool mHaveFix = false;
    uint32_t mFixAtMs = 0; // millis() of the last valid fix, 0 if never
};

extern GpsApp gpsApp;

} // namespace pgros
