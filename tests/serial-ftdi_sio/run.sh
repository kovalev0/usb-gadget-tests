#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/serial-ftdi_sio/serial-ftdi_sio"

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

# ftdi_sio built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/ftdi_sio" ]]; then
    if modinfo ftdi_sio >/dev/null 2>&1; then
        if modprobe ftdi_sio; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load ftdi_sio${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: ftdi_sio module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
# Check gpiolib support
if [[ -d "/sys/class/gpio" ]]; then
    # CONFIG_GPIOLIB=y
    "$executable" &> result
else
    # CONFIG_GPIOLIB is not set
    "$executable" --no-gpiolib &> result
fi

popd >/dev/null
