{
  description = "Local TTS reader (Qt6 GUI + Piper server)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # --- Python side: piper-server -----------------------------------
        #
        # IMPORTANT / KNOWN GAP: the `piper` TTS Python package is not
        # reliably packaged in nixpkgs as of writing, and this sandbox has
        # no network access to resolve a real fetchPypi/fetchurl hash for
        # you right now. Two honest options, pick one before relying on
        # real (non-silent-test) audio:
        #
        #   1. Quick path: let piper_server/server.py fall back to the
        #      `piper` CLI binary (PiperCliBackend in synth.py). Install
        #      the `piper-tts` CLI however you'd normally get it (pipx,
        #      a release binary, etc.) outside this flake, and just make
        #      sure it's on PATH inside `nix develop` via an impure shell
        #      hook, or add it as a flake input once you have a working
        #      fetchurl hash.
        #
        #   2. Proper path: vendor it as a Nix package once you can reach
        #      the network -- e.g. `nix-prefetch-url` the wheel and add a
        #      `python3Packages.buildPythonPackage` derivation here. Left
        #      as a TODO rather than a fabricated hash that would silently
        #      fail your first `nix build`.
        pythonEnv = pkgs.python3.withPackages (ps: [ pkgs.piper ]);
        # ^ intentionally empty: piper_server only uses the stdlib
        #   (asyncio, wave, json, struct). Add ps.<pkg> here once you
        #   vendor a real piper Python package (option 2 above).

        piper-server = pkgs.stdenv.mkDerivation {
          pname = "piper-server";
          version = "0.1.0";
          src = ./piper_server;
          dontBuild = true;
          nativeBuildInputs = [ pkgs.makeWrapper ];
          installPhase = ''
            mkdir -p $out/share/piper-server $out/bin
            cp -r . $out/share/piper-server
            makeWrapper ${pythonEnv}/bin/python3 $out/bin/piper-server \
              --add-flags "$out/share/piper-server/server.py"
          '';
        };

        # --- C++ side: the Qt6 reader -------------------------------------
        reader = pkgs.stdenv.mkDerivation {
          pname = "reader";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.qt6.wrapQtAppsHook ];
          buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtmultimedia ];
          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];
        };
      in
      {
        packages = {
          inherit reader piper-server;
          default = reader;
        };

        apps = {
          reader = flake-utils.lib.mkApp { drv = reader; exePath = "/bin/reader"; };
          piper-server = flake-utils.lib.mkApp { drv = piper-server; exePath = "/bin/piper-server"; };
          default = self.apps.${system}.reader;
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.qt6.qtbase
            pkgs.qt6.qtmultimedia
            pkgs.qt6.qttools # for Qt Creator / IDE integration, debugging tools
            pythonEnv
          ];
          shellHook = ''
            echo "reader dev shell: cmake -B build -G Ninja && cmake --build build"
            echo "piper-server:     python3 piper_server/server.py --verbose"
            echo "(see PROTOCOL.md; note the piper-python-package gap noted in flake.nix)"
          '';
        };
      });
}
