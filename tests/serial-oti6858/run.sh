#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/serial-oti6858/serial-oti6858"

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

# oti6858 built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/oti6858" ]]; then
    if modinfo oti6858 >/dev/null 2>&1; then
        if modprobe oti6858; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load oti6858${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: oti6858 module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
