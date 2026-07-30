{
  description = "text2sprech: local TTS reader (Qt6 GUI + Piper server)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Default voice (en_US-lessac-medium) from rhasspy/piper-voices v1.0.0.
    # flake = false → plain file inputs; hashes land in flake.lock on
    # `nix flake lock` / first build, so no manual fetchurl hashes here.
    piper-voice-onnx = {
      url = "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx";
      flake = false;
    };
    piper-voice-json = {
      url = "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, piper-voice-onnx, piper-voice-json }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        inherit (pkgs) lib;

        # Single source of truth: top-level VERSION (e.g. 0.1.0-dev). Nix
        # builds append the short git rev when available.
        versionBase = lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);
        gitRev = self.shortRev or self.dirtyShortRev or "dirty";
        version = "${versionBase}+g${gitRev}";

        piperVoiceName = "en_US-lessac-medium";

        # Install the two flake-input files under share/piper/voices/ so the
        # server can find ${name}.onnx + ${name}.onnx.json side by side.
        piper-voice-en_US-lessac-medium = pkgs.stdenv.mkDerivation {
          pname = "piper-voice-en_US-lessac-medium";
          version = "1.0.0";
          dontUnpack = true;
          installPhase = ''
            mkdir -p $out/share/piper/voices
            cp ${piper-voice-onnx} $out/share/piper/voices/${piperVoiceName}.onnx
            cp ${piper-voice-json} $out/share/piper/voices/${piperVoiceName}.onnx.json
          '';
          meta = with lib; {
            description = "Piper TTS voice model: en_US-lessac-medium";
            license = licenses.gpl3Plus;
            platforms = platforms.all;
          };
        };

        # --- Python side: piper-server -----------------------------------
        #
        # The `piper` TTS Python package is not reliably in nixpkgs; real
        # audio still needs a `piper` CLI on PATH (PiperCliBackend) or a
        # vendored Python package. Voice *models* are the flake inputs above.
        pythonEnv = pkgs.python3.withPackages (ps: [ ]);

        mkPiperServer = { withDefaultVoice ? false }:
          pkgs.stdenv.mkDerivation {
            pname = if withDefaultVoice then "piper-server-full" else "piper-server";
            inherit version;
            src = ./piper_server;
            dontBuild = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            installPhase = ''
              mkdir -p $out/share/piper-server $out/bin
              cp -r . $out/share/piper-server
              makeWrapper ${pythonEnv}/bin/python3 $out/bin/piper-server \
                --add-flags "$out/share/piper-server/server.py" \
                ${lib.optionalString withDefaultVoice ''
                  --set TEXT2SPRECH_PIPER_MODELS "${piper-voice-en_US-lessac-medium}/share/piper/voices" \
                  --add-flags "--default-voice ${piperVoiceName}"
                ''}
            '';
            meta = with lib; {
              description = "Unix-socket Piper TTS server for text2sprech"
                + (if withDefaultVoice then " (bundled en_US-lessac-medium voice)" else "");
              license = licenses.gpl3Plus;
              platforms = platforms.linux ++ platforms.darwin;
            };
          };

        piper-server = mkPiperServer { withDefaultVoice = false; };
        piper-server-full = mkPiperServer { withDefaultVoice = true; };

        # --- C++ side: the Qt6 app -----------------------------------------
        mkText2sprech = { withDefaultVoice ? false }:
          let
            server = if withDefaultVoice then piper-server-full else piper-server;
          in
          pkgs.stdenv.mkDerivation {
            pname = if withDefaultVoice then "text2sprech-full" else "text2sprech";
            inherit version;
            src = ./.;
            nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.qt6.wrapQtAppsHook ];
            buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtmultimedia ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DPROJECT_VERSION_FULL=${version}"
            ];
            # cmake install phase already places bin/, share/applications/,
            # share/icons/hicolor/, and share/metainfo/. Assert they landed so
            # a missing install() rule fails the build instead of a silent
            # package without a menu entry.
            postInstall = ''
              test -x "$out/bin/text2sprech"
              test -f "$out/share/applications/text2sprech.desktop"
              test -f "$out/share/applications/text2sprech-clipboard.desktop"
              test -f "$out/share/metainfo/text2sprech.metainfo.xml"
              test -f "$out/share/icons/hicolor/48x48/apps/text2sprech.png"
              test -f "$out/share/icons/hicolor/512x512/apps/text2sprech.png"
              test -f "$out/share/text2sprech/piper_server/server.py"
            '';
            qtWrapperArgs = [
              "--prefix" "PATH" ":" "${server}/bin"
            ] ++ lib.optionals withDefaultVoice [
              "--set" "TEXT2SPRECH_PIPER_MODELS" "${piper-voice-en_US-lessac-medium}/share/piper/voices"
            ];
            meta = with lib; {
              description = "Local TTS reader with live sentence highlighting"
                + (if withDefaultVoice then " (bundled en_US-lessac-medium voice)" else "");
              license = licenses.gpl3Plus;
              platforms = platforms.linux ++ platforms.darwin;
              mainProgram = "text2sprech";
            };
          };

        # Lean: no bundled voice (use ~/.local/share/piper/voices or env).
        text2sprech = mkText2sprech { withDefaultVoice = false; };
        # Full: ships en_US-lessac-medium and points the server at it.
        text2sprech-full = mkText2sprech { withDefaultVoice = true; };
      in
      {
        packages = {
          inherit text2sprech text2sprech-full;
          inherit piper-server piper-server-full;
          inherit piper-voice-en_US-lessac-medium;
          default = text2sprech-full;
        };

        apps = {
          text2sprech = flake-utils.lib.mkApp { drv = text2sprech-full; exePath = "/bin/text2sprech"; };
          text2sprech-lean = flake-utils.lib.mkApp { drv = text2sprech; exePath = "/bin/text2sprech"; };
          piper-server = flake-utils.lib.mkApp { drv = piper-server; exePath = "/bin/piper-server"; };
          piper-server-full = flake-utils.lib.mkApp { drv = piper-server-full; exePath = "/bin/piper-server"; };
          default = self.apps.${system}.text2sprech;
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.qt6.qtbase
            pkgs.qt6.qtmultimedia
            pkgs.qt6.qttools
            pythonEnv
            piper-server-full
            piper-voice-en_US-lessac-medium
          ];
          shellHook = ''
            export TEXT2SPRECH_PIPER_MODELS="${piper-voice-en_US-lessac-medium}/share/piper/voices"
            echo "text2sprech dev shell: cmake -B build -G Ninja && cmake --build build && ./build/text2sprech"
            echo "Bundled voice models: $TEXT2SPRECH_PIPER_MODELS"
            echo "Packages: nix build .#text2sprech-full  (default)  |  nix build .#text2sprech  (no model)"
          '';
        };
      });
}
