#!/usr/bin/env python3
"""Require the configured headers from the target Asterisk build, not system PJ headers."""

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import subprocess
import sys
from pathlib import Path

SUPPORTED = "22.11.0"


def check(source):
    if not source:
        raise ValueError(
            "Set ASTERISK_SRC to the configured source tree used to build your Asterisk."
        )
    source = Path(source).resolve()
    paths = [
        "include/asterisk.h",
        ".version",
        "include/asterisk/buildopts.h",
        "include/asterisk/autoconfig.h",
        "third-party/pjproject/source/pjlib/include/pj/config_site.h",
        "third-party/pjproject/source/pjlib/include/pj/compat/os_auto.h",
        "third-party/pjproject/source/pjlib/include/pj/compat/m_auto.h",
        "third-party/pjproject/source/pjsip/include/pjsip.h",
    ]
    for name in paths:
        if not (source / name).is_file():
            raise ValueError(
                f"Missing {name}. Build Asterisk with bundled PJPROJECT first; generic system PJPROJECT headers are not compatible."
            )
    version = (source / paths[1]).read_text().strip()
    if version != SUPPORTED:
        raise ValueError(f"This release supports Asterisk {SUPPORTED} only.")
    config = (source / paths[3]).read_text()
    if not re.search(r"^#define\s+HAVE_PJPROJECT_BUNDLED\s+1\b", config, re.MULTILINE):
        raise ValueError("This build path requires Asterisk's bundled PJPROJECT.")
    buildopts = re.search(
        r'#define\s+AST_BUILDOPT_SUM\s+"([^"]+)"', (source / paths[2]).read_text()
    )
    if not buildopts:
        raise ValueError("Asterisk buildopts.h is not generated; build Asterisk first.")
    saved_flags = source / "pjproject-compiler-flags"
    flags = (
        saved_flags.read_text()
        if saved_flags.exists()
        else subprocess.check_output(
            [
                "make",
                "--no-print-directory",
                "-s",
                "-C",
                str(source / "third-party/pjproject"),
                "echo_cflags",
            ],
            text=True,
            timeout=30,
        )
    )
    defines = [flag for flag in shlex.split(flags) if flag.startswith(("-D", "-U"))]
    if "-DPJ_AUTOCONF=1" not in defines:
        raise ValueError(
            "Cannot obtain the bundled PJPROJECT compiler definitions from this SDK."
        )
    return {
        "asterisk": version,
        "architecture": platform.machine(),
        "asterisk_buildopts": buildopts[1],
        "pjproject_compiler_defines": defines,
        "sdk_headers_sha256": {
            p: hashlib.sha256((source / p).read_bytes()).hexdigest() for p in paths
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()
    try:
        info = check(args.source)
        info["addon_version"] = os.environ.get("ADDON_VERSION", "dev")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", info["addon_version"]):
            raise ValueError("ADDON_VERSION must be a safe tag or commit label.")
        if args.manifest.exists() and json.loads(args.manifest.read_text()) != info:
            raise ValueError(
                "SDK or addon version changed. Run 'make clean' before rebuilding."
            )
        args.manifest.write_text(json.dumps(info, indent=2) + "\n")
        lines = ["/* Generated from the target Asterisk's bundled PJPROJECT build. */"]
        for flag in info["pjproject_compiler_defines"]:
            name, _, value = flag[2:].partition("=")
            if not re.fullmatch(r"[A-Za-z_]\w*", name) or "\n" in value:
                raise ValueError(f"Unsupported PJPROJECT compiler definition: {flag}")
            lines.extend([f"#ifdef {name}", f"#undef {name}", "#endif"])
            if flag.startswith("-D"):
                lines.append(f"#define {name} {value or '1'}")
        args.header.write_text("\n".join(lines) + "\n")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(f"SDK check failed: {error}")


if __name__ == "__main__":
    main()
