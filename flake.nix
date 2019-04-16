{
  name = "dwarffs";

  epoch = 2019;

  description = "A filesystem that fetches DWARF debug info from the Internet on demand";

  requires = [ "nixpkgs" ];

  provides = deps: rec {
    packages.dwarffs =
      with deps.nixpkgs.provides.packages;
      with deps.nixpkgs.provides.builders;
      with deps.nixpkgs.provides.lib;

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

    nixosModules.dwarffs = import ./module.nix deps;

    defaultPackage = packages.dwarffs;
  };
}
