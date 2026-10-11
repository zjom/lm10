{
  description = "Minimal Wayland client in C";

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
        default = pkgs.stdenv.mkDerivation {
          pname = "lm10";
          version = "0.1.0";
          src = ./.;

          # Tools that run at build time
          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];
          # Libraries / data linked against or read during the build
          buildInputs = with pkgs; [
            wayland
            wayland-protocols
            pango
            cairo
            librsvg
            libxkbcommon
          ];

          meta = {
            description = "Minimal xdg-shell Wayland client";
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "lm10";
          };
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          hardeningDisable = [ "fortify" ];
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          packages = with pkgs; [
            gdb
            valgrind
            clang-tools
          ];
        };
      });
    };
}
