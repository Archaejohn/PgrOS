#pragma once
//
// WiFi scanning, joining, and access-point mode.
//
// This class never brings the radio up or down itself -- it asks RadioCoex to
// do that, so the "never BT and WiFi at once" rule cannot be bypassed by adding
// a caller here later. WifiManager only handles what happens once WiFi is the
// active radio: scanning, credentials, association, and AP configuration.
//
// Scanning is asynchronous. scanStart() returns immediately and the results
// arrive as EventType::WifiScanDone; the UI shows a spinner in between. A
// synchronous scan takes several seconds and would freeze the interface.

#include <stdint.h>

namespace pgros {

static constexpr uint8_t kMaxScanResults = 24;
static constexpr uint8_t kMaxSsidLen = 33;     // 32 + NUL
static constexpr uint8_t kMaxPassphrase = 64;  // 63 + NUL
static constexpr uint8_t kMaxSavedNetworks = 8;

enum class WifiSecurity : uint8_t { Open = 0, Wep, WpaPsk, Wpa2Psk, Wpa2Enterprise, Wpa3Psk, Unknown };

struct ScanResult {
    char ssid[kMaxSsidLen];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    WifiSecurity security;
    bool saved; // we hold credentials for this SSID

    // 0..4 bars, derived from rssi. Keeps the mapping in one place rather than
    // scattered through the UI.
    uint8_t bars() const;
};

struct SavedNetwork {
    char ssid[kMaxSsidLen];
    char passphrase[kMaxPassphrase];
    bool autoJoin;
};

class WifiManager
{
  public:
    bool begin();

    // --- scanning --------------------------------------------------------

    // Requests WifiStation mode from RadioCoex if needed, then starts an async
    // scan. Returns false if the radio could not be obtained (e.g. a transition
    // is already in flight). NOT for the UI task -- it may block briefly on the
    // coex transition.
    bool scanStart();

    bool scanning() const { return mScanning; }

    // Copy the most recent results out. Returns the count written, sorted by
    // signal strength descending, duplicates by SSID collapsed to the strongest.
    size_t scanResults(ScanResult *out, size_t max) const;

    // --- joining ---------------------------------------------------------

    // Join an access point. Persists the credentials on success. Blocking with
    // a timeout; call from the service task. Progress arrives as
    // EventType::WifiState.
    bool join(const char *ssid, const char *passphrase, uint32_t timeoutMs = 15000);

    // Join the best saved network in range, if any.
    bool joinBestSaved(uint32_t timeoutMs = 15000);

    void disconnect();

    bool connected() const;
    const char *currentSsid() const;
    int8_t currentRssi() const;

    // Dotted-quad of our current address in whichever mode is active. Returns
    // "0.0.0.0" when there is no address.
    const char *ipAddress() const;

    // --- saved credentials ------------------------------------------------

    size_t savedNetworks(SavedNetwork *out, size_t max) const;
    bool forget(const char *ssid);
    bool forgetAll();

    // --- access point -----------------------------------------------------

    // Bring up our own AP. The SSID defaults to "PgrOS-<shortname>" and the
    // passphrase is generated once and persisted, so it stays stable across
    // reboots and can be shown as a QR code. Blocking; service task only.
    bool startAp();
    void stopAp();
    bool apRunning() const;

    const char *apSsid() const;
    const char *apPassphrase() const;
    uint8_t apClientCount() const;

    // Configure the AP identity. Persisted.
    bool setApCredentials(const char *ssid, const char *passphrase);

  private:
    void onScanComplete(int found);

    bool mScanning = false;
    uint8_t mResultCount = 0;
    ScanResult mResults[kMaxScanResults];
};

extern WifiManager wifi;

} // namespace pgros
