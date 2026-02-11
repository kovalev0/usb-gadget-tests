#!/bin/bash

pushd $(dirname "$(readlink -e "$0")") >/dev/null

# Check if /dev/raw-gadget exists, otherwise load raw_gadget module
if [[ ! -e /dev/raw-gadget ]]; then
    modprobe raw_gadget
    sleep 1  # Give some time for the device to appear
    if [[ ! -e /dev/raw-gadget ]]; then
        echo "Error: /dev/raw-gadget is missing after loading raw_gadget module."
        exit 1
    fi
fi

# Check if /sys/class/udc/dummy_udc.0/uevent contains the expected value
EXPECTED_UDC="USB_UDC_NAME=dummy_udc"
if [[ "$(cat /sys/class/udc/dummy_udc.0/uevent 2>/dev/null)" != "$EXPECTED_UDC" ]]; then
    modprobe dummy_hcd
    sleep 1  # Allow the module to initialize
    if [[ "$(cat /sys/class/udc/dummy_udc.0/uevent 2>/dev/null)" != "$EXPECTED_UDC" ]]; then
        echo "Error: dummy_hcd module did not initialize correctly."
        exit 1
    fi
fi

# Run each test listed in tests/list.txt
while IFS= read -r test_name; do
    test_dir="tests/$test_name"
    test_script="$test_dir/run.sh"
    result_file="$test_dir/result"
    result_outs_dir="$test_dir/result.outs"

    if [[ ! -x "$test_script" ]]; then
        echo "Skipping $test_name: $test_script is not executable or missing."
        continue
    fi

    # Check if result.outs directory exists and contains at least out.1
    if [[ ! -d "$result_outs_dir" || ! -f "$result_outs_dir/out.1" ]]; then
        echo "Skipping $test_name: $result_outs_dir/out.1 is missing."
        continue
    fi

    echo "Running test: $test_name"
    timeout 60 "$test_script"
    exit_code=$?

    if [[ $exit_code -eq 70 ]]; then
        echo -e "$test_name \e[36m[Skip]\e[0m"
        continue
    fi

    if [[ $exit_code -eq 124 ]]; then
        echo -e "$test_name \e[31m[Timeout]\e[0m"
        continue
    fi

    if [[ ! -f "$result_file" ]]; then
        echo -e "$test_name \e[31m[Failed]\e[0m (No result file)"
        continue
    fi

    # Compare result_file with each expected result in result.outs
    match_found=false
    for expected_result in "$result_outs_dir"/out.*; do
        if diff -q "$result_file" "$expected_result" &>/dev/null; then
            match_found=true
            break
        fi
    done

    if [[ "$match_found" == true ]]; then
        echo -e "$test_name \e[32m[Ok]\e[0m"
    else
        echo -e "$test_name \e[31m[Failed]\e[0m"
        diff "$result_outs_dir/out.1" "$result_file"
    fi
done < tests/list.txt

popd >/dev/null
