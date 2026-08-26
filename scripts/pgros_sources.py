"""
PlatformIO post-script: give the PgrOS sources the project's include path.

PgrOS sources are pulled into the build by `build_src_filter` in pgros.ini --
the same mechanism upstream already uses to reach outside `src_dir` for variant
directories. That compiles them, but it does not make the Library Dependency
Finder aware of them: LDF only scans `src_dir`, and LDF is what attaches each
library's include directory to the compile command.

The result is that PgrOS files get the project's defines but not its library
include paths, and the build dies on a header belonging to a library that is
otherwise built and linked perfectly well:

    src/mesh/generated/meshtastic/mesh.pb.h:6:10: fatal error: pb.h: No such file

`projenv` is the environment PlatformIO uses for the project's own sources, and
by this point LDF has finished populating its CPPPATH with every resolved
library. Copying that onto the global env gives our tree exactly the same view
of the world that Meshtastic's own files have -- which is correct, because PgrOS
is project code that happens to live outside src_dir, not a library.

This has to be a `post:` script: `projenv` does not exist in a `pre:` one, and
LDF has not run yet either.
"""

import os

Import("env", "projenv")  # noqa: F821  (projenv exists in post-scripts only)

PROJECT_DIR = projenv["PROJECT_DIR"]  # noqa: F821  -> vendor/firmware
PGROS_ROOT = os.path.abspath(os.path.join(PROJECT_DIR, "..", ".."))
PGROS_SRC = os.path.join(PGROS_ROOT, "src", "pgros")

if not os.path.isdir(PGROS_SRC):
    raise SystemExit("PgrOS: expected sources at %s" % PGROS_SRC)

project_paths = projenv.get("CPPPATH", [])  # noqa: F821
existing = set(str(p) for p in env.get("CPPPATH", []))  # noqa: F821

added = [p for p in project_paths if str(p) not in existing]
if added:
    env.Append(CPPPATH=added)  # noqa: F821

# PgrOS's own root, so `#include "core/Boot.h"` resolves from any file in the
# tree. Also added in the pre-script, because LVGL needs it before this runs.
if PGROS_SRC not in existing:
    env.Append(CPPPATH=[PGROS_SRC])  # noqa: F821

print("PgrOS: inherited %d project include paths for out-of-tree sources" % len(added))
