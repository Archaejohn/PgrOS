#ifdef PGROS

#include "net/Portal.h"

#include "configuration.h"

#include "FSCommon.h"
#include "SPILock.h"
#include "concurrency/LockGuard.h"
#include "core/EventBus.h"
#include "core/Service.h"
#include "mesh/NodeDB.h"

// The esp32_https_server headers collide with a `str` macro pulled in via the
// Meshtastic headers above. Upstream hit this too; see the comment in
// src/mesh/http/WebServer.cpp.
#undef str

#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPServer.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;

#if defined(HAS_SDCARD)
#include <SD.h>
#endif

#include <esp_task_wdt.h>
#include <string.h>

namespace pgros
{

Portal portal;

static HTTPServer *sServer = nullptr;

// ---------------------------------------------------------------------------
// Room storage
//
// The chatroom is a fixed ring in RAM, not a file. It is a local-network
// conversation among people who are physically present; it is not the LoRa mesh
// history, and persisting it would spend the 3.375 MiB flash partition on
// something nobody expects to survive a reboot.
// ---------------------------------------------------------------------------
static PortalMessage sRoom[kPortalHistory];
static uint16_t sRoomCount = 0;
static uint16_t sRoomHead = 0; // next write slot
static uint32_t sNextMsgId = 1;

// ---------------------------------------------------------------------------
// Gallery storage
//
// Prefer the SD card when one is mounted: photos are the one thing on this
// device that can genuinely fill a 3.375 MiB partition, and the SD slot is
// otherwise completely unused by the firmware.
//
// Every SD access must hold spiLock -- the card shares SPI2 with the LoRa radio
// and the display.
// ---------------------------------------------------------------------------
static bool sUseSd = false;

static const char *kGalleryDir = "/pgros/gallery";
static const char *kWwwDir = "/pgros/www";

static fs::FS &galleryFs()
{
#if defined(HAS_SDCARD)
    if (sUseSd)
        return SD;
#endif
    return FSCom;
}

static void detectGalleryStore()
{
    sUseSd = false;
#if defined(HAS_SDCARD)
    concurrency::LockGuard g(spiLock);
    if (SD.cardType() != CARD_NONE) {
        if (!SD.exists(kGalleryDir))
            SD.mkdir(kGalleryDir);
        sUseSd = true;
        LOG_INFO("PgrOS portal: gallery on SD card");
        return;
    }
#endif
    concurrency::LockGuard g2(spiLock);
    if (!FSCom.exists(kGalleryDir))
        FSCom.mkdir(kGalleryDir);
    LOG_INFO("PgrOS portal: gallery on internal flash");
}

// ---------------------------------------------------------------------------
// Input hygiene
//
// The stock Meshtastic upload handler has a comment admitting it does not
// validate filenames. We are exposing this on an access point that strangers
// may join, so we do not copy that: the client never gets to choose a path.
// ---------------------------------------------------------------------------

// Keeps only a safe extension from a client-supplied name; everything else is
// discarded and we generate the stored name ourselves.
static void safeExtension(const char *name, char *out, size_t outLen)
{
    out[0] = '\0';
    if (!name)
        return;
    const char *dot = strrchr(name, '.');
    if (!dot || strlen(dot) > 5)
        return;
    size_t j = 0;
    for (const char *p = dot; *p && j < outLen - 1; ++p) {
        const char c = *p;
        const bool ok = (c == '.') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok)
            return; // anything unexpected: no extension at all
        out[j++] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }
    out[j] = '\0';

    static const char *allowed[] = {".jpg", ".jpeg", ".png", ".gif", ".webp"};
    for (const char *a : allowed)
        if (strcmp(out, a) == 0)
            return;
    out[0] = '\0';
}

// Validates a stored asset id coming back from the client. Only names we
// generated can pass: 8 hex digits, an optional 't' marking the thumbnail
// variant, then an approved extension.
static bool validAssetName(const char *name)
{
    if (!name)
        return false;
    size_t i = 0;
    for (; i < 8; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    if (name[i] == 't')
        i++;
    char ext[8];
    safeExtension(name + i, ext, sizeof(ext));
    return ext[0] != '\0' && strcmp(name + i, ext) == 0;
}

// True for the thumbnail half of a pair. Thumbnails are stored alongside their
// full image but hidden from the gallery listing, so the grid shows each photo
// once.
static bool isThumbName(const char *name)
{
    return name && strlen(name) > 8 && name[8] == 't';
}

// "/pgros/gallery/a1b2c3d4t.jpg" for a full image named "a1b2c3d4.jpg".
static void thumbPathFor(const char *fullName, char *out, size_t outLen)
{
    out[0] = '\0';
    if (!fullName || strlen(fullName) < 9)
        return;
    char stem[16];
    memcpy(stem, fullName, 8);
    stem[8] = '\0';
    snprintf(out, outLen, "%s/%st%s", kGalleryDir, stem, fullName + 8);
}

// Copies UTF-8 text, dropping control characters, and NUL-terminates.
static void sanitiseText(const char *in, char *out, size_t outLen)
{
    size_t j = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const char *p = in; *p && j < outLen - 1; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F)
            continue;
        out[j++] = (char)c;
    }
    out[j] = '\0';
}

static void jsonEscape(const char *in, std::string &out)
{
    for (const char *p = in; *p; ++p) {
        switch (*p) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += *p;
        }
    }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void sendJson(HTTPResponse *res, const std::string &body, int code = 200)
{
    res->setStatusCode(code);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Cache-Control", "no-store");
    res->print(body.c_str());
}

// GET /api/room?since=<id>
static void handleRoomGet(HTTPRequest *req, HTTPResponse *res)
{
    uint32_t since = 0;
    std::string q = req->getParams()->isQueryParameterSet("since") ? "" : "";
    {
        std::string val;
        if (req->getParams()->getQueryParameter("since", val))
            since = (uint32_t)strtoul(val.c_str(), nullptr, 10);
    }

    std::string out = "{\"messages\":[";
    bool first = true;
    const uint16_t n = sRoomCount;
    for (uint16_t i = 0; i < n; ++i) {
        // Walk oldest -> newest through the ring.
        const uint16_t idx = (uint16_t)((sRoomHead + kPortalHistory - n + i) % kPortalHistory);
        const PortalMessage &m = sRoom[idx];
        if (m.id <= since)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"id\":" + std::to_string(m.id);
        out += ",\"at\":" + std::to_string(m.atMs);
        out += ",\"mesh\":" + std::string(m.fromMesh ? "true" : "false");
        out += ",\"nick\":\"";
        jsonEscape(m.nick, out);
        out += "\",\"text\":\"";
        jsonEscape(m.text, out);
        out += "\"}";
    }
    out += "],\"last\":" + std::to_string(sNextMsgId - 1);
    out += ",\"relay\":" + std::string(portal.meshRelay() ? "true" : "false") + "}";
    sendJson(res, out);
}

// POST /api/room   (application/x-www-form-urlencoded: nick, text)
static void handleRoomPost(HTTPRequest *req, HTTPResponse *res)
{
    char nick[kPortalNickLen] = {0};
    char text[kPortalMsgLen] = {0};

    HTTPURLEncodedBodyParser parser(req);
    while (parser.nextField()) {
        const std::string name = parser.getFieldName();
        std::string value;
        char buf[256];
        while (!parser.endOfField()) {
            const size_t got = parser.read((byte *)buf, sizeof(buf) - 1);
            if (!got)
                break;
            buf[got] = '\0';
            value += buf;
            if (value.size() > kPortalMsgLen * 2)
                break; // refuse to accumulate unbounded input
        }
        if (name == "nick")
            sanitiseText(value.c_str(), nick, sizeof(nick));
        else if (name == "text")
            sanitiseText(value.c_str(), text, sizeof(text));
    }

    if (!text[0]) {
        sendJson(res, "{\"error\":\"empty\"}", 400);
        return;
    }
    if (!nick[0])
        strncpy(nick, "guest", sizeof(nick) - 1);

    portal.postToRoom(nick, text, false);
    sendJson(res, "{\"ok\":true}");
}

// GET /api/gallery
static void handleGalleryList(HTTPRequest *req, HTTPResponse *res)
{
    GalleryItem items[64];
    const size_t n = portal.galleryList(items, 64);

    std::string out = "{\"items\":[";
    for (size_t i = 0; i < n; ++i) {
        if (i)
            out += ",";
        out += "{\"name\":\"";
        jsonEscape(items[i].name, out);
        out += "\",\"bytes\":" + std::to_string(items[i].bytes);
        out += ",\"thumb\":" + std::string(items[i].hasThumb ? "true" : "false") + "}";
    }
    out += "],\"free\":" + std::to_string(portal.galleryFreeBytes());
    out += ",\"max\":" + std::to_string((uint32_t)Portal::kMaxUploadBytes) + "}";
    sendJson(res, out);
}

// DELETE /api/gallery?name=<asset>
static void handleGalleryDelete(HTTPRequest *req, HTTPResponse *res)
{
    std::string name;
    if (!req->getParams()->getQueryParameter("name", name)) {
        sendJson(res, "{\"error\":\"name\"}", 400);
        return;
    }
    if (!portal.galleryDelete(name.c_str())) {
        sendJson(res, "{\"error\":\"notfound\"}", 404);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// GET /photo/<name>
static void handlePhoto(HTTPRequest *req, HTTPResponse *res)
{
    std::string param;
    req->getParams()->getPathParameter(0, param);
    if (!validAssetName(param.c_str())) {
        res->setStatusCode(400);
        res->print("bad name");
        return;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kGalleryDir, param.c_str());

    concurrency::LockGuard g(spiLock);
    File f = galleryFs().open(path, FILE_O_READ);
    if (!f) {
        res->setStatusCode(404);
        res->print("not found");
        return;
    }

    const char *type = "image/jpeg";
    if (strstr(param.c_str(), ".png"))
        type = "image/png";
    else if (strstr(param.c_str(), ".gif"))
        type = "image/gif";
    else if (strstr(param.c_str(), ".webp"))
        type = "image/webp";

    res->setHeader("Content-Type", type);
    res->setHeader("Cache-Control", "public, max-age=86400");

    uint8_t buf[512];
    while (f.available()) {
        const size_t got = f.read(buf, sizeof(buf));
        if (!got)
            break;
        res->write(buf, got);
    }
    f.close();
}

// POST /api/upload  (multipart/form-data)
static void handleUpload(HTTPRequest *req, HTTPResponse *res)
{
    if (portal.galleryFreeBytes() < Portal::kMaxUploadBytes) {
        sendJson(res, "{\"error\":\"full\"}", 507);
        return;
    }

    HTTPMultipartBodyParser parser(req);
    bool wrote = false;
    char stored[32] = {0};

    // One id shared by both halves of the upload, chosen HERE. The client's
    // filename is used only for its extension, which is validated against an
    // allow-list; the client never gets to influence the path.
    const unsigned id = (unsigned)(millis() ^ (esp_random() & 0xFFFF));

    while (parser.nextField()) {
        const std::string field = parser.getFieldName();
        const bool isThumb = (field == "thumb");
        if (field != "photo" && !isThumb)
            continue;

        char ext[8];
        safeExtension(parser.getFieldFilename().c_str(), ext, sizeof(ext));
        if (!ext[0]) {
            if (isThumb)
                continue; // a bad thumbnail is not worth failing the upload over
            sendJson(res, "{\"error\":\"type\"}", 415);
            return;
        }

        char name[32];
        snprintf(name, sizeof(name), "%08x%s%s", id, isThumb ? "t" : "", ext);

        char path[96];
        snprintf(path, sizeof(path), "%s/%s", kGalleryDir, name);

        // A thumbnail is small by construction; cap it far tighter than the
        // full image so a mislabelled field cannot smuggle a large file in.
        const uint32_t cap = isThumb ? Portal::kMaxThumbBytes : Portal::kMaxUploadBytes;

        concurrency::LockGuard g(spiLock);
        File f = galleryFs().open(path, FILE_O_WRITE);
        if (!f) {
            if (isThumb)
                continue;
            sendJson(res, "{\"error\":\"open\"}", 500);
            return;
        }

        uint32_t total = 0;
        uint8_t buf[512];
        bool overflow = false;
        while (!parser.endOfField()) {
            const size_t got = parser.read(buf, sizeof(buf));
            if (!got)
                break;
            total += got;
            if (total > cap) {
                overflow = true;
                break;
            }
            f.write(buf, got);
            esp_task_wdt_reset();
        }
        f.close();

        if (overflow) {
            galleryFs().remove(path);
            if (isThumb)
                continue; // keep the full image; the grid falls back to it
            sendJson(res, "{\"error\":\"too_large\"}", 413);
            return;
        }

        if (!isThumb) {
            wrote = true;
            strncpy(stored, name, sizeof(stored) - 1);
            stored[sizeof(stored) - 1] = '\0';
        }
    }

    if (!wrote) {
        sendJson(res, "{\"error\":\"no_file\"}", 400);
        return;
    }

    std::string out = "{\"ok\":true,\"name\":\"";
    out += stored;
    out += "\"}";
    sendJson(res, out);
}

// GET /* — static assets from LittleFS
static void handleStatic(HTTPRequest *req, HTTPResponse *res)
{
    std::string reqPath = req->getRequestString();
    if (reqPath.empty() || reqPath == "/")
        reqPath = "/index.html";

    // Refuse anything that tries to escape the asset directory.
    if (reqPath.find("..") != std::string::npos) {
        res->setStatusCode(400);
        res->print("bad path");
        return;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s%s", kWwwDir, reqPath.c_str());

    concurrency::LockGuard g(spiLock);
    File f = FSCom.open(path, FILE_O_READ);
    if (!f) {
        res->setStatusCode(404);
        res->setHeader("Content-Type", "text/html");
        res->print("<h1>PgrOS</h1><p>Portal assets are not installed. "
                   "Upload the filesystem image with <code>build.ps1 -Target fs</code>.</p>");
        return;
    }

    const char *type = "text/plain";
    if (reqPath.find(".html") != std::string::npos)
        type = "text/html";
    else if (reqPath.find(".css") != std::string::npos)
        type = "text/css";
    else if (reqPath.find(".js") != std::string::npos)
        type = "application/javascript";
    else if (reqPath.find(".svg") != std::string::npos)
        type = "image/svg+xml";
    res->setHeader("Content-Type", type);

    uint8_t buf[512];
    while (f.available()) {
        const size_t got = f.read(buf, sizeof(buf));
        if (!got)
            break;
        res->write(buf, got);
    }
    f.close();
}

// ---------------------------------------------------------------------------
// Portal
// ---------------------------------------------------------------------------

bool Portal::begin()
{
    detectGalleryStore();
    return true;
}

bool Portal::start(uint16_t port)
{
    if (mRunning)
        return true;

    // HTTP only, deliberately. TLS costs roughly 40 KB of heap per handshake
    // window, and a self-signed certificate on a local AP buys nothing but a
    // browser warning. The portal exposes no credentials and no configuration.
    sServer = new HTTPServer(port);
    if (!sServer) {
        LOG_ERROR("PgrOS portal: server alloc failed");
        return false;
    }

    sServer->registerNode(new ResourceNode("/api/room", "GET", &handleRoomGet));
    sServer->registerNode(new ResourceNode("/api/room", "POST", &handleRoomPost));
    sServer->registerNode(new ResourceNode("/api/gallery", "GET", &handleGalleryList));
    sServer->registerNode(new ResourceNode("/api/gallery", "DELETE", &handleGalleryDelete));
    sServer->registerNode(new ResourceNode("/api/upload", "POST", &handleUpload));
    sServer->registerNode(new ResourceNode("/photo/*", "GET", &handlePhoto));
    // Must be last: it matches everything.
    sServer->registerNode(new ResourceNode("/*", "GET", &handleStatic));

    sServer->start();
    mRunning = sServer->isRunning();
    if (mRunning)
        LOG_INFO("PgrOS portal: listening on :%u", (unsigned)port);
    else
        LOG_ERROR("PgrOS portal: failed to start");
    return mRunning;
}

void Portal::stop()
{
    if (!sServer)
        return;
    sServer->stop();
    delete sServer;
    sServer = nullptr;
    mRunning = false;
    LOG_INFO("PgrOS portal: stopped");
}

void Portal::loop()
{
    if (mRunning && sServer)
        sServer->loop();
}

bool Portal::postToRoom(const char *nick, const char *text, bool fromMesh)
{
    if (!text || !text[0])
        return false;

    PortalMessage &m = sRoom[sRoomHead];
    m.id = sNextMsgId++;
    m.atMs = millis();
    m.fromMesh = fromMesh;
    sanitiseText(nick ? nick : "guest", m.nick, sizeof(m.nick));
    sanitiseText(text, m.text, sizeof(m.text));

    sRoomHead = (uint16_t)((sRoomHead + 1) % kPortalHistory);
    if (sRoomCount < kPortalHistory)
        sRoomCount++;

    mStats.roomMessages = sRoomCount;

    // Optionally mirror onto the LoRa mesh. Off by default: pushing browser
    // chatter onto a shared mesh channel without the owner asking would be
    // antisocial, and the mesh has orders of magnitude less bandwidth.
    if (mMeshRelay && !fromMesh) {
        char line[kMaxTextLen + 1];
        snprintf(line, sizeof(line), "%s: %s", m.nick, m.text);
        service_.sendText(ThreadId::broadcast(0), line);
    }
    return true;
}

size_t Portal::roomHistory(PortalMessage *out, size_t max) const
{
    const uint16_t n = sRoomCount < max ? sRoomCount : (uint16_t)max;
    for (uint16_t i = 0; i < n; ++i) {
        const uint16_t idx = (uint16_t)((sRoomHead + kPortalHistory - n + i) % kPortalHistory);
        out[i] = sRoom[idx];
    }
    return n;
}

void Portal::clearRoom()
{
    sRoomCount = 0;
    sRoomHead = 0;
    mStats.roomMessages = 0;
}

size_t Portal::galleryList(GalleryItem *out, size_t max) const
{
    size_t n = 0;
    concurrency::LockGuard g(spiLock);

    File dir = galleryFs().open(kGalleryDir);
    if (!dir || !dir.isDirectory())
        return 0;

    File f = dir.openNextFile();
    while (f && n < max) {
        const char *base = strrchr(f.name(), '/');
        base = base ? base + 1 : f.name();

        // Thumbnails live in the same directory but are not gallery entries in
        // their own right; each is reported as a property of its full image.
        if (!f.isDirectory() && !isThumbName(base)) {
            strncpy(out[n].name, base, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = '\0';
            out[n].bytes = f.size();
            out[n].uploadedAt = 0;
            out[n].uploader[0] = '\0';
            out[n].hasThumb = false;
            out[n].width = 0;
            out[n].height = 0;
            n++;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    // Second pass for thumbnail presence. Done after the walk rather than inside
    // it because opening a second file while a directory iterator is live is not
    // something every FS implementation is happy about.
    for (size_t i = 0; i < n; ++i) {
        char thumb[96];
        thumbPathFor(out[i].name, thumb, sizeof(thumb));
        out[i].hasThumb = galleryFs().exists(thumb);
    }
    return n;
}

bool Portal::galleryDelete(const char *name)
{
    if (!validAssetName(name))
        return false;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kGalleryDir, name);

    concurrency::LockGuard g(spiLock);
    const bool ok = galleryFs().remove(path);

    // Take the thumbnail with it, or the gallery slowly fills with orphans that
    // nothing lists and nobody can delete.
    if (ok && !isThumbName(name)) {
        char thumb[96];
        thumbPathFor(name, thumb, sizeof(thumb));
        if (thumb[0] && galleryFs().exists(thumb))
            galleryFs().remove(thumb);
    }
    return ok;
}

void Portal::galleryClear()
{
    GalleryItem items[64];
    const size_t n = galleryList(items, 64);
    for (size_t i = 0; i < n; ++i)
        galleryDelete(items[i].name);
}

uint32_t Portal::galleryFreeBytes() const
{
#if defined(HAS_SDCARD)
    if (sUseSd) {
        concurrency::LockGuard g(spiLock);
        const uint64_t total = SD.totalBytes();
        const uint64_t used = SD.usedBytes();
        return (uint32_t)((total > used) ? (total - used) : 0);
    }
#endif
    const size_t total = fsTotalBytes();
    const size_t used = fsUsedBytes();
    const size_t free = (total > used) ? (total - used) : 0;
    // Keep headroom on internal flash so chat history and config can always be
    // written; photos must never be able to fill the partition.
    return free > kGalleryReserveBytes ? (uint32_t)(free - kGalleryReserveBytes) : 0;
}

} // namespace pgros

#endif // PGROS
