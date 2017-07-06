with import <nixpkgs> {};

stdenv.mkDerivation {
  name = "dwarffs-0.1";

  buildInputs = [ fuse nixUnstable nlohmann_json ];

  NIX_CFLAGS_COMPILE = "-I ${nixUnstable.dev}/include/nix -include ${nixUnstable.dev}/include/nix/config.h -D_FILE_OFFSET_BITS=64";

  src = lib.cleanSource ./.;

  installPhase =
    ''
      mkdir -p $out/bin
      cp dwarffs $out/bin/
    '';
}
