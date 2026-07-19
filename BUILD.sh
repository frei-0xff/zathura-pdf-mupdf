#!/bin/bash

cd "$(dirname "${BASH_SOURCE[0]}")" || exit

[ -d build ] && {
    read -r -p "Remove old build directory? (y/N) " answer
    [[ $answer != [Yy]* ]] && { echo "Aborting." >&2; exit 1; }
    rm -rf build
}

meson build
cd build
ninja
sudo ninja install
