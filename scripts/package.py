#!/usr/bin/env python3
"""Create a build-specific binary archive and the corresponding source archive."""

import gzip
import hashlib
import json
import tarfile
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def normalized(member):
    if "__pycache__" in member.name or member.name.endswith(".pyc"):
        return None
    member.uid = member.gid = member.mtime = 0
    member.uname = member.gname = ""
    member.mode = 0o755 if member.isdir() or member.mode & 0o111 else 0o644
    return member


@contextmanager
def reproducible_archive(path):
    with (
        path.open("wb") as raw,
        gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed,
        tarfile.open(fileobj=compressed, mode="w") as archive,
    ):
        yield archive


def main():
    build = ROOT / "build"
    info = json.loads((build / "build-info.json").read_text())
    version = info["addon_version"]
    stem = f"asterisk-jio-{version}-asterisk-{info['asterisk']}-linux-{info['architecture']}-{info['asterisk_buildopts'][:12]}"
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    binary = dist / f"{stem}.tar.gz"
    with reproducible_archive(binary) as archive:
        for name in [
            "codec_amr.so",
            "res_pjsip_amr.so",
            "res_pjsip_jio.so",
            "build-info.json",
        ]:
            archive.add(build / name, arcname=f"{stem}/{name}", filter=normalized)
        for name in ["README.md", "LICENSE", "examples"]:
            archive.add(ROOT / name, arcname=f"{stem}/{name}", filter=normalized)
    source = dist / f"asterisk-jio-{version}-source.tar.gz"
    with reproducible_archive(source) as archive:
        for name in [
            "src",
            "examples",
            "tests",
            "scripts",
            "ci",
            "nix",
            ".github",
            ".dockerignore",
            ".gitignore",
            "Makefile",
            "LICENSE",
            "README.md",
            "flake.nix",
            "flake.lock",
        ]:
            archive.add(
                ROOT / name, arcname=f"asterisk-jio-{version}/{name}", filter=normalized
            )
    for path in [binary, source]:
        (dist / f"{path.name}.sha256").write_text(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        )
        print(path.name)


if __name__ == "__main__":
    main()
