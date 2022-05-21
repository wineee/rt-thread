{ pkgs ? import <nixpkgs> {} }:
let
  my-python = pkgs.python3;
  python-with-my-packages = my-python.withPackages (p: with p; [
    tkinter
  ]);
in
with pkgs; mkShell {
  buildInputs = [
    scons
    gcc
    ncurses5
    python-with-my-packages
  ];

  shellHook = ''
    RTT_ROOT=/data/rv/rt-thread
    BSP_ROOT=/data/rv/rt-thread/bsp
    RTT_CC=gcc
    RTT_EXEC_PATH=/opt/riscv/bin
    PYTHONPATH=${python-with-my-packages}/${python-with-my-packages.sitePackages}
    # maybe set more env-vars
  '';
}
