#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/serial-cp210x/serial-cp210x"

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

# cp210x built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/cp210x" ]]; then
    if modinfo cp210x >/dev/null 2>&1; then
        if modprobe cp210x; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load cp210x${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: cp210x module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

KERNEL_VERSION=$(uname -r | cut -d'.' -f1-2)

# Function to compare kernel versions
version_lt() {
    [ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" = "$1" ] && [ "$1" != "$2" ]
}

# Run the test and save the output

# Check if kernel version is less than 5.11
if version_lt "$KERNEL_VERSION" "5.11"; then
    # Legacy kernel (< 5.11)
    "$executable" "--legacy-line-ctl" &> result
else
    # Modern kernel (>= 5.11)
    "$executable" &> result
fi

popd >/dev/null
