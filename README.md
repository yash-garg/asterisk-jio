# Asterisk Jio

Jio IMS compatibility and AMR/AMR-WB support for **Asterisk 22.11.0 on Linux**.
One addon builds and installs all three modules:

- `res_pjsip_jio.so`: initial empty Digest authorization, Jio's `483` authentication
  challenge handling, IMS access-network headers, and Contact capabilities.
- `codec_amr.so`: AMR/AMR-WB registration and transcoding with OpenCORE and VisualOn.
- `res_pjsip_amr.so`: SDP validation and AMR offer/answer compatibility.

The modules are C. Python is used only for build checks, tests, and packaging.
Your Jio credentials, transport/TLS settings, registration, and dialplan remain in
Asterisk's configuration; the addon does not provision a trunk or obtain credentials.

A small [Asterisk registration patch](patches/asterisk-22.11-stale-registration.patch)
also handles replacement Digest challenges (`401`/`407` with `stale=true`). The Nix
package and reference Docker build apply it automatically. Installing only the
three addon modules on an unpatched core does **not** add this registration fix.

## Build and install

Install a C compiler, GNU Make, Python 3, pkg-config, and the development libraries
`opencore-amrnb`, `opencore-amrwb`, and `vo-amrwbenc`. On Debian/Ubuntu, install
`libopencore-amrnb-dev libopencore-amrwb-dev libvo-amrwbenc-dev`.

For a manual build, apply the registration patch to Asterisk 22.11.0 before building
and installing that core:

```sh
patch -d /path/to/asterisk-22.11.0 -p1 < patches/asterisk-22.11-stale-registration.patch
```

Build Asterisk with `--with-pjproject-bundled`. Then use the **configured source tree
from that exact build**, including its bundled PJPROJECT, to build the addon:

```sh
make ASTERISK_SRC=/path/to/asterisk-22.11.0
sudo make ASTERISK_SRC=/path/to/asterisk-22.11.0 install \
  MODULE_DIR=/usr/lib/asterisk/modules
```

Set `MODULE_DIR` to your installation's module directory. `DESTDIR` is supported.
Generic system PJSIP headers can produce incompatible modules even when their
version matches; use the matching bundled SDK.

## Configure

Copy [examples/pjsip_jio.conf](examples/pjsip_jio.conf) to Asterisk's configuration
directory and replace the example identity and IMS domain with your existing values.
`realm`, `username`, `digest_uri`, and `access_network` are required. Set `instance_id`
to your stable instance UUID to enable Contact capability parameters.

In `modules.conf`, require all three modules so initialization failures stop startup:

```ini
[modules]
autoload = yes
require = codec_amr.so
require = res_pjsip_amr.so
require = res_pjsip_jio.so
```

On your Jio endpoint in `pjsip.conf`:

```ini
disallow = all
allow = amr,amrwb,ulaw,alaw
```

Keep your existing transport, TLS verification, authentication and registration
settings. This addon does not change them. The Jio module adds `P-Access-Network-Info`
to outbound SIP requests; Contact features apply to Contacts matching the configured
line identity. It preserves existing Digest responses and Contact parameters.

Restart Asterisk, then check `module show like amr`, `module show like jio`,
`pjsip show registrations`, and `core show translation`. Softphones can keep using
G.711; Asterisk transcodes. AMR modules remain loaded until shutdown, so upgrades
require a restart. Replacing the patched registration module also requires a restart
for this upgrade; a configuration reload does not load the newly installed code.

## Registration recovery

If a registrar rejects an authenticated REGISTER with `stale=true` and a different,
nonempty nonce for the same Digest realm, the patched core answers the replacement
challenge. It allows at most **two additional authenticated requests per registration
round**, including automatic renewals. Ordinary authentication rejection, unchanged
nonces, unrelated realms, and `Retry-After` retain their existing handling. Credentials
are still checked by the registrar; this does not bypass authentication.

Scheduled recovery after a failed round remains a separate `pjsip.conf` policy:

```ini
; Add to your existing outbound registration section.
auth_rejection_permanent=no
retry_interval=300
forbidden_retry_interval=300
max_retries=10
```

This example allows ten delayed retries, five minutes apart, before giving up.
`max_retries=0` disables delayed retries; it does not mean unlimited. The patch does
not change these settings or guarantee that a provider will accept registration.

## NixOS

Add the input to your flake:

```nix
inputs.asterisk-jio.url = "github:yash-garg/asterisk-jio";
```

Then set this in a NixOS module receiving `inputs` and `pkgs`:

```nix
services.asterisk.package = inputs.asterisk-jio.packages.${pkgs.system}.asterisk-with-jio;
```

Keep the input's own pinned nixpkgs so the core and modules use the tested SDK.
The `asterisk-with-jio` output contains the patched Asterisk plus all three modules;
the default `asterisk-jio` package contains the three modules and patch source, but
does not replace an independently installed Asterisk. Keep the configuration above.

```sh
nix build .#asterisk-with-jio
nix flake check
```

## Tests and releases

```sh
make ASTERISK_SRC=/path/to/asterisk-22.11.0 ASTERISK_PREFIX=/path/to/install test
docker build -f ci/Dockerfile -t asterisk-jio-test .
```

Tests use a separate localhost Asterisk and a dummy local registrar. They verify Jio
Digest challenge handling, stale nonces during initial registration and automatic
renewal, retry limits, and Contact serialization alongside 34 AMR audio
mode/alignment combinations, real SIP/RTP, invalid SDP, renegotiation, and module
failure handling. Payload parsing runs under sanitizers. No test contacts Jio or
reads your trunk credentials.

AMR supports mono audio, both RTP alignments, and multiple received frames per packet.
It sends one 20 ms frame at the lowest negotiated mode. Adaptive bitrate, DTX encoding,
CRC, interleaving, and robust sorting are not implemented. Carrier routing behavior
still requires live call validation; passing these tests does not prove an MNP fix.

CI builds natively on x86_64 and ARM64. Version tags create draft releases containing
all three modules, the core patch, source, checksums, and SDK metadata. Release versions come from
the Git tag; branch builds use the commit ID. Binary archives target the
patched Ubuntu 24.04 reference build in `ci/Dockerfile`. They do not contain a rebuilt
Asterisk core: manual users must apply the included patch and rebuild their core.
Other Asterisk builds need addon modules rebuilt against their own SDK. Runtime codec
libraries are not bundled. Local x86_64 results
do not validate ARM64; the corresponding CI job must pass.

AMR is based on [traud/asterisk-amr](https://github.com/traud/asterisk-amr), with bounds
checks, negotiation fixes, and standalone registration. GPL-2.0-only; see
[LICENSE](LICENSE).
