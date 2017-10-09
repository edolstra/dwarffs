{ config, lib, pkgs, ... }:

let

  dwarffs = import ./. { inherit pkgs; };

in

{

  systemd.packages = [ dwarffs ];

  system.fsPackages = [ dwarffs ];

  systemd.units."run-dwarffs.automount".wantedBy = [ "multi-user.target" ];

  environment.variables.NIX_DEBUG_INFO_DIRS = [ "/run/dwarffs" ];

  systemd.tmpfiles.rules = [ "d /var/cache/dwarffs 0755 root root 7d" ];

}
