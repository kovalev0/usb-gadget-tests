# SPDX-License-Identifier: Apache-2.0

CC = gcc
CFLAGS = -O2 -Wall -g
LDFLAGS = -lpthread

# Common object file used by all targets
COMMON_OBJ = src/usb_gadget_tests.o

TARGETS = \
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
	sisusbvga-fops-ioctl

.PHONY: all clean

all: $(TARGETS)

keyboard: src/keyboard/keyboard.o $(COMMON_OBJ)
	$(CC) -o src/keyboard/keyboard $^ $(CFLAGS) $(LDFLAGS)

printer: src/printer/printer.o $(COMMON_OBJ)
	$(CC) -o src/printer/printer $^ $(CFLAGS) $(LDFLAGS)

mouse: src/mouse/mouse.o $(COMMON_OBJ)
	$(CC) -o src/mouse/mouse $^ $(CFLAGS) $(LDFLAGS)

ethernet: src/ethernet/ethernet.o $(COMMON_OBJ)
	$(CC) -o src/ethernet/ethernet $^ $(CFLAGS) $(LDFLAGS)

storage-bot: src/storage-bot/storage-bot.o $(COMMON_OBJ)
	$(CC) -o src/storage-bot/storage-bot $^ $(CFLAGS) $(LDFLAGS)

serial-ch341: src/serial-ch341/serial-ch341.o $(COMMON_OBJ)
	$(CC) -o src/serial-ch341/serial-ch341 $^ $(CFLAGS) $(LDFLAGS)

serial-ftdi_sio: src/serial-ftdi_sio/serial-ftdi_sio.o $(COMMON_OBJ)
	$(CC) -o src/serial-ftdi_sio/serial-ftdi_sio $^ $(CFLAGS) $(LDFLAGS)

serial-cp210x: src/serial-cp210x/serial-cp210x.o $(COMMON_OBJ)
	$(CC) -o src/serial-cp210x/serial-cp210x $^ $(CFLAGS) $(LDFLAGS)

serial-pl2303: src/serial-pl2303/serial-pl2303.o $(COMMON_OBJ)
	$(CC) -o src/serial-pl2303/serial-pl2303 $^ $(CFLAGS) $(LDFLAGS)

serial-oti6858: src/serial-oti6858/serial-oti6858.o $(COMMON_OBJ)
	$(CC) -o src/serial-oti6858/serial-oti6858 $^ $(CFLAGS) $(LDFLAGS)

usbtmc: src/usbtmc/usbtmc.o $(COMMON_OBJ)
	$(CC) -o src/usbtmc/usbtmc $^ $(CFLAGS) $(LDFLAGS)

input-tab-hanwang: src/input-tab-hanwang/input-tab-hanwang.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-hanwang/input-tab-hanwang $^ $(CFLAGS) $(LDFLAGS)

input-tab-pegasus: src/input-tab-pegasus/input-tab-pegasus.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-pegasus/input-tab-pegasus $^ $(CFLAGS) $(LDFLAGS)

input-tab-kbtab: src/input-tab-kbtab/input-tab-kbtab.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-kbtab/input-tab-kbtab $^ $(CFLAGS) $(LDFLAGS)

input-tab-acecad: src/input-tab-acecad/input-tab-acecad.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-acecad/input-tab-acecad $^ $(CFLAGS) $(LDFLAGS)

input-tab-acecad-Flair: src/input-tab-acecad-Flair/input-tab-acecad-Flair.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-acecad-Flair/input-tab-acecad-Flair $^ $(CFLAGS) $(LDFLAGS)

input-tab-aiptek: src/input-tab-aiptek/input-tab-aiptek.o $(COMMON_OBJ)
	$(CC) -o src/input-tab-aiptek/input-tab-aiptek $^ $(CFLAGS) $(LDFLAGS)

sisusbvga-FULL_SPEED: src/sisusbvga-FULL_SPEED/sisusbvga-FULL_SPEED.o $(COMMON_OBJ)
	$(CC) -o src/sisusbvga-FULL_SPEED/sisusbvga-FULL_SPEED $^ $(CFLAGS) $(LDFLAGS)

sisusbvga-init-gfx-dev: src/sisusbvga-init-gfx-dev/sisusbvga-init-gfx-dev.o $(COMMON_OBJ)
	$(CC) -o src/sisusbvga-init-gfx-dev/sisusbvga-init-gfx-dev $^ $(CFLAGS) $(LDFLAGS)

sisusbvga-init-gfx-core-DDR_16Mb: src/sisusbvga-init-gfx-core-DDR_16Mb/sisusbvga-init-gfx-core-DDR_16Mb.o $(COMMON_OBJ)
	$(CC) -o src/sisusbvga-init-gfx-core-DDR_16Mb/sisusbvga-init-gfx-core-DDR_16Mb $^ $(CFLAGS) $(LDFLAGS)

sisusbvga-init-gfx-core-SDR_8Mb: src/sisusbvga-init-gfx-core-SDR_8Mb/sisusbvga-init-gfx-core-SDR_8Mb.o $(COMMON_OBJ)
	$(CC) -o src/sisusbvga-init-gfx-core-SDR_8Mb/sisusbvga-init-gfx-core-SDR_8Mb $^ $(CFLAGS) $(LDFLAGS)

sisusbvga-fops-ioctl: src/sisusbvga-fops-ioctl/sisusbvga-fops-ioctl.o $(COMMON_OBJ)
	$(CC) -o src/sisusbvga-fops-ioctl/sisusbvga-fops-ioctl $^ $(CFLAGS) $(LDFLAGS)

# Generic rule to compile .c files into .o files
src/%.o: src/%.c src/usb_gadget_tests.h
	$(CC) -c $< -o $@ $(CFLAGS)

# Clean up generated files
clean:
	rm -f $(COMMON_OBJ) src/*/*.o tests/*/result $(foreach target,$(TARGETS),src/$(target)/$(target))
