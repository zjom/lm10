CC      ?= cc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g -Wall -Wextra

XDG_SHELL := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
SCANNER   := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
LIBS      := $(shell pkg-config --libs wayland-client)
INCS      := $(shell pkg-config --cflags wayland-client)

all: lm10

xdg-shell-client-protocol.h:
	$(SCANNER) client-header $(XDG_SHELL) $@

xdg-shell-protocol.c:
	$(SCANNER) private-code $(XDG_SHELL) $@

lm10: main.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) $(INCS) -o $@ main.c xdg-shell-protocol.c $(LIBS)

install: lm10
	install -Dm755 lm10 $(DESTDIR)$(PREFIX)/bin/lm10

clean:
	rm -f lm10 xdg-shell-protocol.c xdg-shell-client-protocol.h

.PHONY: all install clean
