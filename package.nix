{
  lib,
  stdenv,
  meson,
  gcc,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  glib,
  pango,
  cairo,
  libxkbcommon,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "lm10";
  version = lib.fileContents ./VERSION;

  # Only what the build reads, so editing anything else doesn't rebuild.
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./VERSION
      ./meson.build
      ./src
      ./include
      ./tests
      ./third-party
      ./examples/lm10.ini
      ./README.md
    ];
  };

  strictDeps = true;
  # The meson hook defaults to "plain", which adds no -O flag.
  mesonBuildType = "release";
  doCheck = true;

  # pkg-config for meson's native: true lookup of wayland-scanner
  depsBuildBuild = [ pkg-config ];
  # Tools that run at build time
  nativeBuildInputs = [
    gcc
    meson
    ninja
    pkg-config
    wayland-scanner
  ];
  # Libraries / data linked against or read during the build
  buildInputs = [
    wayland
    wayland-protocols
    glib
    pango
    cairo
    libxkbcommon
  ];

  meta = {
    description = "A picker for wlr-layer-shell Wayland compositors";
    homepage = "https://github.com/zjom/lm10";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "lm10";
  };
})
