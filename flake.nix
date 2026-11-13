{
  description = "axiomeOS development environment";

  inputs = {
    # Linux: keep on unstable for fresh packages + binary cache
    nixpkgs-linux.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Darwin: 26.05 is the last branch supporting Intel Macs (x86_64-darwin).
    # nixos-unstable (26.11) has dropped x86_64-darwin entirely.
    nixpkgs-darwin.url = "github:NixOS/nixpkgs/nixpkgs-26.05-darwin";
  };

  outputs = { self, nixpkgs-linux, nixpkgs-darwin }:
    let
      lib = nixpkgs-linux.lib;

      systems = [ "x86_64-linux" "x86_64-darwin" ];

      isDarwinSystem = system: lib.hasSuffix "-darwin" system;

      nixpkgsFor = system:
        if isDarwinSystem system then nixpkgs-darwin else nixpkgs-linux;

      forAllSystems = f: lib.genAttrs systems f;
    in
    {
      devShells = forAllSystems (system:
        let
          isDarwin = isDarwinSystem system;

          pkgs = import (nixpkgsFor system) {
            inherit system;
            config = {
              # gnu-efi is marked linux-only, but we build it for x86_64-elf
              allowUnsupportedSystem = true;
            } // lib.optionalAttrs isDarwin {
              # 26.05 still supports Intel Macs but warns about it
              allowDeprecatedx86_64Darwin = true;
            };
          };

          # Cross-compiler that targets x86_64-embedded (ELF)
          cross = pkgs.pkgsCross.x86_64-embedded;

          # The x86_64-elf binutils are configured without PE support, but
          # converting the bootloader from ELF to a PE32+ EFI image needs an
          # objcopy that understands `--target=efi-app-x86_64`. The mingw
          # binutils provide exactly that (pei-x86-64).
          mingwBinutils = pkgs.pkgsCross.mingwW64.stdenv.cc.bintools;

          # Linux: nixpkgs' cross-compiled gnu-efi works as-is.
          #
          # Darwin: nixpkgs' gnu-efi is built with the *cross* stdenv, which
          # drags in a target-side pciutils and platform checks that don't
          # work from a Mac. gnu-efi is just a static lib + headers + crt0 +
          # linker script, so build it with the host stdenv and only borrow
          # the x86_64-elf gcc/binutils from the cross set.
          gnuEfi =
            if isDarwin then
              pkgs.stdenv.mkDerivation {
                pname = "gnu-efi-x86_64-elf";
                inherit (cross.gnu-efi) version src;

                nativeBuildInputs = [
                  cross.stdenv.cc
                  cross.stdenv.cc.bintools
                ];

                # Freestanding code: no stack protector, fortify, etc.
                hardeningDisable = [ "all" ];

                # Output is ELF objects; host (Mach-O) strip can't handle them
                dontStrip = true;
                dontFixup = true;

                makeFlags = [
                  "PREFIX=${placeholder "out"}"
                  "HOSTCC=cc"
                  "CROSS_COMPILE=${cross.stdenv.cc.targetPrefix}"
                ];

                postPatch = ''
                  substituteInPlace Make.defaults \
                    --replace-quiet "-Werror" ""

                  # Only the library, crt0, linker script and headers; skip the
                  # sample apps. Patch the top-level list in place: overriding
                  # SUBDIRS on the command line also clobbers lib/Makefile's own.
                  sed -i '/^SUBDIRS *=/ s/ apps//' Makefile
                  grep -n '^SUBDIRS' Makefile
                  if grep -E '^SUBDIRS.*apps' Makefile; then
                    echo "failed to remove apps from SUBDIRS" >&2
                    exit 1
                  fi
                '';
              }
            else
              cross.gnu-efi;

          # Linux: OVMF from nixpkgs.
          # Darwin: OVMF/edk2 doesn't build there, but QEMU ships a prebuilt
          # edk2 x86_64 firmware image.
          ovmfFd =
            if isDarwin
            then "${pkgs.qemu}/share/qemu/edk2-x86_64-code.fd"
            else "${pkgs.OVMF.fd}/FV/OVMF.fd";
        in
        {
          default = pkgs.mkShell {
            buildInputs = [
              cross.stdenv.cc
              pkgs.qemu
              gnuEfi
              pkgs.mtools
              pkgs.xorriso
              pkgs.python3
              pkgs.gnumake
              pkgs.gdb
              mingwBinutils
            ];

            shellHook = ''
              export OVMF_FD="${ovmfFd}"
              # Point the bootloader build at the nix gnu-efi (headers + libs +
              # crt0 + linker script) instead of the distro /usr layout.
              export EFI_PREFIX="${gnuEfi}"
              # stdenv exports OBJCOPY as the host/LLVM one, which cannot emit
              # PE32+ EFI images; use the mingw binutils objcopy instead.
              export OBJCOPY="${mingwBinutils}/bin/x86_64-w64-mingw32-objcopy"
              echo "axiomeOS dev shell ready (system: ${system})"
            '';
          };
        });
    };
}
