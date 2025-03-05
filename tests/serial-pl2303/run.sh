#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/serial-pl2303/serial-pl2303"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

# usbserial built-in or already loaded ?
if [[ ! -d "/sys/bus/usb-serial" ]]; then
    if modinfo usbserial >/dev/null 2>&1; then
        if modprobe usbserial; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load usbserial${NC}"
        fi
    else
            echo -e "${YELLOW}Warning: usbserial module is not available (not built-in or loadable).${NC}"
    fi
fi

# pl2303 built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/pl2303" ]]; then
    if modinfo pl2303 >/dev/null 2>&1; then
        if modprobe pl2303; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load pl2303${NC}"
        fi
    else
            echo -e "${YELLOW}Warning: pl2303 module is not available (not built-in or loadable).${NC}"
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
