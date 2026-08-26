"""
PlatformIO extra_script: stage the PgrOS portal assets into the LittleFS image.

The PlatformIO project root is vendor/firmware, so `pio run -t uploadfs` builds
its image from vendor/firmware/data. PgrOS keeps its own assets in PgrOS/data/www
(versioned with PgrOS, not with the vendored firmware), so they have to be copied
across before the image is built.

They land at /pgros/www/ on the device, which is where net/Portal.cpp looks for
them. Meshtastic's own data/ contents are left untouched -- we only add a
subdirectory.
"""

import os
import shutil

Import("env")  # noqa: F821  (injected by PlatformIO)

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821  -> vendor/firmware
PGROS_ROOT = os.path.abspath(os.path.join(PROJECT_DIR, "..", ".."))

SRC = os.path.join(PGROS_ROOT, "data", "www")
DST = os.path.join(PROJECT_DIR, "data", "pgros", "www")


def stage_assets(*_args, **_kwargs):
    if not os.path.isdir(SRC):
        print("PgrOS: no portal assets at %s; skipping" % SRC)
        return

    # Mirror rather than merge, so a file deleted from the repo does not linger
    # in the image forever.
    if os.path.isdir(DST):
        shutil.rmtree(DST)
    os.makedirs(DST, exist_ok=True)

    total = 0
    for root, _dirs, files in os.walk(SRC):
        rel = os.path.relpath(root, SRC)
        target = DST if rel == "." else os.path.join(DST, rel)
        os.makedirs(target, exist_ok=True)
        for name in files:
            src_file = os.path.join(root, name)
            shutil.copy2(src_file, os.path.join(target, name))
            total += os.path.getsize(src_file)

    print("PgrOS: staged portal assets -> data/pgros/www (%d bytes)" % total)


# Run before the filesystem image is built, and also on a normal build so the
# staged copy never goes stale.
env.AddPreAction("$BUILD_DIR/littlefs.bin", stage_assets)  # noqa: F821
stage_assets()
