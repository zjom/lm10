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
  mesonBuildType = "release";
  doCheck = true;

  depsBuildBuild = [ pkg-config ];
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
