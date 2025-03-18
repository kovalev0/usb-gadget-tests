# USB Gadget Tests
This repository provides a testing framework for the USB subsystem, primarily focusing on USB gadget functionality. It uses the [raw-gadget](https://github.com/xairy/raw-gadget) interface to emulate USB devices such as keyboards and printers.

## Overview
The emulated USB device implementations are based on code from the [`raw-gadget`](https://github.com/xairy/raw-gadget) repository but have been modified for single-run execution. The tests validate gadget functionality by comparing their output against expected results.

## Kernel Requirements
The Linux kernel must be compiled with the following configurations enabled (`y` for built-in, `m` for modules):

```plaintext
CONFIG_USB_GADGET=m
CONFIG_USB_RAW_GADGET=m
CONFIG_USB_DUMMY_HCD=m
```
## Usage
### Compilation
To compile the USB gadget emulation binaries, run:
```bash
$ make
```
### Running Tests
To execute all tests, run:
```bash
$ sudo ./check.sh
```
The script reads the list of tests from `tests/list.txt`, executes (`run.sh`) each test, and compares its output to the expected results located in the `result.outs` directory (`out.1`, `out.2`, etc., with `out.1` being mandatory). If the output matches any of the expected results, the test passes; otherwise, an error message and the `diff` output against `out.1` are displayed.

#### Test Execution Status

- **[Ok]** - Success
- **[Failed]** - Test execution failed
- **[Timeout]** - Test failed due to exceeding the 30-second execution limit
- **[Skip]** - Test skipped due to unavailable module

## License
This project is licensed under the Apache License 2.0.
