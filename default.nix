{ pkgs ? import <nixpkgs> {} }:

with pkgs;

let nix = pkgs.nixStable2 or pkgs.nix; in

stdenv.mkDerivation {
  name = "dwarffs-0.1";

  buildInputs = [ fuse nix nlohmann_json ];

  NIX_CFLAGS_COMPILE = "-I ${nix.dev}/include/nix -include ${nix.dev}/include/nix/config.h -D_FILE_OFFSET_BITS=64";

  src = lib.cleanSource ./.;

  installPhase =
    ''
      mkdir -p $out/bin $out/lib/systemd/system

      cp dwarffs $out/bin/
      ln -s dwarffs $out/bin/mount.fuse.dwarffs

      cp ${./run-dwarffs.mount} $out/lib/systemd/system/run-dwarffs.mount
      cp ${./run-dwarffs.automount} $out/lib/systemd/system/run-dwarffs.automount
    '';
}
