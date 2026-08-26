#pragma once
//
// The PgrOS shell: status bar, navigation stack, and the UI task itself.
//
// This is the owner of every LVGL call in the firmware. It runs as a dedicated
// FreeRTOS task pinned to core 1 so that the LoRa stack on core 0 can never
// stall a frame, and so that a slow mesh operation never makes the UI feel
// stuck. See docs/ARCHITECTURE.md#threading-model.
//
// Boot ordering matters here. splashEarly() is called before almost anything
// else in setup(): it brings up the panel and paints the first frame while the
// backlight is still off, then ramps the backlight onto an already-drawn image.
// Powering the backlight first and drawing second is what makes an otherwise
// fast boot look slow, and on this panel it also shows a white flash.

#include "App.h"
#include "core/EventBus.h"
#include <stdint.h>

namespace pgros {

class Shell
{
  public:
    // --- boot path -------------------------------------------------------

    // Stage 0. Panel + backlight-off splash, drawn directly with LovyanGFX.
    // No LVGL, no heap churn, no filesystem. Target: under 120 ms.
    // Safe to call before the filesystem or the mesh stack exist.
    bool splashEarly();

    // Stage 1. Initialise LVGL against PSRAM buffers, register the input
    // driver, create every app, and spawn the UI task on core 1. After this
    // returns the display is interactive even though the mesh is still booting.
    bool begin();

    // Reports boot progress from the main task. Thread-safe (posts an event).
    void setBootProgress(uint8_t stage, uint8_t percent);

    // Tells the shell the system is fully up; dismisses the boot overlay.
    void bootComplete();

    // --- navigation (UI task only) ---------------------------------------

    // Push an app onto the stack.
    void push(AppId id, const AppArgs &args = {});

    // Pop the current app. No-op at the root.
    void pop();

    // Clear the stack back to Home.
    void home();

    // Replace the top of the stack without growing it.
    void replace(AppId id, const AppArgs &args = {});

    AppId current() const;
    uint8_t stackDepth() const { return mDepth; }

    // --- chrome ----------------------------------------------------------

    // Transient toast. Safe from any task -- posts an event if called off-task.
    void toast(const char *text, uint8_t severity = 0);

    // Modal dialog showing the BLE pairing passkey. Dismissed by
    // EventType::BlePairingEnd or by the user.
    void showPairingCode(uint32_t passkey);
    void dismissPairingCode();

    // A blocking-looking modal with a spinner, for operations the user must
    // wait on (a radio mode switch). The UI task keeps rendering underneath.
    void showBusy(const char *what);
    void hideBusy();

    // --- display ---------------------------------------------------------

    void setBrightness(uint8_t level); // 0..255, ramped not stepped
    void setDisplayOn(bool on);
    bool displayOn() const { return mDisplayOn; }

    // Resets the inactivity timer. Called by the input path.
    void noteActivity();

    // --- diagnostics -----------------------------------------------------

    uint32_t frameCount() const { return mFrames; }
    uint16_t lastFrameMs() const { return mLastFrameMs; }
    uint32_t droppedEvents() const { return events.dropped(); }

    // Registers an app instance. Called during begin() for each app.
    void registerApp(App *app);

  private:
    // The task body. Drains the event bus, ticks LVGL, renders.
    static void taskEntry(void *arg);
    void run();

    void dispatch(const Event &ev);
    void buildStatusBar();
    void updateStatusBar();

    App *appFor(AppId id) const;

    App *mApps[(size_t)AppId::Count] = {nullptr};

    static constexpr uint8_t kMaxDepth = 8;
    struct StackEntry {
        AppId id;
        AppArgs args;
    };
    StackEntry mStack[kMaxDepth];
    uint8_t mDepth = 0;

    bool mDisplayOn = true;
    uint8_t mBrightness = 0;
    uint32_t mLastActivityMs = 0;
    uint32_t mFrames = 0;
    uint16_t mLastFrameMs = 0;
    void *mTask = nullptr; // TaskHandle_t

    // UI task configuration. LVGL needs a generous stack; rendering recurses
    // through the widget tree.
    static constexpr uint32_t kTaskStackBytes = 12 * 1024;
    static constexpr uint32_t kTaskPriority = 2; // below the radio, above idle
    static constexpr int kTaskCore = 1;          // core 0 runs the mesh stack
};

extern Shell shell;

} // namespace pgros
