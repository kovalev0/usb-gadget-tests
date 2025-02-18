# SPDX-License-Identifier: Apache-2.0

CC=gcc
CFLAGS=-O2 -Wall -g

.PHONY: all

all: keyboard printer mouse

keyboard: src/keyboard/keyboard.c
	$(CC) -o src/$@/$@ $< $(CFLAGS) -lpthread

printer: src/printer/printer.c
	$(CC) -o src/$@/$@ $< $(CFLAGS) -lpthread

mouse: src/mouse/mouse.c
	$(CC) -o src/$@/$@ $< $(CFLAGS) -lpthread
