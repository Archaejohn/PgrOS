#ifdef PGROS

#include "core/Boot.h"

#include "configuration.h"

#include "core/EventBus.h"
#include "core/MeshBridge.h"
#include "core/Panic.h"
#include "core/Policy.h"
#include "core/Service.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
#include "hal/Silence.h"
#include "net/Portal.h"
#include "net/RadioCoex.h"
#include "net/WifiManager.h"
#include "store/ChatStore.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include "ui/apps/ContactsApp.h"
#include "ui/apps/ConversationApp.h"
#include "ui/apps/DiagnosticsApp.h"
#include "ui/apps/GpsApp.h"
#include "ui/apps/HomeApp.h"
#include "ui/apps/MessagesApp.h"
#include "ui/apps/NetworkApp.h"
#include "ui/apps/SettingsApp.h"

namespace pgros
{

// App instances live for the lifetime of the device. They are constructed once
// here, their widget trees are built once in Shell::begin(), and navigation only
// shows and hides them. Rebuilding an LVGL tree on every navigation is the usual
// reason a small-screen UI feels sluggish, and we pay that cost at boot instead,
// while the user is already waiting.

// ---------------------------------------------------------------------------
// Stage 0 -- called from setup() immediately after initSPI().
//
// Nothing here may touch the filesystem, config, or the mesh: none of them
// exist yet. The entire job is to get something on the panel before the slow
// parts of boot begin, so that fsInit(), NodeDB and initLoRa() all happen behind
// a live display rather than behind a black one.
// ---------------------------------------------------------------------------
void earlyBoot()
{
    // FIRST, before anything can initialise the I2S codec. An amplifier that
    // powers up unmuted is what produces the boot pop, and the requirement is
    // that a fresh device is silent. This only drives an expander pin, so it is
    // safe this early.
    Silence::muteAmplifierEarly();

    if (!display.beginPanel()) {
        // A dead panel must not be fatal -- the node should still route mesh
        // traffic and talk to the phone app headlessly.
        LOG_ERROR("PgrOS: panel init failed; continuing headless");
        return;
    }

    // Draw first, light second. Backlight-then-draw shows a white flash on this
    // panel and makes an otherwise quick boot look slow.
    display.drawSplash();
    display.rampBacklight(BRIGHTNESS_DEFAULT);
}

// ---------------------------------------------------------------------------
// Stage 1 -- called from setup() after setupModules() and inputBroker->Init().
//
// Brings up everything else and hands the display to the UI task. Returns as
// soon as that task is running; the rest of Meshtastic's setup() then continues
// on the main task with the UI already interactive.
// ---------------------------------------------------------------------------
void begin()
{
    LOG_INFO("PgrOS: starting");

    // Event bus first -- everything below may want to post to it.
    if (!events.begin())
        LOG_ERROR("PgrOS: event bus failed to start");

    // Crash/boot-loop state, read before anything else can overwrite it.
    panic::begin();

    // Preferences. Defaults are silent; see core/Policy.h.
    policy.begin();
    Silence::applyPolicy();

    postBootStage(1, 20);

    // Persistent chat. begin() is deliberately cheap: it creates directories and
    // returns, and defers per-thread validation to first access, so a deep
    // history does not sit on the boot path.
    if (!chatStore.begin())
        LOG_ERROR("PgrOS: chat store unavailable");
    postSubsysReady(Subsys::Store, chatStore.ready());

    postBootStage(2, 40);

    // The intent queue and its draining OSThread. Must exist before the UI, or
    // the first thing the user does will have nowhere to go.
    service_.begin();

    // Mesh integration: observes text messages and routing acks, resolves sender
    // identity, writes to the chat store.
    if (!mesh.begin())
        LOG_ERROR("PgrOS: mesh bridge failed to start");

    postBootStage(3, 60);

    // Radios. begin() only reads back the configured mode; it does not bring
    // anything up, so it costs nothing here.
    coex.begin();
    wifi.begin();
    portal.begin();

    postBootStage(4, 80);

    // Register apps before Shell::begin(), which builds all their widget trees.
    shell.registerApp(&homeApp);
    shell.registerApp(&messagesApp);
    shell.registerApp(&conversationApp);
    shell.registerApp(&contactsApp);
    shell.registerApp(&settingsApp);
    shell.registerApp(&networkApp);
    shell.registerApp(&gpsApp);
    shell.registerApp(&diagnosticsApp);

    // LVGL, the widget trees, and the UI task on core 1.
    if (!shell.begin()) {
        LOG_ERROR("PgrOS: shell failed to start; continuing headless");
        return;
    }

    // Input last: the UI must exist before keystrokes can reach it.
    keyboard.begin();

    postBootStage(5, 100);
    LOG_INFO("PgrOS: ready");
}

void bootComplete()
{
    // Clears the boot-loop counter. Reaching here means we got all the way to an
    // interactive UI, so this boot counts as good.
    panic::noteBootOk();
    shell.bootComplete();
}

} // namespace pgros

#endif // PGROS
