#pragma once
//
// The "quiet by default" guarantee.
//
// The pager has three ways to make itself noticed: an I2S amplifier, a buzzer,
// and a DRV2605 haptic driver. The requirement is that a fresh device makes no
// sound on boot and no sound in use, and that every audible or tactile output
// is individually opt-in.
//
// This is the single choke point for all three. Nothing else in PgrOS calls
// playBeep(), drv.go(), or touches the amplifier enable -- if it did, "silent"
// would last exactly until someone forgot.
//
// Note that PgrOS deliberately does NOT consult Meshtastic's
// config.device.buzzer_mode for UI feedback. That setting is written by the
// phone app and governs external notifications; a user enabling message alerts
// on their phone should not thereby start making the keyboard click. PgrOS keeps
// its own preferences in core/Policy.h.

namespace pgros {
namespace Silence {

// Drives the audio amplifier enable low. Called from stage 0 boot, before any
// audio code can initialise.
//
// The variant's earlyInitVariant() already sets EXPANDS_AMP_EN low, so on a
// normal boot this is belt-and-braces. It is kept because it is cheap, because
// it documents the requirement at the point it matters, and because it protects
// against a future change to the variant or an early re-enable.
//
// Safe to call before the filesystem, config or mesh exist. Must NOT log: on
// this board the I/O expander is brought up in earlyInitVariant(), which runs
// before consoleInit(), and a LOG_* call that early crashes the device.
void muteAmplifierEarly();

// Pushes the current PolicyStore settings to the hardware: keyboard backlight,
// haptic enable, and the amplifier. Called once policy has loaded, and again
// whenever the user changes a sound or display setting.
void applyPolicy();

// Per-keystroke feedback. Does nothing unless policy.keyClick or policy.keyHaptic
// is set -- both default to false.
void keyFeedback();

// A message arrived. `direct` selects between policy.messageAlert and the
// (usually more insistent) policy.dmAlert.
void notifyMessage(bool direct);

// One-shot haptic pulse, ignoring policy. For confirming a destructive action
// the user explicitly triggered, where silent acknowledgement is worse than a
// bump. Use sparingly.
void confirmPulse();

// True if the haptic driver was found and initialised.
bool hapticsAvailable();

} // namespace Silence
} // namespace pgros
