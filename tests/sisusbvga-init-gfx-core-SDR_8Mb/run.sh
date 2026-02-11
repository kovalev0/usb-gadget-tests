#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

test_name="sisusbvga-init-gfx-core-SDR_8Mb"

executable="../../src/${test_name}/${test_name}"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

driver_name="sisusbvga"
driver_sys_dir="/sys/bus/usb/drivers/sisusb"
# Driver built-in or already loaded ?
if [[ ! -d "${driver_sys_dir}" ]]; then
    if modinfo ${driver_name} >/dev/null 2>&1; then
        if modprobe ${driver_name}; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load ${driver_name}${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: ${driver_name} module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
