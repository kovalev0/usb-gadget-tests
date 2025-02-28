#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/storage-bot/storage-bot"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

# usb-storage built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/usb-storage" ]]; then
    # Check if the usb-storage module is available for loading
    if modinfo usb-storage >/dev/null 2>&1; then
        if modprobe usb-storage; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load usb-storage${NC}"
        fi
    else
            echo -e "${YELLOW}Warning: usb-storage module is not available (not built-in or loadable).${NC}"
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
