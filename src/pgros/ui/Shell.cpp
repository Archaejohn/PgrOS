#ifdef PGROS

#include "ui/Shell.h"

#include "configuration.h"

#include "core/Policy.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
#include "hal/Silence.h"
#include "ui/StatusBar.h"
#include "ui/Theme.h"

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

namespace pgros
{

Shell shell;

// Widgets the shell owns directly, outside any app.
static lv_obj_t *sContent = nullptr;   // apps are parented here
static lv_obj_t *sBootOverlay = nullptr;
static lv_obj_t *sBootBar = nullptr;
static lv_obj_t *sBootLabel = nullptr;
static lv_obj_t *sToast = nullptr;
static lv_obj_t *sToastLabel = nullptr;
static uint32_t sToastUntilMs = 0;
static lv_obj_t *sPairModal = nullptr;
static lv_obj_t *sBusyModal = nullptr;

static TaskHandle_t sUiTask = nullptr;

// ---------------------------------------------------------------------------
// Stage 0
// ---------------------------------------------------------------------------

bool Shell::splashEarly()
{
    if (!display.beginPanel())
        return false;
    display.drawSplash();
    display.rampBacklight(policy.get().brightness);
    mBrightness = policy.get().brightness;
    mDisplayOn = true;
    return true;
}

// ---------------------------------------------------------------------------
// Stage 1
// ---------------------------------------------------------------------------

void Shell::registerApp(App *app)
{
    if (!app)
        return;
    const size_t idx = (size_t)app->id();
    if (idx >= (size_t)AppId::Count) {
        LOG_ERROR("PgrOS: app id %u out of range", (unsigned)idx);
        return;
    }
    mApps[idx] = app;
    app->mShell = this;
}

App *Shell::appFor(AppId id) const
{
    const size_t idx = (size_t)id;
    if (idx >= (size_t)AppId::Count)
        return nullptr;
    return mApps[idx];
}

bool Shell::begin()
{
    if (!display.beginLvgl())
        return false;

    theme.begin();

    lv_obj_t *scr = lv_screen_active();
    theme.styleScreen(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status bar first so it sits above the content in z-order.
    statusBar.build(scr);

    // Content region: everything below the status bar. Apps are children of
    // this, so hiding the bar for a fullscreen app is a single resize.
    sContent = lv_obj_create(scr);
    lv_obj_remove_style_all(sContent);
    lv_obj_set_pos(sContent, 0, metrics::statusBarH);
    lv_obj_set_size(sContent, metrics::screenW, metrics::contentH);
    lv_obj_remove_flag(sContent, LV_OBJ_FLAG_SCROLLABLE);

    // Build every app's widget tree once, now, while the user is already
    // waiting on boot. Navigation then only toggles visibility.
    for (size_t i = 0; i < (size_t)AppId::Count; ++i) {
        if (!mApps[i])
            continue;
        mApps[i]->onCreate(sContent);
        if (mApps[i]->root())
            lv_obj_add_flag(mApps[i]->root(), LV_OBJ_FLAG_HIDDEN);
    }

    buildStatusBar();

    // Boot overlay sits on top of everything until the mesh stack is up.
    {
        sBootOverlay = lv_obj_create(scr);
        lv_obj_remove_style_all(sBootOverlay);
        lv_obj_set_size(sBootOverlay, metrics::screenW, metrics::screenH);
        lv_obj_set_pos(sBootOverlay, 0, 0);
        lv_obj_set_style_bg_color(sBootOverlay, lv_color_hex(theme.colors().bg), 0);
        lv_obj_set_style_bg_opa(sBootOverlay, LV_OPA_COVER, 0);
        lv_obj_remove_flag(sBootOverlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *mark = lv_label_create(sBootOverlay);
        lv_label_set_text(mark, "PgrOS");
        lv_obj_set_style_text_font(mark, theme.fontLarge(), 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(theme.colors().text), 0);
        lv_obj_align(mark, LV_ALIGN_CENTER, 0, -18);

        // A thin determinate line, not a spinner. A spinner says "something is
        // wrong and I am waiting"; a filling line says "this is progressing".
        lv_obj_t *track = lv_obj_create(sBootOverlay);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, 140, 2);
        lv_obj_align(track, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_bg_color(track, lv_color_hex(theme.colors().border), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);

        sBootBar = lv_obj_create(track);
        lv_obj_remove_style_all(sBootBar);
        lv_obj_set_size(sBootBar, 0, 2);
        lv_obj_set_pos(sBootBar, 0, 0);
        lv_obj_set_style_bg_color(sBootBar, lv_color_hex(theme.colors().accent), 0);
        lv_obj_set_style_bg_opa(sBootBar, LV_OPA_COVER, 0);

        sBootLabel = lv_label_create(sBootOverlay);
        lv_label_set_text(sBootLabel, "starting");
        lv_obj_set_style_text_font(sBootLabel, theme.fontSmall(), 0);
        lv_obj_set_style_text_color(sBootLabel, lv_color_hex(theme.colors().textDim), 0);
        lv_obj_align(sBootLabel, LV_ALIGN_CENTER, 0, 28);
    }

    // Toast, hidden until used.
    {
        sToast = lv_obj_create(scr);
        lv_obj_remove_style_all(sToast);
        theme.styleCard(sToast);
        lv_obj_set_size(sToast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(sToast, metrics::padM, 0);
        lv_obj_align(sToast, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_add_flag(sToast, LV_OBJ_FLAG_HIDDEN);

        sToastLabel = lv_label_create(sToast);
        lv_obj_set_style_text_font(sToastLabel, theme.fontBody(), 0);
        lv_label_set_text(sToastLabel, "");
    }

    home();

    BaseType_t ok = xTaskCreatePinnedToCore(&Shell::taskEntry, "pgros-ui", kTaskStackBytes / sizeof(StackType_t), this,
                                            kTaskPriority, &sUiTask, kTaskCore);
    if (ok != pdPASS) {
        LOG_ERROR("PgrOS: UI task failed to start");
        return false;
    }
    mTask = sUiTask;
    mLastActivityMs = millis();
    LOG_INFO("PgrOS: UI task running on core %d", kTaskCore);
    return true;
}

void Shell::buildStatusBar()
{
    statusBar.setTitle("Home");
}

void Shell::updateStatusBar()
{
    statusBar.tick();
}

// ---------------------------------------------------------------------------
// The UI task
// ---------------------------------------------------------------------------

void Shell::taskEntry(void *arg)
{
    static_cast<Shell *>(arg)->run();
}

void Shell::run()
{
    uint32_t lastTickMs = 0;

    for (;;) {
        const uint32_t frameStart = millis();

        // 1. Input. Keys arrive from the mesh task through a queue; this is the
        //    only place they are consumed.
        uint32_t k;
        while (keyboard.poll(k)) {
            noteActivity();

            // Any key wakes a blanked screen and is then swallowed, so the
            // keystroke that wakes the device does not also act on the UI.
            if (!mDisplayOn) {
                setDisplayOn(true);
                continue;
            }

            Silence::keyFeedback();

            // Modals capture input before anything else.
            if (sPairModal) {
                if (k == key::Enter || k == key::Cancel || k == key::Back)
                    dismissPairingCode();
                continue;
            }
            if (sBusyModal) {
                continue; // deliberately swallow; the operation owns the device
            }

            App *app = appFor(current());
            bool consumed = app && app->onKey(k);
            if (!consumed) {
                // Default handling.
                if (k == key::Back || k == key::Cancel) {
                    if (app && app->backExitsToHome())
                        home();
                    else
                        pop();
                }
            }
        }

        // 2. System events.
        Event ev;
        while (events.poll(ev))
            dispatch(ev);

        // 3. Periodic app tick and chrome refresh, ~5 Hz.
        if (frameStart - lastTickMs >= 200) {
            lastTickMs = frameStart;
            App *app = appFor(current());
            if (app)
                app->onTick();
            updateStatusBar();

            // Inactivity blanking. PowerFSM would normally do this via
            // screen->setOn(false), but that call is a no-op stub in our build,
            // so the policy lives here.
            const uint16_t timeout = policy.get().screenTimeoutS;
            if (mDisplayOn && timeout && (frameStart - mLastActivityMs) > (uint32_t)timeout * 1000)
                setDisplayOn(false);
        }

        // 4. Toast expiry.
        if (sToastUntilMs && frameStart >= sToastUntilMs) {
            sToastUntilMs = 0;
            lv_obj_add_flag(sToast, LV_OBJ_FLAG_HIDDEN);
        }

        // 5. Render.
        uint32_t next = 5;
        if (mDisplayOn) {
            next = lv_timer_handler();
            if (next > 30)
                next = 30; // stay responsive to input even when idle
        } else {
            next = 50; // nothing to draw; back right off
        }

        mFrames++;
        mLastFrameMs = (uint16_t)(millis() - frameStart);

        // Always yield at least one tick. This task shares core 1 with
        // Arduino's loopTask; never spin.
        vTaskDelay(pdMS_TO_TICKS(next ? next : 1));
    }
}

void Shell::dispatch(const Event &ev)
{
    // Chrome first.
    statusBar.onEvent(ev);

    switch (ev.type) {
    case EventType::BootStage:
        if (sBootBar) {
            lv_obj_set_width(sBootBar, (140 * ev.boot.percent) / 100);
            static const char *stageText[] = {"starting", "preferences", "history", "mesh", "radios", "ready"};
            const uint8_t s = ev.boot.stage < 6 ? ev.boot.stage : 5;
            if (sBootLabel)
                lv_label_set_text(sBootLabel, stageText[s]);
        }
        break;

    case EventType::BlePairing:
        showPairingCode(ev.ble.passkey);
        break;

    case EventType::BlePairingEnd:
        dismissPairingCode();
        break;

    case EventType::Notification:
        toast(ev.note.text, ev.note.severity);
        break;

    default:
        break;
    }

    // Then the visible app.
    App *app = appFor(current());
    if (app)
        app->onEvent(ev);
}

void Shell::setBootProgress(uint8_t stage, uint8_t percent)
{
    postBootStage(stage, percent);
}

void Shell::bootComplete()
{
    if (!sBootOverlay)
        return;
    // Fade rather than cut: the content underneath is already drawn, so this
    // reads as the OS arriving rather than as a screen change.
    lv_obj_fade_out(sBootOverlay, 220, 0);
    sBootOverlay = nullptr;
    sBootBar = nullptr;
    sBootLabel = nullptr;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

AppId Shell::current() const
{
    return mDepth ? mStack[mDepth - 1].id : AppId::None;
}

void Shell::push(AppId id, const AppArgs &args)
{
    App *next = appFor(id);
    if (!next) {
        LOG_WARN("PgrOS: push of unregistered app %u", (unsigned)id);
        return;
    }
    if (mDepth >= kMaxDepth) {
        LOG_WARN("PgrOS: nav stack full");
        return;
    }

    App *prev = appFor(current());
    if (prev) {
        prev->onHide();
        if (prev->root())
            lv_obj_add_flag(prev->root(), LV_OBJ_FLAG_HIDDEN);
    }

    mStack[mDepth].id = id;
    mStack[mDepth].args = args;
    mDepth++;

    if (next->root())
        lv_obj_remove_flag(next->root(), LV_OBJ_FLAG_HIDDEN);
    next->onShow(args);

    statusBar.setTitle(next->title());
    statusBar.setVisible(!next->fullscreen());
    lv_obj_set_pos(sContent, 0, next->fullscreen() ? 0 : metrics::statusBarH);
    lv_obj_set_size(sContent, metrics::screenW, next->fullscreen() ? metrics::screenH : metrics::contentH);
    noteActivity();
}

void Shell::pop()
{
    if (mDepth <= 1)
        return; // never pop the root

    App *top = appFor(current());
    if (top) {
        top->onHide();
        if (top->root())
            lv_obj_add_flag(top->root(), LV_OBJ_FLAG_HIDDEN);
    }
    mDepth--;

    App *now = appFor(current());
    if (now) {
        if (now->root())
            lv_obj_remove_flag(now->root(), LV_OBJ_FLAG_HIDDEN);
        now->onShow(mStack[mDepth - 1].args);
        statusBar.setTitle(now->title());
        statusBar.setVisible(!now->fullscreen());
        lv_obj_set_pos(sContent, 0, now->fullscreen() ? 0 : metrics::statusBarH);
        lv_obj_set_size(sContent, metrics::screenW, now->fullscreen() ? metrics::screenH : metrics::contentH);
    }
    noteActivity();
}

void Shell::home()
{
    while (mDepth > 1)
        pop();

    if (mDepth == 0) {
        push(AppId::Home);
        return;
    }
    if (current() != AppId::Home) {
        mDepth = 0;
        push(AppId::Home);
    }
}

void Shell::replace(AppId id, const AppArgs &args)
{
    if (mDepth)
        mDepth--;
    push(id, args);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

void Shell::toast(const char *text, uint8_t severity)
{
    if (!text || !text[0])
        return;

    // Safe from any task: if we are not the UI task, go through the bus.
    if (xTaskGetCurrentTaskHandle() != sUiTask) {
        postNotification(text, severity);
        return;
    }
    if (!sToast)
        return;

    lv_label_set_text(sToastLabel, text);
    Color c = theme.colors().text;
    if (severity == 1)
        c = theme.colors().warn;
    else if (severity >= 2)
        c = theme.colors().error;
    lv_obj_set_style_text_color(sToastLabel, lv_color_hex(c), 0);

    lv_obj_remove_flag(sToast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sToast);
    sToastUntilMs = millis() + (severity >= 2 ? 4000 : 2200);
}

void Shell::showPairingCode(uint32_t passkey)
{
    if (sPairModal)
        dismissPairingCode();

    // Wake the screen unconditionally. A pairing code the user cannot see is
    // the same as no pairing code.
    if (!mDisplayOn)
        setDisplayOn(true);
    noteActivity();

    lv_obj_t *scr = lv_screen_active();

    sPairModal = lv_obj_create(scr);
    lv_obj_remove_style_all(sPairModal);
    lv_obj_set_size(sPairModal, metrics::screenW, metrics::screenH);
    lv_obj_set_pos(sPairModal, 0, 0);
    lv_obj_set_style_bg_color(sPairModal, lv_color_hex(theme.colors().bg), 0);
    lv_obj_set_style_bg_opa(sPairModal, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sPairModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(sPairModal);

    lv_obj_t *cap = lv_label_create(sPairModal);
    lv_label_set_text(cap, "Bluetooth pairing");
    lv_obj_set_style_text_font(cap, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.colors().textDim), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -52);

    // Grouped 3+3, which is how every phone shows a passkey and how people
    // read six digits aloud.
    char pin[16];
    snprintf(pin, sizeof(pin), "%03u %03u", (unsigned)(passkey / 1000) % 1000, (unsigned)(passkey % 1000));

    lv_obj_t *code = lv_label_create(sPairModal);
    lv_label_set_text(code, pin);
    lv_obj_set_style_text_font(code, theme.fontLarge(), 0);
    lv_obj_set_style_text_color(code, lv_color_hex(theme.colors().accent), 0);
    lv_obj_align(code, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *hint = lv_label_create(sPairModal);
    lv_label_set_text(hint, "Enter this code on your phone");
    lv_obj_set_style_text_font(hint, theme.fontBody(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *dismiss = lv_label_create(sPairModal);
    lv_label_set_text(dismiss, "Enter to dismiss");
    lv_obj_set_style_text_font(dismiss, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(dismiss, lv_color_hex(theme.colors().textFaint), 0);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void Shell::dismissPairingCode()
{
    if (!sPairModal)
        return;
    lv_obj_delete(sPairModal);
    sPairModal = nullptr;
}

void Shell::showBusy(const char *what)
{
    if (sBusyModal)
        hideBusy();

    lv_obj_t *scr = lv_screen_active();
    sBusyModal = lv_obj_create(scr);
    lv_obj_remove_style_all(sBusyModal);
    lv_obj_set_size(sBusyModal, metrics::screenW, metrics::screenH);
    lv_obj_set_pos(sBusyModal, 0, 0);
    lv_obj_set_style_bg_color(sBusyModal, lv_color_hex(theme.colors().bg), 0);
    lv_obj_set_style_bg_opa(sBusyModal, LV_OPA_80, 0);
    lv_obj_remove_flag(sBusyModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(sBusyModal);

    lv_obj_t *sp = lv_spinner_create(sBusyModal);
    lv_obj_set_size(sp, 32, 32);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_arc_color(sp, lv_color_hex(theme.colors().accent), LV_PART_INDICATOR);

    lv_obj_t *lbl = lv_label_create(sBusyModal);
    lv_label_set_text(lbl, what ? what : "Working");
    lv_obj_set_style_text_font(lbl, theme.fontBody(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(theme.colors().text), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 22);
}

void Shell::hideBusy()
{
    if (!sBusyModal)
        return;
    lv_obj_delete(sBusyModal);
    sBusyModal = nullptr;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Shell::setBrightness(uint8_t level)
{
    mBrightness = level;
    display.setBrightness(level);
}

void Shell::setDisplayOn(bool on)
{
    if (on == mDisplayOn)
        return;
    mDisplayOn = on;
    display.setOn(on);
    if (on)
        noteActivity();
}

void Shell::noteActivity()
{
    mLastActivityMs = millis();
}

} // namespace pgros

#endif // PGROS
