# Asterisk Jio

Jio IMS compatibility and AMR/AMR-WB support for **Asterisk 22.11.0 on Linux**,
without patching its core. One addon builds and installs all three modules:

- `res_pjsip_jio.so`: initial empty Digest authorization, Jio's `483` authentication
  challenge handling, IMS access-network headers, and Contact capabilities.
- `codec_amr.so`: AMR/AMR-WB registration and transcoding with OpenCORE and VisualOn.
- `res_pjsip_amr.so`: SDP validation and AMR offer/answer compatibility.

The modules are C. Python is used only for build checks, tests, and packaging.
Your Jio credentials, transport/TLS settings, registration, and dialplan remain in
Asterisk's configuration; the addon does not provision a trunk or obtain credentials.

## Build and install

Install a C compiler, GNU Make, Python 3, pkg-config, and the development libraries
`opencore-amrnb`, `opencore-amrwb`, and `vo-amrwbenc`. On Debian/Ubuntu, install
`libopencore-amrnb-dev libopencore-amrwb-dev libvo-amrwbenc-dev`.

Use the **configured source tree from the Asterisk build you run**, including its
bundled PJPROJECT. Build Asterisk with `--with-pjproject-bundled` first, then:

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
require a restart. Use a stock Asterisk core.

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
The `asterisk-with-jio` output contains Asterisk plus all three modules; the default
`asterisk-jio` package contains the three modules alone. Keep the configuration above.

```sh
nix build .#asterisk-with-jio
nix flake check
```

## Tests

```sh
make ASTERISK_SRC=/path/to/asterisk-22.11.0 ASTERISK_PREFIX=/path/to/install test
```

Tests use a separate localhost Asterisk and a dummy local registrar. They verify Jio
Digest challenge handling and Contact serialization alongside 34 AMR audio
mode/alignment combinations, real SIP/RTP, invalid SDP, renegotiation, and module
failure handling. Payload parsing runs under sanitizers. No test contacts Jio or
reads your trunk credentials.

AMR supports mono audio, both RTP alignments, and multiple received frames per packet.
It sends one 20 ms frame at the lowest negotiated mode. Adaptive bitrate, DTX encoding,
CRC, interleaving, and robust sorting are not implemented. Carrier routing behavior
still requires live call validation; passing these tests does not prove an MNP fix.

AMR is based on [traud/asterisk-amr](https://github.com/traud/asterisk-amr), with bounds
checks, negotiation fixes, and standalone registration. GPL-2.0-only; see
[LICENSE](LICENSE).
