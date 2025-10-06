#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/input-tab-acecad-Flair/input-tab-acecad-Flair"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

# acecad built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/acecad" ]]; then
    if modinfo acecad >/dev/null 2>&1; then
        if modprobe acecad; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load acecad${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: acecad module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
