#!/usr/bin/env python3
"""Derive Moonlight build versions from CI_VERSION or Git tags.

Version precedence:
  1. CI_VERSION environment variable (explicit CI/release override)
  2. `git describe --tags --long --dirty` from the source tree
  3. app/version.txt fallback for source archives without Git metadata

The script intentionally emits separate display/artifact/numeric forms because
Windows file versions and Apple bundle versions must remain numeric, while
application/about-box versions can include Git metadata.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
from pathlib import Path


SEMVER_RE = re.compile(
    r"^v?(?P<base>\d+\.\d+\.\d+)"
    r"(?:-(?P<count>\d+)-g(?P<sha>[0-9a-fA-F]+))?"
    r"(?P<dirty>-dirty)?$"
)


def run_git(source_root: Path, *args: str) -> str | None:
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=source_root,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def read_fallback_version(source_root: Path) -> str:
    version_file = source_root / "app" / "version.txt"
    try:
        return version_file.read_text(encoding="utf-8").strip()
    except OSError:
        return "0.0.0"


def split_numeric_base(version: str) -> tuple[int, int, int]:
    match = re.search(r"(\d+)\.(\d+)\.(\d+)", version)
    if not match:
        return (0, 0, 0)
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def clamp_u16(value: int) -> int:
    return max(0, min(value, 65535))


def derive(source_root: Path) -> dict[str, str]:
    ci_version = os.environ.get("CI_VERSION", "").strip()
    fallback = read_fallback_version(source_root)

    if ci_version:
        base = ".".join(str(part) for part in split_numeric_base(ci_version))
        major, minor, patch = split_numeric_base(base)
        return {
            "base": base,
            "display": ci_version,
            "artifact": ci_version.replace("+", "-"),
            "numeric": f"{major}.{minor}.{patch}.0",
            "describe": ci_version,
            "commit": run_git(source_root, "rev-parse", "--short=8", "HEAD") or "unknown",
        }

    describe = run_git(
        source_root,
        "describe",
        "--tags",
        "--long",
        "--dirty",
        "--match",
        "v[0-9]*",
        "--match",
        "[0-9]*",
    )
    commit = run_git(source_root, "rev-parse", "--short=8", "HEAD") or "unknown"

    if describe:
        match = SEMVER_RE.match(describe)
        if match:
            base = match.group("base")
            count = int(match.group("count") or 0)
            sha = match.group("sha") or commit
            dirty = bool(match.group("dirty"))
            major, minor, patch = split_numeric_base(base)

            if count == 0 and not dirty:
                display = base
            else:
                display = f"{base}+{count}.g{sha[:8]}"
                if dirty:
                    display += ".dirty"

            return {
                "base": base,
                "display": display,
                "artifact": display.replace("+", "-"),
                "numeric": f"{major}.{minor}.{patch}.{clamp_u16(count)}",
                "describe": describe,
                "commit": commit,
            }

    major, minor, patch = split_numeric_base(fallback)
    return {
        "base": fallback,
        "display": fallback,
        "artifact": fallback.replace("+", "-"),
        "numeric": f"{major}.{minor}.{patch}.0",
        "describe": fallback,
        "commit": commit,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Moonlight Qt source root (defaults to this script's parent repo)",
    )
    parser.add_argument(
        "--field",
        choices=("base", "display", "artifact", "numeric", "describe", "commit"),
        default="display",
        help="Single field to print",
    )
    parser.add_argument(
        "--write-header",
        metavar="PATH",
        help=(
            "Write a C header defining VERSION_STR and rewrite it only when the "
            "contents change, so the version reaches object files through a real "
            "dependency instead of a compiler define"
        ),
    )
    parser.add_argument(
        "--format",
        choices=("plain", "env", "qmake"),
        default="plain",
        help="Output format for all fields (plain prints only --field)",
    )
    args = parser.parse_args()

    source_root = Path(args.source_root).resolve()
    info = derive(source_root)

    if args.write_header:
        # The version used to arrive as -DVERSION_STR. Changing a define does not
        # invalidate anything already compiled, so an object built at 0.2.1 kept
        # reporting 0.2.1 through every later release while the resource-compiled
        # FileVersion moved on - the app said one version and the installer another.
        # A header is a dependency make understands.
        header = (
            "// Generated by scripts/derive-version.py. Do not edit.\n"
            "//\n"
            "// The single source of truth for the version is app/version.txt, via\n"
            "// CI_VERSION. Everything that reports a version reads it from here.\n"
            "#pragma once\n"
            "\n"
            '#define VERSION_STR "%s"\n'
            '#define VERSION_NUMERIC_STR "%s"\n'
        ) % (info["display"], info["numeric"])

        path = Path(args.write_header)
        # Only touch the file when it actually changes: an unconditional write would
        # make every qmake run look like a version change and rebuild the world.
        if not path.exists() or path.read_text(encoding="utf-8") != header:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(header, encoding="utf-8", newline="\n")

    if args.format == "env":
        for key in ("base", "display", "artifact", "numeric", "describe", "commit"):
            print(f"MOONLIGHT_{key.upper()}={info[key]}")
    elif args.format == "qmake":
        print(f"MOONLIGHT_VERSION = {info['display']}")
        print(f"MOONLIGHT_ARTIFACT_VERSION = {info['artifact']}")
        print(f"MOONLIGHT_NUMERIC_VERSION = {info['numeric']}")
        print(f"MOONLIGHT_GIT_COMMIT = {info['commit']}")
    else:
        print(info[args.field])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
