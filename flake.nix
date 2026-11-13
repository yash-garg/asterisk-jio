{
  description = "Jio IMS and AMR/AMR-WB addon for Asterisk 22.11";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/4fe25e12eb9f4c744966dd3de5a05246cf098601";
  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      eachSystem = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          asterisk = pkgs.callPackage ./nix/asterisk.nix { };
          addon = pkgs.callPackage ./nix/package.nix {
            inherit asterisk;
            version = self.shortRev or "dirty";
          };
        in
        {
          default = addon;
          asterisk-jio = addon;
          asterisk-with-jio = pkgs.symlinkJoin {
            name = "asterisk-with-jio-${asterisk.version}";
            paths = [
              asterisk
              addon
            ];
            passthru = { inherit asterisk addon; };
          };
        }
      );
      checks = eachSystem (system: {
        addon = self.packages.${system}.default;
      });
    };
}
