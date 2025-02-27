#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/ethernet/ethernet"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

# Built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/rtl8150" ]]; then
    # Check if the rtl8150 module is available for loading
    if modinfo rtl8150 >/dev/null 2>&1; then
        if modprobe rtl8150; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load rtl8150${NC}"
        fi
    else
            echo -e "${YELLOW}Warning: rtl8150 module is not available (not built-in or loadable).${NC}"
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
