#pragma once
//
// Device policy: the user-facing preferences PgrOS owns.
//
// These are deliberately SEPARATE from Meshtastic's config protobufs. Meshtastic's
// `config.device.buzzer_mode` is written by the phone app and governs external
// notifications; reusing it for UI feedback means a user who enables message
// alerts on their phone suddenly gets a clicking keyboard. PgrOS keeps its own
// settings and never writes Meshtastic's.
//
// Defaults are SILENT. A fresh device makes no sound on boot and no sound while
// being used. Every audible or tactile output is opt-in, individually.

#include <stdint.h>

namespace pgros {

enum class AlertMode : uint8_t {
    Off = 0,   // nothing
    Haptic,    // vibrate only
    Sound,     // buzzer only
    Both,      // vibrate and buzz
};

enum class ThemeMode : uint8_t { Dark = 0, Light, Auto };

// Persisted device policy. Trivially copyable so it can be written to flash as
// a single versioned blob.
struct Policy {
    // --- version ---------------------------------------------------------
    uint16_t version = 1;

    // --- sound and haptics: everything off by default --------------------
    bool bootChime = false;      // never make a noise on power-up
    bool keyClick = false;       // no per-keystroke feedback
    bool keyHaptic = false;      // no per-keystroke vibration either
    AlertMode messageAlert = AlertMode::Off;
    AlertMode dmAlert = AlertMode::Off;   // direct messages may warrant more
    bool alertsWhileCharging = true;      // suppress nothing when on USB
    uint8_t volume = 3;                   // 0..10, only used once sound is enabled

    // --- display ---------------------------------------------------------
    uint8_t brightness = 130;    // matches BRIGHTNESS_DEFAULT for the panel
    uint8_t kbBrightness = 0;    // keyboard backlight off by default
    uint16_t screenTimeoutS = 60;
    uint16_t sleepTimeoutS = 300; // 0 disables deep sleep
    ThemeMode theme = ThemeMode::Dark;
    bool showClockOnLock = true;

    // --- messaging -------------------------------------------------------
    bool sendReadReceipts = false;
    bool showNodeShortNames = true; // short vs long name in channel bubbles
    bool relativeTimestamps = true;
    uint16_t historyPerThread = 500;

    // --- radios ----------------------------------------------------------
    // Which radio, if any, comes up automatically after boot. Off by default so
    // boot is as fast and as quiet as possible; the user opts in.
    uint8_t bootRadioMode = 0; // CoexMode::Off
    bool wifiPortalRequiresConfirm = true;

    // --- privacy ---------------------------------------------------------
    bool shareLocationOnMesh = true;
    bool storeGpsTrack = false;
};

class PolicyStore
{
  public:
    // Loads policy from flash, or installs defaults if absent/corrupt. Cheap
    // enough to sit on the boot path -- it is one small file.
    bool begin();

    Policy &get() { return mPolicy; }
    const Policy &get() const { return mPolicy; }

    // Persists the current policy. Debounced by the caller; this writes
    // immediately and synchronously, so do not call it from the UI task on
    // every slider tick.
    bool save();

    // Restores defaults and persists them.
    bool reset();

    // True if the on-flash copy differs from the in-memory copy.
    bool dirty() const { return mDirty; }
    void markDirty() { mDirty = true; }

    // --- convenience predicates used all over the UI ---------------------
    bool shouldHapticOnKey() const { return mPolicy.keyHaptic; }
    bool shouldSoundOnKey() const { return mPolicy.keyClick; }
    bool anyAudibleOutput() const;

  private:
    Policy mPolicy;
    bool mDirty = false;
};

extern PolicyStore policy;

} // namespace pgros
