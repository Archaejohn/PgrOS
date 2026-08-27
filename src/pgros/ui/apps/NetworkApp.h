#pragma once
//
// Radios: Bluetooth, WiFi client, WiFi hotspot.
//
// This screen is where the coexistence constraint becomes the user's problem,
// so it is the screen most responsible for not being surprising.
//
// On ESP32 the Bluetooth controller cannot be handed back once released, so
// switching into or out of Bluetooth REQUIRES A REBOOT (see net/RadioCoex.h for
// the mechanics). PgrOS therefore asks first, and says why, rather than
// appearing to hang and then restarting on its own. Fast boot is what makes the
// answer "yes" reasonable.
//
// Off <-> WiFi client <-> Hotspot are live transitions and just happen.
//
// Nothing here calls coex or wifi directly: every one of those can block for
// seconds or reboot the device, and this runs on the UI task. Requests go
// through service_ and results arrive as events.

#include "ui/App.h"
#include "net/WifiManager.h"
#include <stdint.h>

namespace pgros {

class NetworkApp : public App
{
  public:
    AppId id() const override { return AppId::Network; }
    const char *title() const override { return "Network"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;
    void onTick() override;

  private:
    // Which pane is showing. A single app with panes rather than four
    // registered apps: they share all their state and navigating between them
    // should not touch the shell's nav stack.
    enum class Pane : uint8_t { Modes, Scan, Passphrase, Hotspot, Confirm };

    void buildModes(lv_obj_t *parent);
    void buildScan(lv_obj_t *parent);
    void buildPassphrase(lv_obj_t *parent);
    void buildHotspot(lv_obj_t *parent);
    void buildConfirm(lv_obj_t *parent);

    void showPane(Pane p);
    void refreshModes();
    void refreshScan();
    void refreshHotspot();
    void refreshPassphrase();

    void moveSelection(int8_t delta);
    void activate();

    // Requests the target mode, routing through the confirm pane first when the
    // transition needs a restart.
    void requestMode(uint8_t coexMode);
    void commitPendingMode();

    Pane mPane = Pane::Modes;

    lv_obj_t *mModes = nullptr;
    lv_obj_t *mScan = nullptr;
    lv_obj_t *mPass = nullptr;
    lv_obj_t *mHotspot = nullptr;
    lv_obj_t *mConfirm = nullptr;

    // Modes pane: four fixed rows plus a status line.
    static constexpr uint8_t kModeRows = 4;
    lv_obj_t *mModeRow[kModeRows] = {nullptr};
    lv_obj_t *mModeLabel[kModeRows] = {nullptr};
    lv_obj_t *mModeMark[kModeRows] = {nullptr};
    lv_obj_t *mModeStatus = nullptr;

    // Scan pane.
    lv_obj_t *mScanList = nullptr;
    lv_obj_t *mScanSpinner = nullptr;
    lv_obj_t *mScanEmpty = nullptr;
    static constexpr uint8_t kScanRows = 12;
    lv_obj_t *mScanRow[kScanRows] = {nullptr};
    lv_obj_t *mScanSsid[kScanRows] = {nullptr};
    lv_obj_t *mScanMeta[kScanRows] = {nullptr};
    ScanResult mResults[kScanRows];
    uint8_t mResultCount = 0;

    // Passphrase pane.
    lv_obj_t *mPassSsid = nullptr;
    lv_obj_t *mPassField = nullptr;
    lv_obj_t *mPassHint = nullptr;
    char mPendingSsid[kMaxSsidLen] = {0};
    char mPassBuf[kMaxPassphrase] = {0};
    uint8_t mPassLen = 0;

    // Hotspot pane.
    lv_obj_t *mApSsid = nullptr;
    lv_obj_t *mApPass = nullptr;
    lv_obj_t *mApUrl = nullptr;
    lv_obj_t *mApClients = nullptr;

    // Confirm pane.
    lv_obj_t *mConfirmText = nullptr;
    uint8_t mPendingMode = 0;
    uint8_t mConfirmChoice = 0; // 0 = cancel, 1 = proceed

    uint8_t mSelected = 0;
    uint32_t mLastTickMs = 0;
};

extern NetworkApp networkApp;

} // namespace pgros
