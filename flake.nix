{
  description = "lm10, a picker for wlr-layer-shell Wayland compositors";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./package.nix { };
      });

      overlays.default = final: _: { lm10 = final.callPackage ./package.nix { }; };

      checks = forAllSystems (pkgs: {
        inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) default;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);

      homeModules.default = ./home-manager.nix;
      homeManagerModules.default = self.homeModules.default;

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          hardeningDisable = [ "fortify" ];
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = with pkgs; [
            gdb
            valgrind
            clang-tools
          ];
        };
      });
    };
}
