#pragma once
//
// Portal assets built into the firmware.
//
// The web UI is under 20 KB and the app partition has megabytes spare, so the
// assets ship inside the binary. That removes a whole class of problem: there is
// no filesystem image to flash, nothing to lose when one is written (that image
// is a whole-partition write and takes the Meshtastic config, the channels, the
// node keypair and the chat history with it), and no dependency on whatever file
// manager a given device happens to offer.
//
// On-disk copies still win when present, so the portal can be customised without
// rebuilding -- drop files in /pgros/www on the SD card or on internal flash.
// See Portal::handleStatic() for the lookup order.
//
// The .cpp is generated. Regenerate it after editing data/www/:
//
//     node scripts/embed_assets.js

#include <stddef.h>
#include <stdint.h>

namespace pgros {

struct EmbeddedAsset {
    const char *path; // request path, with the leading slash, e.g. "/app.js"
    const char *mime;
    const uint8_t *data;
    size_t len;
};

extern const EmbeddedAsset kEmbeddedAssets[];
extern const size_t kEmbeddedAssetCount;

// Returns the asset for a request path, or nullptr. "/" maps to "/index.html".
const EmbeddedAsset *findEmbeddedAsset(const char *requestPath);

} // namespace pgros
