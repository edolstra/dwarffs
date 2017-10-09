{ config, lib, pkgs, ... }:

let

  dwarffs = import ./. { inherit pkgs; };

in

{

  systemd.packages = [ dwarffs ];

  system.fsPackages = [ dwarffs ];

  systemd.units."run-dwarffs.automount".wantedBy = [ "multi-user.target" ];

  environment.variables.NIX_DEBUG_INFO_DIRS = [ "/run/dwarffs" ];

}
