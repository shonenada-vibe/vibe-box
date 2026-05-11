#!/bin/bash

export PYENV_ROOT="$HOME/.pyenv"
eval "$(pyenv init -)"
pyenv shell 3.13
source ~/w/esp/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0

cd firmware
idf.py flash
# idf.py -p /dev/cu.usbmodem421201 monitor
