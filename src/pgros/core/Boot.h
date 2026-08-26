#pragma once
//
// PgrOS entry points, called from Meshtastic's setup() via the integration
// patch in patches/integration/0001-pgros-hooks/.
//
// Only these two functions are visible to the vendored tree. Everything else in
// PgrOS is reached from here, which keeps the patch to main.cpp down to two
// call sites and one include -- small enough to re-apply by hand if the pin
// moves.

namespace pgros {

// Stage 0, called immediately after initSPI().
//
// Brings up the ST7796 and paints the splash WITH THE BACKLIGHT STILL OFF, then
// ramps the backlight onto an already-drawn frame. Ordering matters: powering
// the backlight before there is anything in the framebuffer shows a white flash
// and makes an otherwise quick boot look slow.
//
// Runs before fsInit(), NodeDB and the radio, so it must not touch the
// filesystem, config, or the mesh. It also mutes the audio amplifier via the
// XL9555 expander before any audio code can initialise, which is what prevents
// the power-on pop.
//
// Must be fast. Target is under 120 ms.
void earlyBoot();

// Stage 1, called after setupModules() and inputBroker->Init().
//
// Initialises LVGL against PSRAM buffers, builds the app tree, subscribes to
// InputBroker and the mesh, and spawns the UI task on core 1. Returns as soon
// as the UI task is running -- the rest of Meshtastic's setup() then continues
// on the main task while the display is already interactive.
void begin();

// Called from the UI task once the mesh stack has finished coming up, to
// dismiss the boot overlay. Wired internally; declared here for clarity.
void bootComplete();

} // namespace pgros
