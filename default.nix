with import <nixpkgs> {};

stdenv.mkDerivation {
  name = "dwarffs-0.1";

  buildInputs = [ fuse nixUnstable ];

  NIX_CFLAGS_COMPILE = "-I ${nixUnstable.dev}/include/nix -include ${nixUnstable.dev}/include/nix/config.h -D_FILE_OFFSET_BITS=64";
}
