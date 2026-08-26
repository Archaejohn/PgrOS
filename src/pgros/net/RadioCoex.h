#pragma once
//
// Radio coexistence.
//
// The ESP32-S3 has one 2.4 GHz radio shared between BLE and WiFi. Meshtastic
// already refuses to run both, and PgrOS keeps that guarantee: this state
// machine is the ONLY code permitted to change which radio is active.
//
//         OFF
//        /  |  \
//      BT  STA  AP
//
// ---------------------------------------------------------------------------
// IMPORTANT: on ESP32, switching between Bluetooth and WiFi REQUIRES A REBOOT.
// ---------------------------------------------------------------------------
//
// This is a platform limitation, not a design choice, and it is worth stating
// plainly because it shapes the whole UI:
//
//   * `NimbleBluetooth::deinit()` calls `BLEDevice::deinit(true)`, and that
//     `true` is `release_memory` -> `esp_bt_controller_mem_release()`. The
//     Arduino core's own comment says it "prevents reinitialization". A later
//     `BLEDevice::init()` cannot get the controller memory back.
//   * `esp32ReleaseBluetoothMemoryIfUnused()` runs at boot (main.cpp, right
//     after NodeDB loads) and releases BTDM memory outright if the saved config
//     has WiFi enabled. By the time setup() finishes, BLE is gone for that boot.
//   * `setBluetoothEnable(false)` and `NimbleBluetooth::shutdown()` are both
//     no-ops on ESP32 -- shutdown() is `#ifndef ARCH_ESP32`.
//   * Every stock Meshtastic UI that toggles these (the OLED menu, the InkHUD
//     menu, SystemCommandsModule) writes both config flags and then reboots.
//
// So PgrOS does the same, honestly: a BT <-> WiFi change is staged as a config
// write plus a clean restart. `requiresReboot()` says whether a given
// transition needs one, and the UI tells the user before it happens rather than
// appearing to hang. Fast boot is what makes this acceptable -- the pager is
// back and interactive quickly enough that a radio switch feels like a mode
// change rather than a power cycle.
//
// Transitions that do NOT need a reboot (handled live):
//   Off <-> WifiStation, Off <-> WifiAp, WifiStation <-> WifiAp
//
// Threading: request() may block or reboot, so it MUST NOT be called from the
// UI task. UI code posts an intent; the service task performs it. Mode changes
// come back as EventType::RadioState.

#include <stdint.h>

namespace pgros {

enum class CoexMode : uint8_t {
    Off = 0,     // both 2.4 GHz radios down (LoRa is separate and always available)
    Bluetooth,   // NimBLE up, WiFi down
    WifiStation, // joined an access point
    WifiAp,      // running our own AP for the web portal
};

enum class CoexResult : uint8_t {
    Ok = 0,
    AlreadyInMode,
    RebootScheduled, // config written; the device is restarting to apply it
    Busy,            // another transition is in flight
    Failed,          // the target stack refused to start
    NotPermitted,
};

enum class CoexReason : uint8_t {
    UserRequest = 0,
    BootDefault,
    PhonePairing,
    PortalStart,
    PowerSaving,
    Failure,
};

class RadioCoex
{
  public:
    // Reads the active mode back from Meshtastic's config without touching
    // either radio. Cheap; safe on the boot path.
    bool begin();

    CoexMode mode() const { return mMode; }
    bool transitioning() const { return mBusy; }

    // True if going from the current mode to `target` needs a restart.
    // The UI calls this to decide whether to show a confirmation first.
    bool requiresReboot(CoexMode target) const;

    // Request a mode. If requiresReboot(target), this writes the Meshtastic
    // config (enforcing that wifi_enabled and bluetooth.enabled are never both
    // set), schedules a restart, and returns RebootScheduled -- it does not
    // return "Ok" for something that has not happened yet.
    // NOT for the UI task.
    CoexResult request(CoexMode target, CoexReason reason = CoexReason::UserRequest);

    CoexResult enableBluetooth(CoexReason r = CoexReason::UserRequest) { return request(CoexMode::Bluetooth, r); }
    CoexResult enableWifiStation(CoexReason r = CoexReason::UserRequest) { return request(CoexMode::WifiStation, r); }
    CoexResult enableWifiAp(CoexReason r = CoexReason::PortalStart) { return request(CoexMode::WifiAp, r); }
    CoexResult allOff(CoexReason r = CoexReason::UserRequest) { return request(CoexMode::Off, r); }

    bool wifiActive() const { return mMode == CoexMode::WifiStation || mMode == CoexMode::WifiAp; }
    bool bluetoothActive() const { return mMode == CoexMode::Bluetooth; }

    const char *modeName() const;
    CoexReason lastReason() const { return mLastReason; }
    const char *lastError() const { return mLastError; }

    // Always false on ESP32. Kept as a function rather than a constant so the
    // UI asks the question instead of hardcoding the platform's answer.
    static bool bleRestartSupported();

    // How long the UI shows "restarting to switch radios" before the reset.
    static constexpr uint32_t kRebootNoticeMs = 1200;

    // Settle time between tearing down and bringing up WiFi modes, which is the
    // one pair we can switch live.
    static constexpr uint32_t kSettleMs = 250;

  private:
    // Writes config.network.wifi_enabled / config.bluetooth.enabled so they can
    // never both be true, persists, and schedules the restart.
    CoexResult stageRebootInto(CoexMode target, CoexReason reason);

    // Persists the requested mode into PgrOS policy; see the definition.
    void rememberMode(CoexMode target);

    bool startWifiStation();
    bool startWifiAp();
    void stopWifi();

    CoexMode mMode = CoexMode::Off;
    CoexReason mLastReason = CoexReason::BootDefault;
    bool mBusy = false;
    const char *mLastError = "";
};

extern RadioCoex coex;

} // namespace pgros
