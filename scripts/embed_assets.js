/*
 * Generate src/pgros/net/PortalAssets.cpp from data/www/.
 *
 * The portal's assets are tiny -- under 20 KB for all three -- and the app
 * partition has megabytes spare, so building them into the firmware removes an
 * entire class of problem: no filesystem image to flash, nothing to lose when
 * that image is written, and no dependency on whatever file manager happens to
 * be available for a given device.
 *
 * The on-disk copies still win when present, so the portal can be customised
 * without a rebuild. See Portal::handleStatic().
 *
 * Run:  node scripts/embed_assets.js
 * This is deliberately NOT wired into the build. The assets change rarely, the
 * generated file is committed, and a build step that rewrites a source file is
 * a nuisance when bisecting.
 */

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const srcDir = path.join(root, "data", "www");
const outFile = path.join(root, "src", "pgros", "net", "PortalAssets.cpp");

const mimes = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".svg": "image/svg+xml",
  ".json": "application/json",
};

const files = fs
  .readdirSync(srcDir)
  .filter((f) => fs.statSync(path.join(srcDir, f)).isFile())
  .sort();

if (!files.length) {
  console.error("No assets found in " + srcDir);
  process.exit(1);
}

let out = `// GENERATED FILE -- do not edit by hand.
//
// Produced by scripts/embed_assets.js from data/www/.
// Regenerate after changing the portal assets:
//
//     node scripts/embed_assets.js
//
// These are the fallback copies, built into the firmware so the portal works on
// a device that has never had the filesystem image flashed. A file of the same
// name on the SD card or in /pgros/www takes precedence.

#ifdef PGROS

#include "net/PortalAssets.h"

namespace pgros
{

`;

let total = 0;
const entries = [];

for (const name of files) {
  const buf = fs.readFileSync(path.join(srcDir, name));
  const ext = path.extname(name).toLowerCase();
  const mime = mimes[ext] || "application/octet-stream";
  const sym = "kAsset_" + name.replace(/[^A-Za-z0-9]/g, "_");
  total += buf.length;

  out += `// ${name} (${buf.length} bytes)\nstatic const uint8_t ${sym}[] = {\n`;
  for (let i = 0; i < buf.length; i += 16) {
    const row = [...buf.subarray(i, i + 16)]
      .map((b) => "0x" + b.toString(16).padStart(2, "0"))
      .join(", ");
    out += "    " + row + ",\n";
  }
  out += "};\n\n";

  entries.push({ name, mime, sym, len: buf.length });
}

out += "const EmbeddedAsset kEmbeddedAssets[] = {\n";
for (const e of entries) {
  out += `    {"/${e.name}", "${e.mime}", ${e.sym}, ${e.len}},\n`;
}
out += "};\n\n";
out += `const size_t kEmbeddedAssetCount = ${entries.length};\n\n`;
out += "} // namespace pgros\n\n#endif // PGROS\n";

fs.mkdirSync(path.dirname(outFile), { recursive: true });
fs.writeFileSync(outFile, out);

console.log(
  `Embedded ${entries.length} asset(s), ${total} bytes -> ` +
    path.relative(root, outFile)
);
for (const e of entries) console.log(`  /${e.name}  ${e.len}  ${e.mime}`);
