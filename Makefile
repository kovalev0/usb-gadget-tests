# SPDX-License-Identifier: Apache-2.0

CC = gcc
CFLAGS = -O2 -Wall -g
LDFLAGS = -lpthread

# Common object file used by all targets
COMMON_OBJ = src/usb_gadget_tests.o

TARGETS =	keyboard		\
		printer			\
		mouse			\
		ethernet		\
		storage-bot		\
		serial-ch341		\
		serial-ftdi_sio		\
		serial-cp210x		\
		serial-pl2303		\
		serial-oti6858		\
		usbtmc

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

# Generic rule to compile .c files into .o files
src/%.o: src/%.c src/usb_gadget_tests.h
	$(CC) -c $< -o $@ $(CFLAGS)

# Clean up generated files
clean:
	rm -f $(COMMON_OBJ) src/*/*.o tests/*/result $(foreach target,$(TARGETS),src/$(target)/$(target))
