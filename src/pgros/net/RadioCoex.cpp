//
// PgrOS radio coexistence -- implementation.
//
// See RadioCoex.h for the constraint this file exists to honour: on ESP32 a
// BLE <-> WiFi change cannot be done live, because NimbleBluetooth::deinit()
// releases the BT controller memory (BLEDevice::deinit(true) ->
// esp_bt_controller_mem_release()) and main-esp32.cpp releases BTDM memory at
// boot whenever the saved config says WiFi is on. There is no way back without
// a reset, so we stage the config write and restart instead of pretending.
//
// Everything that is genuinely live -- Off <-> STA <-> AP -- is done here in
// place, always through a real teardown plus a settle delay, never as a direct
// STA -> AP hop.
//
#ifdef PGROS

#include "configuration.h"

#include "NodeDB.h"
#include "main.h" // rebootAtMsec
#include "mesh/MeshService.h"

#include <WiFi.h>
#include <string.h>

#include "net/Portal.h"
#include "net/RadioCoex.h"
#include "net/WifiManager.h"

#include "core/EventBus.h"

namespace pgros
{

// Implemented in WifiManager.cpp. RadioCoex is the only permitted caller: the
// public WifiManager::startAp()/stopAp() route back through request() so the
// "one 2.4 GHz stack at a time" rule cannot be bypassed by a later caller.
namespace detail
{
bool wifiBringUpSta();
bool wifiBringUpAp();
void wifiTearDown();
} // namespace detail

RadioCoex coex;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static RadioMode toRadioMode(CoexMode m)
{
    switch (m) {
    case CoexMode::Bluetooth:
        return RadioMode::Bluetooth;
    case CoexMode::WifiStation:
        return RadioMode::WifiStation;
    case CoexMode::WifiAp:
        return RadioMode::WifiAp;
    case CoexMode::Off:
    default:
        return RadioMode::Off;
    }
}

static const char *coexModeName(CoexMode m)
{
    switch (m) {
    case CoexMode::Off:
        return "Off";
    case CoexMode::Bluetooth:
        return "Bluetooth";
    case CoexMode::WifiStation:
        return "WiFi Station";
    case CoexMode::WifiAp:
        return "WiFi AP";
    }
    return "?";
}

static const char *reasonName(CoexReason r)
{
    switch (r) {
    case CoexReason::UserRequest:
        return "user";
    case CoexReason::BootDefault:
        return "boot";
    case CoexReason::PhonePairing:
        return "pairing";
    case CoexReason::PortalStart:
        return "portal";
    case CoexReason::PowerSaving:
        return "power";
    case CoexReason::Failure:
        return "failure";
    }
    return "?";
}

static void announce(CoexMode m)
{
    if (events.ready())
        postRadioState(toRadioMode(m));
}

static void notify(const char *text, uint8_t severity)
{
    if (events.ready())
        postNotification(text, severity);
}

// ---------------------------------------------------------------------------
// RadioCoex
// ---------------------------------------------------------------------------

bool RadioCoex::begin()
{
    // Read the mode back from what is actually up, not from what we would like
    // to be up. Cheap and side-effect free: nothing here touches a radio.
    mBusy = false;
    mLastError = "";
    mLastReason = CoexReason::BootDefault;

#ifdef ARCH_ESP32
    const wifi_mode_t wm = WiFi.getMode();
    if (wm == WIFI_MODE_AP || wm == WIFI_MODE_APSTA)
        mMode = CoexMode::WifiAp;
    else if (wm == WIFI_MODE_STA)
        mMode = CoexMode::WifiStation;
    else if (config.bluetooth.enabled && !config.network.wifi_enabled)
        mMode = CoexMode::Bluetooth;
    else
        mMode = CoexMode::Off;
#else
    mMode = config.bluetooth.enabled ? CoexMode::Bluetooth : CoexMode::Off;
#endif

    LOG_INFO("Coex begin: mode=%s (cfg wifi=%d ble=%d)", modeName(), (int)config.network.wifi_enabled,
             (int)config.bluetooth.enabled);
    announce(mMode);
    return true;
}

bool RadioCoex::bleRestartSupported()
{
#ifdef ARCH_ESP32
    // BLEDevice::deinit(true) released the controller memory; the Arduino core
    // documents that as preventing reinitialisation. Nothing we can do here.
    return false;
#else
    return true;
#endif
}

bool RadioCoex::requiresReboot(CoexMode target) const
{
    if (target == mMode)
        return false;

    // Any transition that crosses the Bluetooth boundary, in either direction.
    if (mMode == CoexMode::Bluetooth || target == CoexMode::Bluetooth)
        return true;

    // Off <-> WifiStation <-> WifiAp are all live.
    return false;
}

const char *RadioCoex::modeName() const
{
    return coexModeName(mMode);
}

CoexResult RadioCoex::request(CoexMode target, CoexReason reason)
{
    if (mBusy) {
        mLastError = "transition already in flight";
        return CoexResult::Busy;
    }

    if (target == mMode) {
        mLastReason = reason;
        return CoexResult::AlreadyInMode;
    }

    if (requiresReboot(target))
        return stageRebootInto(target, reason);

    mBusy = true;
    mLastError = "";
    mLastReason = reason;

    LOG_INFO("Coex %s -> %s (%s)", modeName(), coexModeName(target), reasonName(reason));

    // Every transition goes through a real teardown. The portal goes first: it
    // holds a listening socket on the interface we are about to pull out.
    if (portal.running())
        portal.stop();

    stopWifi();
    mMode = CoexMode::Off;
    announce(mMode);

    if (target == CoexMode::Off) {
        mBusy = false;
        return CoexResult::Ok;
    }

    delay(kSettleMs);

    bool ok = false;
    switch (target) {
    case CoexMode::WifiStation:
        ok = startWifiStation();
        break;
    case CoexMode::WifiAp:
        ok = startWifiAp();
        break;
    default:
        // Unreachable: Bluetooth was handled by requiresReboot() above.
        mLastError = "unsupported live transition";
        mBusy = false;
        return CoexResult::NotPermitted;
    }

    if (!ok) {
        LOG_ERROR("Coex: failed to bring up %s (%s)", coexModeName(target), mLastError);
        stopWifi();
        mMode = CoexMode::Off;
        mLastReason = CoexReason::Failure;
        announce(mMode);
        notify("WiFi failed to start", 2);
        mBusy = false;
        return CoexResult::Failed;
    }

    mMode = target;
    announce(mMode);

    // The portal is a service of whichever WiFi mode is up; it never starts
    // itself.
    if (wifiActive())
        portal.start();

    mBusy = false;
    return CoexResult::Ok;
}

CoexResult RadioCoex::stageRebootInto(CoexMode target, CoexReason reason)
{
    // The two flags must never both be true -- that is the whole invariant.
    bool wantWifi = false;
    bool wantBle = false;

    switch (target) {
    case CoexMode::Bluetooth:
        wantBle = true;
        break;
    case CoexMode::WifiStation:
    case CoexMode::WifiAp:
        wantWifi = true;
        break;
    case CoexMode::Off:
        break;
    }

    config.network.wifi_enabled = wantWifi;
    config.bluetooth.enabled = wantBle;

    LOG_INFO("Coex: staging reboot into %s (wifi=%d ble=%d, %s)", coexModeName(target), (int)wantWifi, (int)wantBle,
             reasonName(reason));

    // Match what the stock menus do (MenuHandler::wifiToggleMenu,
    // SystemCommandsModule): reloadConfig() re-applies the radio config and
    // calls nodeDB->saveToDisk(SEGMENT_CONFIG) for us. Fall back to a bare save
    // if the mesh service is not up yet.
    if (service) {
        service->reloadConfig(SEGMENT_CONFIG);
    } else if (nodeDB) {
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    } else {
        mLastError = "no NodeDB to persist config";
        LOG_ERROR("Coex: %s", mLastError);
        return CoexResult::Failed;
    }

    mLastReason = reason;
    mLastError = "";

    // Tell the user before the lights go out, so this reads as a mode change
    // rather than a crash.
    notify(wantBle ? "Restarting for Bluetooth" : (wantWifi ? "Restarting for WiFi" : "Restarting: radios off"), 1);

    rebootAtMsec = millis() + kRebootNoticeMs;

    // Deliberately NOT updating mMode: the switch has not happened yet.
    return CoexResult::RebootScheduled;
}

bool RadioCoex::startWifiStation()
{
    if (!detail::wifiBringUpSta()) {
        mLastError = "STA interface refused to start";
        return false;
    }
    return true;
}

bool RadioCoex::startWifiAp()
{
    if (!detail::wifiBringUpAp()) {
        mLastError = "softAP refused to start";
        return false;
    }
    return true;
}

void RadioCoex::stopWifi()
{
    detail::wifiTearDown();
}

} // namespace pgros

#endif // PGROS
