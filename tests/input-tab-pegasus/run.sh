#!/bin/bash

# Get the absolute path of the script directory
script_dir="$(dirname "$(readlink -e "$0")")"
pushd "$script_dir" >/dev/null || exit 1

executable="../../src/input-tab-pegasus/input-tab-pegasus"

# Check if the executable exists and is runnable
if [[ ! -x "$executable" ]]; then
    echo "Error: $executable is missing or not executable."
    exit 1
fi

YELLOW='\033[1;33m'
NC='\033[0m'

# pegasus_notetaker built-in or already loaded ?
if [[ ! -d "/sys/bus/usb/drivers/pegasus_notetaker" ]]; then
    if modinfo pegasus_notetaker >/dev/null 2>&1; then
        if modprobe pegasus_notetaker; then
            sleep 1
        else
            echo -e "${YELLOW}Error: Failed to load pegasus_notetaker${NC}"
            exit 70
        fi
    else
            echo -e "${YELLOW}Warning: pegasus_notetaker module is not available (not built-in or loadable).${NC}"
            exit 70
    fi
fi

# Run the test and save the output
"$executable" &> result

popd >/dev/null
