#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/serial-ch341/serial-ch341"

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
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: usbserial module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# ch341 built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/ch341" ]]; then
    if modinfo ch341 >/dev/null 2>&1; then
        if modprobe ch341; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load ch341${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: ch341 module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
