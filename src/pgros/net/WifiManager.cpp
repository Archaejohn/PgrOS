//
// PgrOS WiFi manager -- implementation.
//
// Almost none of this exists upstream. Meshtastic's WiFiAPClient.cpp has a
// boot-only, STA-only initWifi(); there is no C++ scan API (the only scan in
// the tree lives inside an HTTP handler, is synchronous, and silently drops
// open networks), no AP mode anywhere, and no runtime join. So:
//
//   * scanning is asynchronous -- WiFi.scanNetworks(true) plus a poll from a
//     concurrency::Periodic on the main task, finishing with EventType::
//     WifiScanDone. A synchronous scan takes seconds and would freeze the UI.
//   * open networks are kept. They are exactly the ones a user is most likely
//     to be looking at in a cafe.
//   * credentials and the AP identity are persisted by us, via SafeFile, in
//     /pgros/wifi.cfg. Meshtastic's config.network.wifi_ssid holds one network;
//     we need a list, and we must not stomp on the phone app's copy.
//   * the AP passphrase is generated ONCE and persisted, so it is stable across
//     reboots and can be shown as a QR code.
//
// This class never enables or disables the radio itself: every entry point that
// needs WiFi to be the active stack asks RadioCoex for it. The detail::wifi*
// functions at the bottom are the inverse direction -- RadioCoex calling in to
// do the actual interface work -- and are deliberately not public API.
//
#ifdef PGROS

#include "configuration.h"

#include "mesh/wifi/WiFiAPClient.h" // getWifiDisconnectReason()
#include "net/Portal.h"

#include "FSCommon.h"
#include "NodeDB.h"
#include "SafeFile.h"
#include "concurrency/Periodic.h"

#include <WiFi.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef ARCH_ESP32
#include <esp_random.h>
#endif

#include "core/EventBus.h"
#include "net/RadioCoex.h"
#include "net/WifiManager.h"

namespace pgros
{

// Defined at the bottom of this file; RadioCoex.cpp declares the same three.
// They are the only entry points allowed to touch the WiFi stack directly.
namespace detail
{
bool wifiBringUpSta();
bool wifiBringUpAp();
void wifiTearDown();
} // namespace detail

WifiManager wifi;

// ---------------------------------------------------------------------------
// persisted state
// ---------------------------------------------------------------------------

static constexpr const char *kCfgPath = "/pgros/wifi.cfg";
static constexpr uint8_t kCfgMagic0 = 'P';
static constexpr uint8_t kCfgMagic1 = 'W';
static constexpr uint8_t kCfgVersion = 1;

static constexpr uint8_t kApSsidMax = 32;      // 802.11 limit, plus NUL below
static constexpr uint8_t kApPskMin = 8;        // WPA2 minimum
static constexpr uint8_t kApGeneratedLen = 10; // generated passphrase length

static SavedNetwork sSaved[kMaxSavedNetworks];
static uint8_t sSavedCount = 0;

static char sApSsid[kMaxSsidLen] = {0};
static char sApPsk[kMaxPassphrase] = {0};

// ---------------------------------------------------------------------------
// live state
// ---------------------------------------------------------------------------

static bool sBegun = false;
static bool sApRunning = false;
static concurrency::Periodic *sPoller = nullptr;
static char sIpBuf[16] = "0.0.0.0";
static char sSsidBuf[kMaxSsidLen] = {0};

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

/// Bounded strncpy that always terminates. Every string that crosses into our
/// fixed buffers goes through this.
static void copyBounded(char *dst, size_t dstSize, const char *src, size_t srcLen)
{
    if (!dst || dstSize == 0)
        return;
    if (!src)
        srcLen = 0;
    size_t n = srcLen < (dstSize - 1) ? srcLen : (dstSize - 1);
    // Drop control characters; an SSID is attacker-controlled text that ends up
    // in log lines and in the UI.
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c != 0x7f)
            dst[w++] = (char)c;
    }
    dst[w] = 0;
}

static void copyCStr(char *dst, size_t dstSize, const char *src)
{
    copyBounded(dst, dstSize, src, src ? strlen(src) : 0);
}

static void postWifi(WifiState s, uint8_t count = 0)
{
    if (!events.ready())
        return;
    Event ev{};
    ev.type = EventType::WifiState;
    ev.atMs = millis();
    ev.wifi.state = (uint8_t)s;
    ev.wifi.count = count;
    events.post(ev);
}

static void postScanDone(uint8_t count)
{
    if (!events.ready())
        return;
    Event ev{};
    ev.type = EventType::WifiScanDone;
    ev.atMs = millis();
    ev.wifi.state = (uint8_t)WifiState::Idle;
    ev.wifi.count = count;
    events.post(ev);
}

static WifiSecurity toSecurity(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:
        return WifiSecurity::Open;
    case WIFI_AUTH_WEP:
        return WifiSecurity::Wep;
    case WIFI_AUTH_WPA_PSK:
        return WifiSecurity::WpaPsk;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return WifiSecurity::Wpa2Psk;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return WifiSecurity::Wpa2Enterprise;
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return WifiSecurity::Wpa3Psk;
#endif
    default:
        return WifiSecurity::Unknown;
    }
}

static int findSaved(const char *ssid)
{
    if (!ssid || !*ssid)
        return -1;
    for (uint8_t i = 0; i < sSavedCount; i++)
        if (strncmp(sSaved[i].ssid, ssid, kMaxSsidLen - 1) == 0)
            return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// ScanResult
// ---------------------------------------------------------------------------

uint8_t ScanResult::bars() const
{
    if (rssi >= -55)
        return 4;
    if (rssi >= -66)
        return 3;
    if (rssi >= -77)
        return 2;
    if (rssi >= -88)
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------
//
// /pgros/wifi.cfg, all little-endian, all lengths bounded on read:
//
//   0   'P' 'W'
//   2   version
//   3   savedCount        <= kMaxSavedNetworks
//   4   apSsidLen         <= 32
//   5   apPskLen          <= 63
//   6   apSsid bytes
//   +   apPsk bytes
//   +   savedCount * { ssidLen(1), pskLen(1), autoJoin(1), ssid..., psk... }
//
static bool saveConfigFile()
{
#ifdef FSCom
    FSCom.mkdir("/pgros");

    SafeFile f(kCfgPath, true /* fullAtomic -- this file is tiny */);

    uint8_t hdr[6];
    hdr[0] = kCfgMagic0;
    hdr[1] = kCfgMagic1;
    hdr[2] = kCfgVersion;
    hdr[3] = sSavedCount;
    hdr[4] = (uint8_t)strnlen(sApSsid, kApSsidMax);
    hdr[5] = (uint8_t)strnlen(sApPsk, kMaxPassphrase - 1);
    f.write(hdr, sizeof(hdr));
    f.write((const uint8_t *)sApSsid, hdr[4]);
    f.write((const uint8_t *)sApPsk, hdr[5]);

    for (uint8_t i = 0; i < sSavedCount; i++) {
        uint8_t rec[3];
        rec[0] = (uint8_t)strnlen(sSaved[i].ssid, kMaxSsidLen - 1);
        rec[1] = (uint8_t)strnlen(sSaved[i].passphrase, kMaxPassphrase - 1);
        rec[2] = sSaved[i].autoJoin ? 1 : 0;
        f.write(rec, sizeof(rec));
        f.write((const uint8_t *)sSaved[i].ssid, rec[0]);
        f.write((const uint8_t *)sSaved[i].passphrase, rec[1]);
    }

    if (!f.close()) {
        LOG_ERROR("WiFi: failed to write %s", kCfgPath);
        return false;
    }
    return true;
#else
    return false;
#endif
}

static bool loadConfigFile()
{
#ifdef FSCom
    auto file = FSCom.open(kCfgPath, FILE_O_READ);
    if (!file)
        return false;

    bool ok = false;
    uint8_t hdr[6];
    do {
        if (file.read(hdr, sizeof(hdr)) != (int)sizeof(hdr))
            break;
        if (hdr[0] != kCfgMagic0 || hdr[1] != kCfgMagic1 || hdr[2] != kCfgVersion)
            break;

        const uint8_t savedCount = hdr[3] > kMaxSavedNetworks ? kMaxSavedNetworks : hdr[3];
        const uint8_t apSsidLen = hdr[4] > kApSsidMax ? kApSsidMax : hdr[4];
        const uint8_t apPskLen = hdr[5] > (kMaxPassphrase - 1) ? (kMaxPassphrase - 1) : hdr[5];

        // Reject a header that claims lengths the file cannot hold rather than
        // reading past the end.
        if (hdr[3] > kMaxSavedNetworks || hdr[4] > kApSsidMax || hdr[5] > (kMaxPassphrase - 1)) {
            LOG_WARN("WiFi: %s header out of range, ignoring", kCfgPath);
            break;
        }

        char tmp[kMaxPassphrase];
        memset(tmp, 0, sizeof(tmp));
        if (apSsidLen && file.read((uint8_t *)tmp, apSsidLen) != (int)apSsidLen)
            break;
        copyBounded(sApSsid, sizeof(sApSsid), tmp, apSsidLen);

        memset(tmp, 0, sizeof(tmp));
        if (apPskLen && file.read((uint8_t *)tmp, apPskLen) != (int)apPskLen)
            break;
        copyBounded(sApPsk, sizeof(sApPsk), tmp, apPskLen);

        sSavedCount = 0;
        for (uint8_t i = 0; i < savedCount; i++) {
            uint8_t rec[3];
            if (file.read(rec, sizeof(rec)) != (int)sizeof(rec))
                break;
            if (rec[0] > (kMaxSsidLen - 1) || rec[1] > (kMaxPassphrase - 1))
                break;

            memset(tmp, 0, sizeof(tmp));
            if (rec[0] && file.read((uint8_t *)tmp, rec[0]) != (int)rec[0])
                break;
            copyBounded(sSaved[sSavedCount].ssid, kMaxSsidLen, tmp, rec[0]);

            memset(tmp, 0, sizeof(tmp));
            if (rec[1] && file.read((uint8_t *)tmp, rec[1]) != (int)rec[1])
                break;
            copyBounded(sSaved[sSavedCount].passphrase, kMaxPassphrase, tmp, rec[1]);

            sSaved[sSavedCount].autoJoin = rec[2] != 0;
            if (sSaved[sSavedCount].ssid[0])
                sSavedCount++;
        }
        ok = true;
    } while (false);

    file.close();
    return ok;
#else
    return false;
#endif
}

/// Generated once, then persisted. Unambiguous alphabet: no 0/O, 1/l/I.
static void generateApPassphrase(char *out, size_t outSize)
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    const size_t n = sizeof(alphabet) - 1;
    size_t want = kApGeneratedLen;
    if (want > outSize - 1)
        want = outSize - 1;
    for (size_t i = 0; i < want; i++) {
#ifdef ARCH_ESP32
        out[i] = alphabet[esp_random() % n];
#else
        out[i] = alphabet[(uint32_t)random(0, (long)n)];
#endif
    }
    out[want] = 0;
}

/// "PgrOS-<shortname>", falling back to the MAC tail when the node has no short
/// name yet (NodeDB may not have loaded on the very first boot).
static void defaultApSsid(char *out, size_t outSize)
{
    char tag[8];
    memset(tag, 0, sizeof(tag));
    copyCStr(tag, sizeof(tag), owner.short_name);

    // Strip anything that would look wrong in a phone's WiFi list.
    char clean[8];
    size_t w = 0;
    for (size_t i = 0; tag[i] && w < sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (isalnum(c) || c == '-' || c == '_')
            clean[w++] = (char)c;
    }
    clean[w] = 0;

    if (w == 0) {
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        snprintf(clean, sizeof(clean), "%02x%02x", mac[4], mac[5]);
    }
    snprintf(out, outSize, "PgrOS-%s", clean);
}

// ---------------------------------------------------------------------------
// WifiManager
// ---------------------------------------------------------------------------

bool WifiManager::begin()
{
    if (sBegun)
        return true;

    mScanning = false;
    mResultCount = 0;
    memset(mResults, 0, sizeof(mResults));

    if (!loadConfigFile())
        LOG_INFO("WiFi: no saved config, starting fresh");

    bool dirty = false;
    if (!sApSsid[0]) {
        defaultApSsid(sApSsid, sizeof(sApSsid));
        dirty = true;
    }
    if (strnlen(sApPsk, kMaxPassphrase) < kApPskMin) {
        generateApPassphrase(sApPsk, sizeof(sApPsk));
        dirty = true;
        LOG_INFO("WiFi: generated a new AP passphrase");
    }
    if (dirty)
        saveConfigFile();

    // Scan poller. A lambda declared inside a member function keeps the private
    // onScanComplete() genuinely private while still being reachable from the
    // scheduler. OSThread instances must be new'd after setup() has started,
    // which begin() is.
    if (!sPoller) {
        sPoller = new concurrency::Periodic("pgrosWifi", [this]() -> int32_t {
            if (!mScanning)
                return 5000;
            const int16_t n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING)
                return 250;
            onScanComplete((int)n);
            return 5000;
        });
    }

    LOG_INFO("WiFi: %u saved network(s), AP \"%s\"", (unsigned)sSavedCount, sApSsid);
    sBegun = true;
    return true;
}

// --- scanning --------------------------------------------------------------

bool WifiManager::scanStart()
{
    if (mScanning)
        return true;

    if (!coex.wifiActive()) {
        const CoexResult r = coex.request(CoexMode::WifiStation, CoexReason::UserRequest);
        if (r != CoexResult::Ok && r != CoexResult::AlreadyInMode) {
            LOG_WARN("WiFi: scan denied, coex result %d", (int)r);
            return false;
        }
    }

    WiFi.scanDelete();
    const int16_t r = WiFi.scanNetworks(true /* async */, true /* show hidden */);
    if (r == WIFI_SCAN_FAILED) {
        LOG_ERROR("WiFi: scan failed to start");
        postWifi(WifiState::Failed);
        return false;
    }

    mScanning = true;
    postWifi(WifiState::Scanning);
    if (sPoller)
        sPoller->setIntervalFromNow(300);
    return true;
}

void WifiManager::onScanComplete(int found)
{
    mScanning = false;
    mResultCount = 0;

    if (found < 0) {
        LOG_WARN("WiFi: scan ended with %d", found);
        WiFi.scanDelete();
        postScanDone(0);
        return;
    }

    for (int i = 0; i < found && mResultCount < kMaxScanResults; i++) {
        String ssid;
        uint8_t enc = 0;
        int32_t rssi = 0;
        uint8_t *bssid = nullptr;
        int32_t chan = 0;
        if (!WiFi.getNetworkInfo((uint8_t)i, ssid, enc, rssi, bssid, chan))
            continue;

        // Hidden networks come back with an empty SSID and cannot be joined by
        // name, so there is nothing useful to show.
        if (ssid.length() == 0)
            continue;

        char name[kMaxSsidLen];
        copyBounded(name, sizeof(name), ssid.c_str(), ssid.length());
        if (!name[0])
            continue;

        // Collapse duplicates (multi-AP networks) to the strongest BSSID.
        int existing = -1;
        for (uint8_t j = 0; j < mResultCount; j++) {
            if (strncmp(mResults[j].ssid, name, kMaxSsidLen - 1) == 0) {
                existing = (int)j;
                break;
            }
        }

        int8_t r8 = rssi < -128 ? -128 : (rssi > 0 ? 0 : (int8_t)rssi);
        if (existing >= 0) {
            if (r8 <= mResults[existing].rssi)
                continue;
        } else {
            existing = mResultCount++;
        }

        ScanResult &out = mResults[existing];
        memset(&out, 0, sizeof(out));
        memcpy(out.ssid, name, sizeof(out.ssid));
        if (bssid)
            memcpy(out.bssid, bssid, 6);
        out.rssi = r8;
        out.channel = (chan < 0 || chan > 255) ? 0 : (uint8_t)chan;
        out.security = toSecurity((wifi_auth_mode_t)enc);
        out.saved = findSaved(name) >= 0;
    }

    // Sort by signal, strongest first. Insertion sort: n <= 24.
    for (uint8_t i = 1; i < mResultCount; i++) {
        ScanResult key = mResults[i];
        int j = (int)i - 1;
        while (j >= 0 && mResults[j].rssi < key.rssi) {
            mResults[j + 1] = mResults[j];
            j--;
        }
        mResults[j + 1] = key;
    }

    WiFi.scanDelete();
    LOG_INFO("WiFi: scan found %d, kept %u", found, (unsigned)mResultCount);
    postScanDone(mResultCount);
}

size_t WifiManager::scanResults(ScanResult *out, size_t max) const
{
    if (!out || max == 0)
        return 0;
    size_t n = mResultCount < max ? mResultCount : max;
    for (size_t i = 0; i < n; i++)
        out[i] = mResults[i];
    return n;
}

// --- joining ---------------------------------------------------------------

bool WifiManager::join(const char *ssid, const char *passphrase, uint32_t timeoutMs)
{
    if (!ssid || !ssid[0])
        return false;

    char cleanSsid[kMaxSsidLen];
    copyCStr(cleanSsid, sizeof(cleanSsid), ssid);
    if (!cleanSsid[0])
        return false;

    char cleanPsk[kMaxPassphrase];
    copyCStr(cleanPsk, sizeof(cleanPsk), passphrase);

    if (coex.mode() != CoexMode::WifiStation) {
        const CoexResult r = coex.request(CoexMode::WifiStation, CoexReason::UserRequest);
        if (r == CoexResult::RebootScheduled) {
            LOG_INFO("WiFi: join deferred, rebooting into WiFi first");
            return false;
        }
        if (r != CoexResult::Ok && r != CoexResult::AlreadyInMode) {
            LOG_WARN("WiFi: join denied, coex result %d", (int)r);
            return false;
        }
    }

    if (timeoutMs < 2000)
        timeoutMs = 2000;
    if (timeoutMs > 60000)
        timeoutMs = 60000;

    LOG_INFO("WiFi: joining \"%s\"", cleanSsid);
    postWifi(WifiState::Connecting);

    WiFi.persistent(false);
    WiFi.disconnect(false, true);
    WiFi.begin(cleanSsid, cleanPsk[0] ? cleanPsk : nullptr);

    const uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        const wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED)
            break;
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            LOG_WARN("WiFi: join failed early, status %d", (int)st);
            break;
        }
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN("WiFi: join \"%s\" timed out/failed (reason %u)", cleanSsid, (unsigned)getWifiDisconnectReason());
        WiFi.disconnect(false, true);
        postWifi(WifiState::Failed);
        return false;
    }

    copyCStr(sSsidBuf, sizeof(sSsidBuf), cleanSsid);
    LOG_INFO("WiFi: joined \"%s\" as %s", cleanSsid, WiFi.localIP().toString().c_str());

    // Persist on success only, so a typo does not stick.
    int idx = findSaved(cleanSsid);
    if (idx < 0) {
        if (sSavedCount >= kMaxSavedNetworks) {
            // Evict the oldest entry; the list is small and ordered by age.
            memmove(&sSaved[0], &sSaved[1], sizeof(SavedNetwork) * (kMaxSavedNetworks - 1));
            sSavedCount = kMaxSavedNetworks - 1;
        }
        idx = sSavedCount++;
        memset(&sSaved[idx], 0, sizeof(SavedNetwork));
    }
    copyCStr(sSaved[idx].ssid, kMaxSsidLen, cleanSsid);
    copyCStr(sSaved[idx].passphrase, kMaxPassphrase, cleanPsk);
    sSaved[idx].autoJoin = true;
    saveConfigFile();

    for (uint8_t i = 0; i < mResultCount; i++)
        if (strncmp(mResults[i].ssid, cleanSsid, kMaxSsidLen - 1) == 0)
            mResults[i].saved = true;

    postWifi(WifiState::Connected);

    // A station-mode portal is still useful (the pager on someone's home LAN),
    // and RadioCoex is what owns starting it.
    if (!portal.running())
        portal.start();

    return true;
}

bool WifiManager::joinBestSaved(uint32_t timeoutMs)
{
    if (sSavedCount == 0)
        return false;

    // Prefer the current scan. If there is none, take a synchronous one -- this
    // is documented as a service-task, blocking call.
    if (mResultCount == 0 && !mScanning) {
        if (!coex.wifiActive()) {
            const CoexResult r = coex.request(CoexMode::WifiStation, CoexReason::UserRequest);
            if (r != CoexResult::Ok && r != CoexResult::AlreadyInMode)
                return false;
        }
        WiFi.scanDelete();
        const int16_t n = WiFi.scanNetworks(false, true);
        onScanComplete(n < 0 ? -1 : (int)n);
    }

    for (uint8_t i = 0; i < mResultCount; i++) {
        const int idx = findSaved(mResults[i].ssid);
        if (idx < 0 || !sSaved[idx].autoJoin)
            continue;
        if (join(sSaved[idx].ssid, sSaved[idx].passphrase, timeoutMs))
            return true;
    }
    return false;
}

void WifiManager::disconnect()
{
    sSsidBuf[0] = 0;
    WiFi.disconnect(false, true);
    postWifi(WifiState::Idle);
}

bool WifiManager::connected() const
{
    return WiFi.status() == WL_CONNECTED;
}

const char *WifiManager::currentSsid() const
{
    if (WiFi.status() == WL_CONNECTED) {
        const String s = WiFi.SSID();
        copyBounded(sSsidBuf, sizeof(sSsidBuf), s.c_str(), s.length());
    } else if (!sApRunning) {
        sSsidBuf[0] = 0;
    }
    return sSsidBuf;
}

int8_t WifiManager::currentRssi() const
{
    if (WiFi.status() != WL_CONNECTED)
        return 0;
    return WiFi.RSSI();
}

const char *WifiManager::ipAddress() const
{
    IPAddress ip;
    if (sApRunning)
        ip = WiFi.softAPIP();
    else if (WiFi.status() == WL_CONNECTED)
        ip = WiFi.localIP();
    else
        ip = IPAddress((uint32_t)0);

    snprintf(sIpBuf, sizeof(sIpBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return sIpBuf;
}

// --- saved credentials -----------------------------------------------------

size_t WifiManager::savedNetworks(SavedNetwork *out, size_t max) const
{
    if (!out || max == 0)
        return 0;
    size_t n = sSavedCount < max ? sSavedCount : max;
    for (size_t i = 0; i < n; i++)
        out[i] = sSaved[i];
    return n;
}

bool WifiManager::forget(const char *ssid)
{
    const int idx = findSaved(ssid);
    if (idx < 0)
        return false;

    for (uint8_t i = (uint8_t)idx; i + 1 < sSavedCount; i++)
        sSaved[i] = sSaved[i + 1];
    sSavedCount--;
    memset(&sSaved[sSavedCount], 0, sizeof(SavedNetwork));

    for (uint8_t i = 0; i < mResultCount; i++)
        if (strncmp(mResults[i].ssid, ssid, kMaxSsidLen - 1) == 0)
            mResults[i].saved = false;

    return saveConfigFile();
}

bool WifiManager::forgetAll()
{
    memset(sSaved, 0, sizeof(sSaved));
    sSavedCount = 0;
    for (uint8_t i = 0; i < mResultCount; i++)
        mResults[i].saved = false;
    return saveConfigFile();
}

// --- access point ----------------------------------------------------------

bool WifiManager::startAp()
{
    const CoexResult r = coex.request(CoexMode::WifiAp, CoexReason::PortalStart);
    if (r == CoexResult::Ok || r == CoexResult::AlreadyInMode)
        return sApRunning;
    LOG_WARN("WiFi: AP start denied, coex result %d", (int)r);
    return false;
}

void WifiManager::stopAp()
{
    if (coex.mode() == CoexMode::WifiAp)
        coex.allOff(CoexReason::UserRequest);
    else
        detail::wifiTearDown();
}

bool WifiManager::apRunning() const
{
    return sApRunning;
}

const char *WifiManager::apSsid() const
{
    return sApSsid;
}

const char *WifiManager::apPassphrase() const
{
    return sApPsk;
}

uint8_t WifiManager::apClientCount() const
{
    return sApRunning ? WiFi.softAPgetStationNum() : 0;
}

bool WifiManager::setApCredentials(const char *ssid, const char *passphrase)
{
    char newSsid[kMaxSsidLen];
    char newPsk[kMaxPassphrase];
    copyCStr(newSsid, sizeof(newSsid), ssid);
    copyCStr(newPsk, sizeof(newPsk), passphrase);

    if (!newSsid[0] || strnlen(newSsid, kMaxSsidLen) > kApSsidMax) {
        LOG_WARN("WiFi: rejecting AP SSID");
        return false;
    }
    // WPA2 requires 8..63; refuse to silently downgrade the AP to open.
    const size_t pskLen = strnlen(newPsk, kMaxPassphrase);
    if (pskLen < kApPskMin || pskLen > (kMaxPassphrase - 1)) {
        LOG_WARN("WiFi: rejecting AP passphrase (length %u)", (unsigned)pskLen);
        return false;
    }

    memcpy(sApSsid, newSsid, sizeof(sApSsid));
    memcpy(sApPsk, newPsk, sizeof(sApPsk));
    if (!saveConfigFile())
        return false;

    // Re-arm a running AP in place. We are already the active radio, so this
    // needs no coex transition.
    if (sApRunning) {
        WiFi.softAPdisconnect(false);
        sApRunning = false;
        delay(RadioCoex::kSettleMs);
        detail::wifiBringUpAp();
    }
    return true;
}

// ---------------------------------------------------------------------------
// detail:: -- called only by RadioCoex
// ---------------------------------------------------------------------------

namespace detail
{

bool wifiBringUpSta()
{
    sApRunning = false;
    WiFi.persistent(false); // never let the SDK keep its own copy of credentials
    if (!WiFi.mode(WIFI_STA)) {
        LOG_ERROR("WiFi: WIFI_STA mode refused");
        return false;
    }
    WiFi.setSleep(false);
    postWifi(WifiState::Idle);
    return true;
}

bool wifiBringUpAp()
{
    if (!sApSsid[0] || strnlen(sApPsk, kMaxPassphrase) < kApPskMin) {
        // begin() guarantees these; if we somehow got here without it, do not
        // fall back to an open AP.
        LOG_ERROR("WiFi: AP credentials not initialised");
        return false;
    }

    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP)) {
        LOG_ERROR("WiFi: WIFI_AP mode refused");
        return false;
    }

    // Fixed, predictable address so "browse to 192.168.4.1" is always true.
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

    const bool ok = WiFi.softAP(sApSsid, sApPsk, 6 /* channel */, 0 /* not hidden */, 4 /* max clients */);
    if (!ok) {
        LOG_ERROR("WiFi: softAP(\"%s\") failed", sApSsid);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    sApRunning = true;
    copyCStr(sSsidBuf, sizeof(sSsidBuf), sApSsid);
    LOG_INFO("WiFi: AP \"%s\" up at %s", sApSsid, WiFi.softAPIP().toString().c_str());
    postWifi(WifiState::ApRunning);
    return true;
}

void wifiTearDown()
{
    // Note: Meshtastic's deinitWifi() early-returns unless isWifiAvailable(),
    // which is false for an AP-only session, so it cannot be reused here. Go
    // straight at the interface and turn it off unconditionally.
    if (sApRunning)
        WiFi.softAPdisconnect(true);
    WiFi.disconnect(true /* wifioff */, false /* eraseap */);
    WiFi.mode(WIFI_OFF);

    sApRunning = false;
    sSsidBuf[0] = 0;
    snprintf(sIpBuf, sizeof(sIpBuf), "0.0.0.0");
    postWifi(WifiState::Idle);
}

} // namespace detail

} // namespace pgros

#endif // PGROS
