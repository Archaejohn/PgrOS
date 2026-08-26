#pragma once
//
// The PgrOS web portal.
//
// When the pager is running as an access point, phones on that AP get a browser
// UI served from LittleFS: a local chatroom and a photo gallery they can upload
// to. This is deliberately independent of the LoRa mesh -- it is a local-network
// service for people in the same room, and it works at photo sizes LoRa never
// could.
//
// The portal only runs while RadioCoex is in WifiAp (or WifiStation) mode. It
// is started and stopped by the coexistence transitions, never on its own.
//
// Security posture: the AP is WPA2 with a generated passphrase, and the portal
// trusts anyone who is on the network. It does not expose device configuration,
// mesh keys, or anything that could reconfigure the node -- only the chatroom
// and the gallery. Keep it that way; a captive local web UI is not the place to
// put node administration.

#include <stddef.h>
#include <stdint.h>

namespace pgros {

static constexpr uint8_t kPortalNickLen = 24;
static constexpr uint16_t kPortalMsgLen = 280;
static constexpr uint8_t kPortalHistory = 100; // messages kept in the room

struct PortalMessage {
    uint32_t id;
    uint32_t atMs;
    char nick[kPortalNickLen];
    char text[kPortalMsgLen];
    bool fromMesh; // relayed in from a LoRa channel rather than a browser
};

struct GalleryItem {
    char name[64];     // filename on flash
    uint32_t bytes;
    uint32_t uploadedAt;
    char uploader[kPortalNickLen];
    uint16_t width;
    uint16_t height;
};

struct PortalStats {
    uint8_t clients = 0;
    uint32_t requests = 0;
    uint16_t roomMessages = 0;
    uint16_t galleryItems = 0;
    uint32_t galleryBytes = 0;
};

class Portal
{
  public:
    // Registers routes. Does not open a socket; start() does that.
    bool begin();

    // Start/stop serving. Called by RadioCoex on entering/leaving a WiFi mode.
    bool start(uint16_t port = 80);
    void stop();
    bool running() const { return mRunning; }

    // Must be pumped from the service task if the underlying HTTP stack is
    // synchronous. Harmless when the stack is event-driven.
    void loop();

    PortalStats stats() const { return mStats; }

    // --- chatroom ---------------------------------------------------------

    // Post a message into the room from device-side code (e.g. mirroring a LoRa
    // message in). Browser-originated messages arrive through the HTTP handler.
    bool postToRoom(const char *nick, const char *text, bool fromMesh = false);

    size_t roomHistory(PortalMessage *out, size_t max) const;
    void clearRoom();

    // Whether messages posted in the browser room are also transmitted on the
    // LoRa mesh. Off by default: the room is a local-network feature, and
    // silently pushing browser chatter onto a shared mesh channel would be
    // antisocial. The user opts in per session.
    void setMeshRelay(bool on) { mMeshRelay = on; }
    bool meshRelay() const { return mMeshRelay; }

    // --- gallery ----------------------------------------------------------

    size_t galleryList(GalleryItem *out, size_t max) const;
    bool galleryDelete(const char *name);
    void galleryClear();

    // Free space available for uploads, in bytes.
    uint32_t galleryFreeBytes() const;

    // Upload limits. A 4 MB photo would fill the partition, so uploads are
    // capped and the browser is told to downscale before sending.
    static constexpr uint32_t kMaxUploadBytes = 512 * 1024;
    static constexpr uint32_t kGalleryReserveBytes = 256 * 1024; // keep free for logs

  private:
    bool mRunning = false;
    bool mMeshRelay = false;
    PortalStats mStats;
};

extern Portal portal;

} // namespace pgros
