#!/bin/sh
# Builds out/tweb for Linux with g++ (C++17).
#
# Runtime dependencies, all resolved on the user's machine:
#   curl              the CLI, for HTTPS
#   zenity            the dialogs (falls back to the terminal if absent)
#   xdg-open          opens the staged page in the browser
#   libnng.so.1       KiCad's own IPC library, dlopen'ed at run time
#   libz.so.1         linked normally
# So build and install need only g++ and zlib headers:
#   Fedora:  sudo dnf install gcc-c++ zlib-devel
#   Debian:  sudo apt install build-essential zlib1g-dev
# Tested on Fedora with g++ 16.2, KiCad 10.0.6.
set -e
cd "$(dirname "$0")"
mkdir -p out
g++ -O2 -Wall -Wextra -std=c++17 -o out/tweb tweb_linux.cpp string_table_linux.cpp -lz -ldl
echo "built out/tweb"