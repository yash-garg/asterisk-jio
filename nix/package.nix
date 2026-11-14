{
  lib,
  stdenv,
  pkg-config,
  python3,
  opencore-amr,
  vo-amrwbenc,
  asterisk,
  version ? "dev",
}:
stdenv.mkDerivation {
  pname = "asterisk-jio";
  inherit version;
  ADDON_VERSION = version;
  src = lib.cleanSourceWith {
    src = ../.;
    filter =
      path: type:
      !(builtins.elem (baseNameOf path) [
        ".git"
        "build"
        "dist"
        "__pycache__"
        ".superpowers"
      ])
      && !(lib.hasPrefix "result" (baseNameOf path));
  };
  nativeBuildInputs = [
    pkg-config
    python3
  ];
  buildInputs = [
    opencore-amr
    vo-amrwbenc
  ];
  makeFlags = [
    "ASTERISK_SRC=${asterisk.dev}/share/asterisk"
    "ASTERISK_PREFIX=${asterisk}"
    "MODULE_DIR=$(out)/lib/asterisk/modules"
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    # LeakSanitizer cannot inspect processes in some Nix sandbox configurations.
    ASAN_OPTIONS=detect_leaks=0 make \
      ASTERISK_SRC=${asterisk.dev}/share/asterisk \
      ASTERISK_PREFIX=${asterisk} test
    runHook postCheck
  '';
  postInstall = ''
    mkdir -p $out/share/asterisk-jio
    cp build/build-info.json README.md LICENSE $out/share/asterisk-jio/
    cp -r examples patches $out/share/asterisk-jio/
  '';
  passthru = { inherit asterisk; };
  meta = {
    description = "Jio IMS compatibility and AMR/AMR-WB modules for Asterisk";
    license = lib.licenses.gpl2Only;
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
