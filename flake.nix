{
  description = "A filesystem that fetches DWARF debug info from the Internet on demand";

  inputs.nixpkgs.follows = "nix/nixpkgs";

  outputs = { self, nix, nixpkgs }:

    let
      supportedSystems = [ "x86_64-linux" "i686-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs supportedSystems (system: f system);
      version = "0.1.${nixpkgs.lib.substring 0 8 self.lastModifiedDate}.${self.shortRev or "dirty"}";
    in

    {

      overlay = final: prev: {

        dwarffs = with final; let nix = final.nix; in stdenv.mkDerivation {
          name = "dwarffs-${version}";

          buildInputs = [ fuse nix nlohmann_json boost ];

          NIX_CFLAGS_COMPILE = "-I ${nix.dev}/include/nix -include ${nix.dev}/include/nix/config.h -D_FILE_OFFSET_BITS=64 -DVERSION=\"${version}\"";

          src = self;

          installPhase =
            ''
              mkdir -p $out/bin $out/lib/systemd/system

              cp dwarffs $out/bin/
              ln -s dwarffs $out/bin/mount.fuse.dwarffs

              cp ${./run-dwarffs.mount} $out/lib/systemd/system/run-dwarffs.mount
              cp ${./run-dwarffs.automount} $out/lib/systemd/system/run-dwarffs.automount
            '';
        };

      };

      defaultPackage = forAllSystems (system: (import nixpkgs {
        inherit system;
        overlays = [ self.overlay nix.overlays.default ];
      }).dwarffs);

      checks = forAllSystems (system: {
        build = self.defaultPackage.${system};

        test =
          with import (nixpkgs + "/nixos/lib/testing-python.nix") {
            inherit system;
          };

          makeTest {
            nodes = {
              client = { ... }: {
                imports = [ self.nixosModules.dwarffs ];
                nixpkgs.overlays = [ nix.overlay ];
              };
            };

            testScript =
              ''
                start_all()
                client.wait_for_unit("multi-user.target")
                client.succeed("dwarffs --version")
                client.succeed("cat /run/dwarffs/README")
                client.succeed("[ -e /run/dwarffs/.build-id/00 ]")
              '';
          };
      });

      nixosModules.dwarffs =
        { pkgs, ... }:
        {
          nixpkgs.overlays = [ self.overlay ];

          systemd.packages = [ pkgs.dwarffs ];

          system.fsPackages = [ pkgs.dwarffs ];

          systemd.units."run-dwarffs.automount".wantedBy = [ "multi-user.target" ];

          environment.variables.NIX_DEBUG_INFO_DIRS = [ "/run/dwarffs" ];

          systemd.tmpfiles.rules = [ "d /var/cache/dwarffs 0755 dwarffs dwarffs 7d" ];

          users.users.dwarffs =
            { description = "Debug symbols file system daemon user";
              group = "dwarffs";
              isSystemUser = true;
            };

          users.groups.dwarffs = {};
        };

    };
}
