# SPDX-License-Identifier: Apache-2.0

CC = gcc
CFLAGS = -O2 -Wall -g
LDFLAGS = -lpthread

# Common object file used by all targets
COMMON_OBJ = src/usb_gadget_tests.o

ALL_AVAILABLE_TARGETS = \
	keyboard \
	printer \
	mouse \
	ethernet \
	storage-bot \
	serial-ch341 \
	serial-ftdi_sio \
	serial-cp210x \
	serial-pl2303 \
	serial-oti6858 \
	usbtmc \
	input-tab-hanwang \
	input-tab-pegasus \
	input-tab-kbtab \
	input-tab-acecad \
	input-tab-acecad-Flair \
	input-tab-aiptek \
	sisusbvga-FULL_SPEED \
	sisusbvga-init-gfx-dev \
	sisusbvga-init-gfx-core-DDR_16Mb \
	sisusbvga-init-gfx-core-SDR_8Mb \
	sisusbvga-fops-ioctl \
	sisusbvga-fops-read_write \
	sisusbvga-fops-svace-int-overflow \
	sisusbvga-fops-svace-null-deref

# Read active targets from the list file for 'make all'
TARGETS = $(shell cat tests/list.txt)

.PHONY: all clean

# Default goal: build only targets from list.txt
all: $(TARGETS)

# Function to generate a rule for each target
# This solves the "src/%/%.o" issue by explicitly defining paths
define BUILD_RULE
$(1): src/$(1)/$(1).o $(COMMON_OBJ)
	$(CC) -o src/$(1)/$(1) $$^ $(CFLAGS) $(LDFLAGS)
endef

# Generate rules for all available targets dynamically
$(foreach t,$(ALL_AVAILABLE_TARGETS),$(eval $(call BUILD_RULE,$(t))))

# Generic rule to compile any .c file into .o file
%.o: %.c src/usb_gadget_tests.h
	$(CC) -c $< -o $@ $(CFLAGS)

# Clean everything defined in ALL_AVAILABLE_TARGETS
clean:
	rm -f $(COMMON_OBJ) src/*/*.o tests/*/result
	rm -f $(foreach t,$(ALL_AVAILABLE_TARGETS),$(wildcard src/$(t)/$(t)))
