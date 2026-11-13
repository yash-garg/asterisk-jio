# Export the exact configured headers alongside an otherwise unpatched Asterisk.
{ asterisk }:
asterisk.overrideAttrs (old: {
  enableParallelBuilding = true;
  outputs = [
    "out"
    "dev"
  ];
  postInstall = (old.postInstall or "") + ''
    sdk=$dev/share/asterisk
    mkdir -p "$sdk"
    cp -r include "$sdk/"
    cp .version "$sdk/"
    for flag in $(make --no-print-directory -s -C third-party/pjproject echo_cflags); do
      case "$flag" in
        -D*|-U*) printf '%s\n' "$flag" ;;
      esac
    done > "$sdk/pjproject-compiler-flags"
    for dir in pjlib pjlib-util pjnath pjmedia pjsip; do
      mkdir -p "$sdk/third-party/pjproject/source/$dir"
      cp -r third-party/pjproject/source/$dir/include "$sdk/third-party/pjproject/source/$dir/"
    done
  '';
})
