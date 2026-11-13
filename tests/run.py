#!/usr/bin/env python3
"""Test a separate localhost Asterisk. Only dummy localhost registration; no host configuration."""

import argparse
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import outbound
import sip
from configure import configure

import jio


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument(
        "--suite",
        choices=["all", "sip", "outbound", "failure", "resident"],
        default="all",
    )
    args = parser.parse_args()
    package, addon = args.prefix.resolve(), args.addon.resolve()
    binary = package / "sbin/asterisk"
    if not binary.is_file():
        parser.error(
            "Set ASTERISK_PREFIX to the matching Asterisk installation prefix."
        )
    artifact = Path(f"build/test-{args.suite}.log").resolve()
    artifact.parent.mkdir(exist_ok=True)
    with (
        tempfile.TemporaryDirectory(prefix="amr-test-") as temp,
        artifact.open("w+") as log,
        jio.registrar() as registrar,
    ):
        base = Path(temp)
        port = free_port()
        config = configure(base, package, addon, port, registrar.getsockname()[1])
        if args.suite == "resident":
            modules = base / "etc/modules.conf"
            modules.write_text(
                modules.read_text()
                .replace("require=res_pjsip_amr.so\n", "")
                .replace("require=amr_validation.so\n", "")
            )
        process = subprocess.Popen(
            [str(binary), "-f", "-g", "-n", "-C", str(config)],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            cli = [str(binary), "-C", str(config), "-rx"]
            if args.suite == "failure":
                code = process.wait(timeout=30)
                output = artifact.read_text()
                assert code > 0, ("startup must fail normally, not crash", code)
                assert "AMR initialization failed" in output
                assert "AMR codec initialization incomplete" in output
                print(
                    "PASS: incomplete codec initialization stops required-module startup",
                    flush=True,
                )
                return
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(
                        f"Isolated Asterisk exited: {process.returncode}"
                    )
                if (base / "run/asterisk.ctl").exists():
                    result = subprocess.run(
                        cli + ["core waitfullybooted"],
                        capture_output=True,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                    if "fully booted" in result.stdout:
                        break
                time.sleep(0.05)
            else:
                raise RuntimeError("Isolated Asterisk did not start")
            if args.suite == "resident":
                assert "AMR initialization failed" in artifact.read_text()
                for command in [
                    "module unload codec_amr.so",
                    "module load codec_amr.so",
                ]:
                    subprocess.run(
                        cli + [command], capture_output=True, check=False, timeout=5
                    )
                result = subprocess.run(
                    cli + ["module show like codec_amr.so"],
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=5,
                )
                assert "Running" in result.stdout, result.stdout
                assert process.poll() is None
                print(
                    "PASS: failed module stays resident through unload/reload attempts",
                    flush=True,
                )
                return
            jio.run(registrar, cli)
            assert "JIO CONTACT VALIDATION PASSED" in artifact.read_text()
            if "AMR VALIDATION ALL PASSED" not in artifact.read_text():
                raise RuntimeError("Codec/format validation did not pass")
            print(
                "PASS: 34 audio combinations with independent decoder checks",
                flush=True,
            )
            sip.server = ("127.0.0.1", port)
            if args.suite in ("all", "sip"):
                sip.run()
            if args.suite in ("all", "outbound"):
                outbound.run(cli, free_port())
            if process.poll() is not None:
                raise RuntimeError("Asterisk crashed during tests")
            print("ALL ADDON TESTS PASSED", flush=True)
        except Exception:
            log.flush()
            print(artifact.read_text()[-16000:])
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
