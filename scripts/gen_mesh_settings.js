/*
 * Generate src/pgros/ui/apps/MeshSettingsTable.h from the vendored protobufs.
 *
 * PgrOS exposes a small, curated set of Meshtastic node settings on-device --
 * the phone app remains the better place to configure a node, and a 480x222
 * screen driven by a rotary is no place for MQTT URLs or 32-byte keys. What is
 * NOT hand-written is the enum vocabularies: role, region, modem preset and
 * pairing mode all gain values upstream over time, and a hand-copied table would
 * silently drift the moment the submodule pin moves.
 *
 * So the field list is chosen here by a human, and every enum's values and names
 * are read out of vendor/firmware/protobufs/meshtastic/config.proto.
 *
 * Run after moving the submodule pin:
 *     node scripts/gen_mesh_settings.js
 *
 * Deliberately not wired into the build: the output is committed, and a build
 * step that rewrites a tracked source file is a nuisance when bisecting.
 */

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const protoFile = path.join(root, "vendor", "firmware", "protobufs", "meshtastic", "config.proto");
const outFile = path.join(root, "src", "pgros", "ui", "apps", "MeshSettingsTable.h");

if (!fs.existsSync(protoFile)) {
  console.error("Cannot find " + protoFile + "\nIs the protobufs submodule checked out?");
  process.exit(1);
}
const proto = fs.readFileSync(protoFile, "utf8");

/* The enums we surface, and the C prefix nanopb gives each value. */
const wanted = [
  { enumName: "Role", cPrefix: "meshtastic_Config_DeviceConfig_Role_", table: "kRoleValues" },
  { enumName: "RegionCode", cPrefix: "meshtastic_Config_LoRaConfig_RegionCode_", table: "kRegionValues" },
  { enumName: "ModemPreset", cPrefix: "meshtastic_Config_LoRaConfig_ModemPreset_", table: "kPresetValues" },
  { enumName: "PairingMode", cPrefix: "meshtastic_Config_BluetoothConfig_PairingMode_", table: "kPairingValues" },
];

/* Tokens that should keep their capitalisation when a SCREAMING_SNAKE name is
   turned into something readable. Without this, RANDOM_PIN reads "Random Pin"
   and EU_868 reads "Eu 868". */
const acronyms = new Set([
  "PIN", "US", "EU", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN", "NZ", "TH",
  "UA", "MY", "SG", "PH", "KZ", "NP", "BR", "TAK", "MQTT", "GPS", "LORA",
]);

function pretty(name) {
  return name
    .split("_")
    .map((tok) => {
      if (acronyms.has(tok)) return tok === "LORA" ? "LoRa" : tok;
      if (/^\d+$/.test(tok)) return tok;
      return tok.charAt(0) + tok.slice(1).toLowerCase();
    })
    .join(" ");
}

/* Pull `NAME = N;` pairs out of `enum <name> { ... }`, skipping comments. */
function parseEnum(enumName) {
  const start = proto.search(new RegExp("\\benum\\s+" + enumName + "\\s*\\{"));
  if (start < 0) return null;

  let i = proto.indexOf("{", start);
  let depth = 0;
  let end = i;
  for (; end < proto.length; end++) {
    if (proto[end] === "{") depth++;
    else if (proto[end] === "}") {
      depth--;
      if (depth === 0) break;
    }
  }

  const body = proto
    .slice(i + 1, end)
    .replace(/\/\*[\s\S]*?\*\//g, "")   // block comments
    .replace(/\/\/[^\n]*/g, "");        // line comments

  // Capture the option block too, so deprecated values are dropped on purpose
  // rather than by accident. Meshtastic retires roles this way -- ROUTER_CLIENT
  // and REPEATER are both `[deprecated = true]` -- and offering them in a picker
  // would invite someone to select a role upstream no longer wants used.
  const out = [];
  const re = /([A-Z][A-Z0-9_]*)\s*=\s*(\d+)\s*(\[[^\]]*\])?\s*;/g;
  let m;
  let skipped = 0;
  while ((m = re.exec(body)) !== null) {
    if (m[3] && /deprecated\s*=\s*true/.test(m[3])) {
      skipped++;
      continue;
    }
    out.push({ name: m[1], value: Number(m[2]) });
  }
  out.skipped = skipped;
  return out;
}

let src = `#pragma once
// GENERATED FILE -- do not edit by hand.
//
// Produced by scripts/gen_mesh_settings.js from
// vendor/firmware/protobufs/meshtastic/config.proto.
// Regenerate after moving the submodule pin:
//
//     node scripts/gen_mesh_settings.js
//
// Only the enum vocabularies are generated. Which settings PgrOS surfaces is a
// curated decision and lives in SettingsApp.cpp -- the phone app is still the
// right place to configure a node, and most of the config surface has no
// business on a 480x222 screen driven by a rotary.

#include <stdint.h>

namespace pgros {

struct MeshEnumValue {
    int32_t value;
    const char *label;
};

`;

let totalValues = 0;
for (const w of wanted) {
  const values = parseEnum(w.enumName);
  if (!values || !values.length) {
    console.error("Could not parse enum " + w.enumName + " from config.proto");
    process.exit(1);
  }
  totalValues += values.length;

  src += `// meshtastic ${w.enumName} (${values.length} values)\n`;
  src += `static const MeshEnumValue ${w.table}[] = {\n`;
  for (const v of values) {
    src += `    {${w.cPrefix}${v.name}, "${pretty(v.name)}"},\n`;
  }
  src += `};\nstatic const uint8_t ${w.table}Count = ${values.length};\n\n`;

  console.log(`  ${w.enumName}: ${values.length} values` + (values.skipped ? ` (${values.skipped} deprecated, skipped)` : ""));
}

src += "} // namespace pgros\n";

fs.mkdirSync(path.dirname(outFile), { recursive: true });
fs.writeFileSync(outFile, src);
console.log(`Wrote ${path.relative(root, outFile)} (${totalValues} enum values)`);
