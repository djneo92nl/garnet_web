# PlatformIO library extraScript: regenerate src/gw_ui_gz.h from web/
# before compiling, so an edited UI can never ship with a stale header.
# The header is committed too, so a build without this hook still works,
# and build_ui.py only rewrites it when the content actually changed.
Import("env")  # noqa: F821 - provided by SCons

import pathlib
import sys

# SConscript runs with the script's own directory as ".", but be lenient
# about whether that resolves to tools/ or the library root.
base = pathlib.Path(Dir(".").srcnode().abspath)  # noqa: F821
tools = base if (base / "build_ui.py").exists() else base / "tools"
sys.path.insert(0, str(tools))
import build_ui  # noqa: E402

build_ui.main([])
