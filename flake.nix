{
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
        CviBurn = pkgs.stdenv.mkDerivation {
          pname = "CviBurn";
          version = "0-unstable";
          src = self;

          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.llvmPackages.llvm
          ];

          buildInputs = [
            pkgs.expat
          ];

          makeFlags = [
            "PREFIX=${placeholder "out"}"
            "MAGIC_PATH=${placeholder "out"}/share/CviBurn/cv_dl_magic.bin"
            "AARCH64_CC=${pkgs.llvmPackages.clang-unwrapped}/bin/clang"
            "OBJCOPY=${pkgs.llvmPackages.llvm}/bin/llvm-objcopy"
          ];

          doCheck = true;
          checkTarget = "check-magic";

          installPhase = ''
            runHook preInstall
            install -Dm755 usb_dl $out/bin/CviBurn
            install -Dm644 src/cv_dl_magic.bin $out/share/CviBurn/cv_dl_magic.bin
            runHook postInstall
          '';

          doInstallCheck = true;
          installCheckPhase = ''
            runHook preInstallCheck
            $out/bin/CviBurn -V
            test -f $out/share/CviBurn/cv_dl_magic.bin
            test "$(stat -c %s $out/share/CviBurn/cv_dl_magic.bin)" = 128
            runHook postInstallCheck
          '';

          meta = {
            description = "Milk-V Duo EMMC downloader for CVI/CV180x/CV181x SoCs";
            mainProgram = "CviBurn";
            platforms = pkgs.lib.platforms.linux;
            licenses = [ pkgs.lib.licenses.unfree ];
          };
        };

        usb_dl = pkgs.stdenv.mkDerivation rec {
          pname = "usb_dl";
          version = "0-unstable";

          src = pkgs.fetchurl {
            url = "https://raw.githubusercontent.com/milkv-duo/duo-buildroot-sdk-v2/refs/tags/v2.0.1/build/tools/common/usb_dl/Linux/usb_dl";
            hash = "sha256-JFPiNzcVnDwsbq6PCpuUZlrurPgAtH9br0q2SIlqNis=";
          };

          nativeBuildInputs = [ pkgs.autoPatchelfHook ];

          buildInputs = with pkgs; [
            stdenv.cc.cc.lib
          ];

          dontUnpack = true;
          dontBuild = true;
          dontConfigure = true;

          installPhase = ''
            mkdir -p $out/bin
            install -Dm755 ${src} $out/bin/usb_dl
          '';

          meta = {
            description = "Milk-V Duo EMMC downloader for CVI/CV180x/CV181x SoCs";
            mainProgram = "usb_dl";
            platforms = [ "x86_64-linux" ];
            licenses = [ pkgs.lib.licenses.unfree ];
          };
        };
      });
    };
}
