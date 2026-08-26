"""
PlatformIO pre-script for the PgrOS overlay.

Two jobs, both of which have to happen outside platformio.ini:

1. Put src/pgros on the GLOBAL include path.

   An `-I` in an environment's `build_flags` reaches the project's own sources
   but NOT the libraries PlatformIO builds alongside them. That matters here
   because LVGL resolves `lv_conf.h` from its own include path: with the flag
   only on the project env, every LVGL translation unit fails with
   "fatal error: lv_conf.h: No such file or directory" while our own files
   compile fine.

   Appending to `env` (rather than `projenv`) applies to library builds too,
   which is what LVGL needs. Note also that `${PROJECT_DIR}` does NOT expand to
   the project directory inside `build_flags` -- it resolves to the platform
   package directory and silently produces a bogus path -- so the location is
   computed here instead.

2. Stage the portal assets into the LittleFS image.

   The PlatformIO project root is vendor/firmware, so `pio run -t uploadfs`
   builds its image from vendor/firmware/data. PgrOS keeps its assets in
   PgrOS/data/www so they are versioned with PgrOS rather than with the
   vendored firmware; they are copied to data/pgros/www, which is where
   net/Portal.cpp looks for them.
"""

import os
import shutil

Import("env")  # noqa: F821  (injected by PlatformIO)

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821  -> vendor/firmware
PGROS_ROOT = os.path.abspath(os.path.join(PROJECT_DIR, "..", ".."))

PGROS_SRC = os.path.join(PGROS_ROOT, "src", "pgros")
ASSET_SRC = os.path.join(PGROS_ROOT, "data", "www")
ASSET_DST = os.path.join(PROJECT_DIR, "data", "pgros", "www")


# --- 1. include path -------------------------------------------------------

if not os.path.isdir(PGROS_SRC):
    raise SystemExit("PgrOS: expected sources at %s" % PGROS_SRC)

env.Append(CPPPATH=[PGROS_SRC])  # noqa: F821
print("PgrOS: added %s to the global include path" % PGROS_SRC)


# --- 1b. point LVGL at our lv_conf.h ---------------------------------------
#
# CPPPATH still does not reach library builds even from here -- PlatformIO
# rebuilds the include path per library -- but PREPROCESSOR DEFINES do. LVGL
# offers exactly the escape hatch we need: if LV_CONF_PATH is defined it is used
# verbatim as the include, ahead of every other lookup:
#
#     #ifdef LV_CONF_PATH
#         #include LV_CONF_PATH
#     #elif defined(LV_CONF_INCLUDE_SIMPLE)
#         #include "lv_conf.h"
#
# So hand it an absolute path. It has to be computed here rather than written
# into platformio.ini, because it differs per checkout. Forward slashes work on
# Windows and avoid backslash-escaping inside the string literal.

LV_CONF = os.path.join(PGROS_SRC, "ui", "lv_conf.h").replace("\\", "/")
if not os.path.isfile(LV_CONF):
    raise SystemExit("PgrOS: expected LVGL config at %s" % LV_CONF)

env.Append(CPPDEFINES=[("LV_CONF_PATH", env.StringifyMacro(LV_CONF))])  # noqa: F821
print("PgrOS: LV_CONF_PATH -> %s" % LV_CONF)


# --- 2. portal assets ------------------------------------------------------


def stage_assets(*_args, **_kwargs):
    if not os.path.isdir(ASSET_SRC):
        print("PgrOS: no portal assets at %s; skipping" % ASSET_SRC)
        return

    # Mirror rather than merge, so a file deleted from the repo does not linger
    # in the image forever.
    if os.path.isdir(ASSET_DST):
        shutil.rmtree(ASSET_DST)
    os.makedirs(ASSET_DST, exist_ok=True)

    total = 0
    for root, _dirs, files in os.walk(ASSET_SRC):
        rel = os.path.relpath(root, ASSET_SRC)
        target = ASSET_DST if rel == "." else os.path.join(ASSET_DST, rel)
        os.makedirs(target, exist_ok=True)
        for name in files:
            src_file = os.path.join(root, name)
            shutil.copy2(src_file, os.path.join(target, name))
            total += os.path.getsize(src_file)

    print("PgrOS: staged portal assets -> data/pgros/www (%d bytes)" % total)


stage_assets()
