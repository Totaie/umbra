#!/usr/bin/env python3
"""Install the Qt SDK that Umbra builds against.

Usage:
    pip install aqtinstall
    python scripts/setup-qt.py [--qt-version 6.11.1] [--outdir D:/Qt]

Why this exists instead of a plain `aqt install-qt` line:

aqtinstall (through at least 3.3.0) does not know about the per-compiler
repository layout Qt adopted in 6.8. QtRepoProperty.extension_for_arch() only
returns a suffix for wasm and android, so for a Windows MSVC kit
ArchiveId.to_folder() builds

    online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml

when the real path is

    online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111_msvc2022_64/Updates.xml

Every metadata fetch then 404s, which aqt reports as the rather misleading
"Failed to download checksum for the file 'Updates.xml'". We restore the missing
suffix by stripping the "win<bits>_" prefix off the architecture name.

Remove this shim once aqtinstall handles the 6.8+ layout itself.
"""
import argparse
import re
import sys

try:
    from aqt.metadata import QtRepoProperty
except ImportError:
    sys.exit("aqtinstall is not installed. Run: pip install aqtinstall")

_WIN_ARCH = re.compile(r"^win\d+_(?P<suffix>.+)$")
_original_extension_for_arch = QtRepoProperty.extension_for_arch

# Modules Umbra needs beyond the Qt essentials. qtdeclarative (Quick, Quick
# Controls) and qtsvg are part of the base install and are not valid here.
DEFAULT_MODULES = ["qtmultimedia", "qtimageformats", "qt5compat", "qtshadertools"]


def extension_for_arch(architecture: str, is_version_ge_6: bool) -> str:
    ext = _original_extension_for_arch(architecture, is_version_ge_6)
    if ext:
        return ext

    match = _WIN_ARCH.match(architecture or "")
    if match and is_version_ge_6:
        suffix = match.group("suffix")
        # ARM64 cross builds live in a differently named folder
        if suffix.startswith("msvc") and suffix.endswith("_arm64"):
            return suffix + "_cross_compiled"
        return suffix

    return ext


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-version", default="6.11.1",
                        help="Qt version to install (default: %(default)s)")
    parser.add_argument("--arch", default="win64_msvc2022_64",
                        help="Qt architecture (default: %(default)s)")
    parser.add_argument("--outdir", default="D:/Qt",
                        help="Install location (default: %(default)s)")
    args = parser.parse_args()

    QtRepoProperty.extension_for_arch = staticmethod(extension_for_arch)

    from aqt.__main__ import main as aqt_main

    sys.argv = [
        "aqt", "install-qt", "windows", "desktop",
        args.qt_version, args.arch,
        "-O", args.outdir,
        "-m", *DEFAULT_MODULES,
    ]
    print("Installing Qt {} ({}) to {}".format(args.qt_version, args.arch, args.outdir))
    return aqt_main()


if __name__ == "__main__":
    sys.exit(main())
