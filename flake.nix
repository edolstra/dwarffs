{
  name = "dwarffs";

  description = "A filesystem that fetches DWARF debug info from the Internet on demand";

  requires = [ flake:nixpkgs ];

  provides = flakes: rec {
    packages.dwarffs =
      with flakes.nixpkgs.provides.packages;
      with flakes.nixpkgs.provides.builders;
      with flakes.nixpkgs.provides.lib;

      stdenv.mkDerivation {
        name = "dwarffs-0.1";

        buildInputs = [ fuse nix nlohmann_json boost ];

        NIX_CFLAGS_COMPILE = "-I ${nix.dev}/include/nix -include ${nix.dev}/include/nix/config.h -D_FILE_OFFSET_BITS=64";

        src = cleanSource ./.;

        installPhase =
          ''
            mkdir -p $out/bin $out/lib/systemd/system

            cp dwarffs $out/bin/
            ln -s dwarffs $out/bin/mount.fuse.dwarffs

            cp ${./run-dwarffs.mount} $out/lib/systemd/system/run-dwarffs.mount
            cp ${./run-dwarffs.automount} $out/lib/systemd/system/run-dwarffs.automount
          '';
      };

    nixosModules.dwarffs = import ./module.nix flakes;

    defaultPackage = packages.dwarffs;
  };
}
