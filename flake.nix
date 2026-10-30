{
  description = "grassy-terrain: a minimal Janet terminal emulator on libghostty + Jaylib";

  inputs = {
    # Standard nixpkgs. The dev shell below pins Zig 0.17.0 (required by
    # libghostty) from the zig-overlay input.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Zig is not in nixpkgs at the exact version libghostty requires
    # (0.17.0), so we pull it from the same overlay the Ghostty project uses.
    zig = {
      url = "github:mitchellh/zig-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, zig }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
    in {
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};

          # libghostty-vt is built with `zig build -Demit-lib-vt`.
          zigPkg = zig.packages.${system}."0.17.0";

          # Everything Jaylib needs to compile the bundled raylib + GLFW on
          # Linux (X11 backend, OpenGL, and audio backends).
          nativeDeps = with pkgs; [
            gcc
            gnumake
            cmake
            ninja
            pkg-config
            git
            janet
            jpm
          ];

          graphicsDeps = with pkgs; [
            libGL
            libGLU
            libX11
            libXcursor
            libXrandr
            libXinerama
            libXi
            libXext
            libXxf86vm
            xorgproto
            libxkbcommon
            wayland
            wayland-protocols
            alsa-lib
          ];
        in {
          default = pkgs.mkShell {
            packages = [ zigPkg ] ++ nativeDeps ++ graphicsDeps;

            shellHook = ''
              echo "grassy-terrain dev shell"
              echo "  zig:   $(zig version)"
              echo "  janet: $(janet -v 2>/dev/null | head -1)"
              export GRASSY_ROOT="$PWD"
            '';
          };
        });
    };
}
