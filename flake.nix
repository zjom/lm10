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
      # The version lives in meson.build.
      version = builtins.head (
        builtins.match ".*project\\([^)]*[^_]version: '([^']+)'.*" (builtins.readFile ./meson.build)
      );
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "lm10";
          inherit version;
          src = ./.;

          # Tools that run at build time
          doCheck = true;
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
            description = "A picker for wlr-layer-shell Wayland compositors";
            license = pkgs.lib.licenses.mit;
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "lm10";
          };
        };
      });

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
