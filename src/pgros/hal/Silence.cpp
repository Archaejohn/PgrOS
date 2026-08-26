#ifdef PGROS

#include "hal/Silence.h"

#include "configuration.h"

#include "core/Policy.h"

#include "buzz/buzz.h"
#include "buzz/BuzzerFeedbackThread.h"

#if defined(T_LORA_PAGER)
#include "ExtensionIOXL9555.hpp"
// Same declaration the variant uses. The object is defined by the variant's
// support code; we are only borrowing it.
extern ExtensionIOXL9555 io;
#endif

#if defined(HAS_DRV2605)
#include <Adafruit_DRV2605.h>
extern Adafruit_DRV2605 drv;
#endif

namespace pgros
{
namespace Silence
{

static bool sHaptics = false;
static uint32_t sLastFeedbackMs = 0;

// Feedback is rate limited. Holding a key or receiving a burst of messages
// should not turn into a continuous buzz, which is both unpleasant and a real
// current draw on a battery device.
static constexpr uint32_t kMinFeedbackGapMs = 40;
static constexpr uint32_t kMinAlertGapMs = 750;

void muteAmplifierEarly()
{
#if defined(T_LORA_PAGER)
    // No logging here -- see the header. This runs early enough that the console
    // may not exist yet.
    io.pinMode(EXPANDS_AMP_EN, OUTPUT);
    io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
}

static void setAmplifier(bool on)
{
#if defined(T_LORA_PAGER)
    io.digitalWrite(EXPANDS_AMP_EN, on ? HIGH : LOW);
#else
    (void)on;
#endif
}

// Meshtastic builds a BuzzerFeedbackThread in setupModules() whenever the
// display mode is not COLOR, which is our case. It observes InputBroker directly
// and calls playChirp() on every Up/Down and playBeep() on Select, gated only on
// config.device.buzzer_mode -- a setting the phone app writes for *notification*
// preferences, which has nothing to do with whether the UI should click.
//
// The audible result is a chirp per rotary detent, which is exactly the "no
// noises while using it" promise PgrOS makes. Deleting the object detaches its
// observer (Observer's destructor unobserves) and costs nothing else: PgrOS
// drives its own key feedback through keyFeedback() below.
//
// This is deliberately NOT done by writing config.device.buzzer_mode. That
// setting is the user's, it is visible in the phone app, and it also governs
// external message notifications they may well want.
static void suppressUpstreamInputBuzzer()
{
    if (!buzzerFeedbackThread)
        return;
    delete buzzerFeedbackThread;
    buzzerFeedbackThread = nullptr;
    LOG_INFO("PgrOS: detached Meshtastic input buzzer; PgrOS owns key feedback");
}

void applyPolicy()
{
    suppressUpstreamInputBuzzer();

    const Policy &p = policy.get();

#if defined(HAS_DRV2605)
    // The haptic driver is powered through the expander. Leave it unpowered
    // entirely when no haptic setting is enabled: an idle DRV2605 still draws
    // current, and this device is expected to sit in a pocket for days.
    const bool wantHaptics = p.keyHaptic || p.messageAlert == AlertMode::Haptic ||
                             p.messageAlert == AlertMode::Both || p.dmAlert == AlertMode::Haptic ||
                             p.dmAlert == AlertMode::Both;

#if defined(T_LORA_PAGER)
    io.pinMode(EXPANDS_DRV_EN, OUTPUT);
    io.digitalWrite(EXPANDS_DRV_EN, wantHaptics ? HIGH : LOW);
#endif

    if (wantHaptics && !sHaptics) {
        sHaptics = drv.begin();
        if (sHaptics) {
            drv.selectLibrary(1);
            drv.setMode(DRV2605_MODE_INTTRIG);
        } else {
            LOG_WARN("PgrOS: DRV2605 not found; haptics unavailable");
        }
    } else if (!wantHaptics) {
        sHaptics = false;
    }
#endif

    // The amplifier stays off unless something actually wants to make a sound.
    setAmplifier(policy.anyAudibleOutput());
}

bool hapticsAvailable()
{
    return sHaptics;
}

static void pulse(uint8_t effect)
{
#if defined(HAS_DRV2605)
    if (!sHaptics)
        return;
    drv.setWaveform(0, effect);
    drv.setWaveform(1, 0); // end of sequence
    drv.go();
#else
    (void)effect;
#endif
}

void keyFeedback()
{
    const Policy &p = policy.get();
    if (!p.keyClick && !p.keyHaptic)
        return; // the default path: do nothing at all

    const uint32_t now = millis();
    if (now - sLastFeedbackMs < kMinFeedbackGapMs)
        return;
    sLastFeedbackMs = now;

    if (p.keyHaptic)
        pulse(7); // "soft bump", short and unobtrusive

    if (p.keyClick)
        playClick();
}

void notifyMessage(bool direct)
{
    const Policy &p = policy.get();
    const AlertMode mode = direct ? p.dmAlert : p.messageAlert;
    if (mode == AlertMode::Off)
        return; // the default path

    if (!p.alertsWhileCharging) {
        // Placeholder for a charging check; the power state arrives on the event
        // bus and PolicyStore does not track it. Left explicit rather than
        // silently ignoring the setting.
    }

    const uint32_t now = millis();
    if (now - sLastFeedbackMs < kMinAlertGapMs)
        return;
    sLastFeedbackMs = now;

    if (mode == AlertMode::Haptic || mode == AlertMode::Both)
        pulse(direct ? 14 : 12); // stronger buzz for a direct message

    if (mode == AlertMode::Sound || mode == AlertMode::Both) {
        setAmplifier(true);
        if (direct)
            playLongBeep();
        else
            playBeep();
        // The amplifier is left enabled only while sounds are wanted at all;
        // applyPolicy() is the authority on the steady state.
        setAmplifier(policy.anyAudibleOutput());
    }
}

void confirmPulse()
{
    pulse(1); // "strong click"
}

} // namespace Silence
} // namespace pgros

#endif // PGROS
