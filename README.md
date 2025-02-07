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
The script reads the list of tests from `tests/list.txt`, executes (`run.sh`) each test, and compares its output to the expected result (`result.out`). If the output differs, an error message and `diff` output are displayed.

## License
This project is licensed under the Apache License 2.0.
