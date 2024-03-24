#!/bin/bash -e

set -o pipefail

readonly PARAM_SYSTEM="$1"
readonly PARAM_EMULATOR="$2"
readonly PARAM_ROM_PATH="$3"
readonly PARAM_COMMAND="$4"

readonly SPINNER_DEVICE="/dev/tty.SpinnerMouse"  # see: /etc/udev/rules.d/99-spinner-mouse.rules
readonly SPINNER_BAUD="115200"


# Ensure the spinner returns to its default (startup) speed mode...
if [[ "$PARAM_SYSTEM" == "arcade" && -e "$SPINNER_DEVICE" ]]; then
    sudo stty -F "$SPINNER_DEVICE" "$SPINNER_BAUD"
    echo -n 'r' | sudo tee "$SPINNER_DEVICE" >/dev/null
fi
