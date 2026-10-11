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

      homeModules.default =
        {
          config,
          lib,
          pkgs,
          ...
        }:
        let
          cfg = config.programs.lm10;
          # Quote every string so leading/trailing spaces survive; lm10 only
          # strips the outer pair, so quotes inside need no escaping.
          formatValue =
            v:
            if builtins.isBool v then
              (if v then "yes" else "no")
            else if builtins.isInt v then
              toString v
            else
              "\"${v}\"";
          iniFormat = lib.generators.toINI {
            mkKeyValue = k: v: "${k} = ${formatValue v}";
          };
          # lm10 reads one key = value per line, split at the first '='.
          lineStr = lib.types.strMatching "[^\n]*";
          nonEmpty = lib.filterAttrs (_: s: s != { }) cfg.settings;
          badKeys = lib.concatMap (
            section:
            map (k: "[${section}] ${k}") (
              lib.filter (k: lib.hasInfix "=" k || lib.hasInfix "\n" k) (lib.attrNames cfg.settings.${section})
            )
          ) (lib.attrNames cfg.settings);
        in
        {
          options.programs.lm10 = {
            enable = lib.mkEnableOption "lm10, a picker for wlr-layer-shell Wayland compositors";

            package = lib.mkOption {
              type = lib.types.package;
              default = pkgs.callPackage ./package.nix { };
              defaultText = lib.literalMD "lm10 built against the configuration's `pkgs`";
              description = "The lm10 package to install.";
            };

            settings = lib.mkOption {
              type = lib.types.submodule {
                options =
                  lib.genAttrs
                    [
                      "main"
                      "colors"
                    ]
                    (
                      section:
                      lib.mkOption {
                        type =
                          with lib.types;
                          attrsOf (oneOf [
                            lineStr
                            int
                            bool
                          ]);
                        default = { };
                        description = "Keys of the [${section}] section.";
                      }
                    )
                  // {
                    keys = lib.mkOption {
                      type = lib.types.attrsOf lineStr;
                      default = { };
                      description = ''
                        Keys of the [keys] section: key = action[+action...].
                        These join the built-in binds; "none" unbinds a key.
                      '';
                    };
                  };
              };
              default = { };
              example = lib.literalExpression ''
                {
                  main = {
                    font = "Sans 13";
                    prompt = "❯ ";
                    width = "40%";
                    match = "fuzzy";
                    show-counter = false;
                  };
                  colors.background = "#141415f2";
                  keys = {
                    "Control+j" = "match-next";
                    Tab = "none";
                  };
                }
              '';
              description = ''
                Configuration written to {file}`$XDG_CONFIG_HOME/lm10/lm10.ini`.
                See `lm10 --print-config` for the available keys. No file is
                written when every section is empty.
              '';
            };
          };

          config = lib.mkIf cfg.enable {
            assertions = [
              {
                assertion = badKeys == [ ];
                message = "programs.lm10.settings: keys can't contain '=' or newlines: ${lib.concatStringsSep ", " badKeys}";
              }
            ];

            home.packages = [ cfg.package ];

            xdg.configFile."lm10/lm10.ini" = lib.mkIf (nonEmpty != { }) {
              text = iniFormat nonEmpty;
            };
          };
        };
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
