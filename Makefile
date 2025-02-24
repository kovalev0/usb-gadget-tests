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
		storage-bot

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

# Generic rule to compile .c files into .o files
src/%.o: src/%.c src/usb_gadget_tests.h
	$(CC) -c $< -o $@ $(CFLAGS)

# Clean up generated files
clean:
	rm -f $(COMMON_OBJ) src/*/*.o tests/*/result $(foreach target,$(TARGETS),src/$(target)/$(target))
