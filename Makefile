# ---------------------------------------------------------------
# utbd Makefile — UTC Time Broadcast Daemon
#
#   make                  Build
#   make voice            Generate voice files (PIPER_MODEL=... for Piper)
#   make install          Install binary, config and voice files
#   make install-service  Also install and register the systemd unit
#   make clean / uninstall
#
# Dependencies: spandsp and Sofia-SIP (see README.md)
#   apt install pkg-config libspandsp-dev libsofia-sip-ua-dev
#   dnf install pkgconf spandsp-devel sofia-sip-devel
#
# Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>
# SPDX-License-Identifier: GPL-3.0-or-later
# ---------------------------------------------------------------
PKG_CONFIG   ?= pkg-config
SOFIA_CFLAGS != $(PKG_CONFIG) --cflags sofia-sip-ua 2>/dev/null || true
SOFIA_LIBS   != $(PKG_CONFIG) --libs   sofia-sip-ua 2>/dev/null || echo "-lsofia-sip-ua"

# Without pkg-config: the headers are in a versioned directory,
# e.g. /usr/include/sofia-sip-1.12 (Debian) or sofia-sip-1.13
SOFIA_SEARCH ?= /usr/include /usr/local/include
ifeq ($(strip $(SOFIA_CFLAGS)),)
SOFIA_CFLAGS := $(addprefix -I,$(lastword $(sort $(wildcard $(addsuffix /sofia-sip-1.*,$(SOFIA_SEARCH))))))
endif

SPANDSP_CFLAGS != $(PKG_CONFIG) --cflags spandsp 2>/dev/null || true
SPANDSP_LIBS   != $(PKG_CONFIG) --libs   spandsp 2>/dev/null || echo "-lspandsp"

PREFIX   = /usr/local
BINDIR   = $(PREFIX)/sbin
DATADIR  = $(PREFIX)/share/utbd
CONFDIR  = /etc/utbd
UNITDIR  = /etc/systemd/system
STATION ?= wwv

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g $(SOFIA_CFLAGS) $(SPANDSP_CFLAGS) \
          -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Wformat=2
LDFLAGS = $(SOFIA_LIBS) $(SPANDSP_LIBS) -lpthread -lm -pie -Wl,-z,relro -Wl,-z,now

.PHONY: all voice install install-service uninstall clean

all: utbd

utbd: utbd.c sip.c sip.h
	$(CC) $(CFLAGS) -o $@ utbd.c sip.c $(LDFLAGS)

voice:
	./gen-voice.sh voice $(STATION)

install: utbd
	@getent group utbd >/dev/null || groupadd --system utbd
	@getent passwd utbd >/dev/null || useradd --system --gid utbd --no-create-home \
	    --home-dir /nonexistent --shell /usr/sbin/nologin --comment "UTC Time Broadcast Daemon" utbd
	install -D -m 755 utbd $(DESTDIR)$(BINDIR)/utbd
	install -d -m 755 $(DESTDIR)$(CONFDIR) $(DESTDIR)$(DATADIR)/voice
	@if [ -f $(DESTDIR)$(CONFDIR)/utbd.conf ]; then \
	    install -m 640 -g utbd contrib/utbd.conf.sample $(DESTDIR)$(CONFDIR)/utbd.conf.sample; \
	    echo "Kept existing $(CONFDIR)/utbd.conf"; \
	else \
	    install -m 640 -g utbd contrib/utbd.conf.sample $(DESTDIR)$(CONFDIR)/utbd.conf; \
	    echo "*** Edit $(CONFDIR)/utbd.conf before starting utbd ***"; \
	fi
	@if ls voice/*.wav >/dev/null 2>&1; then \
	    install -m 644 voice/*.wav $(DESTDIR)$(DATADIR)/voice/; \
	else \
	    echo "No voice files - run 'make voice' (announcements are skipped without them)"; \
	fi

install-service: install
	install -D -m 644 contrib/utbd.service $(DESTDIR)$(UNITDIR)/utbd.service
	systemctl daemon-reload
	@echo "Enable and start: systemctl enable --now utbd"

uninstall:
	-systemctl disable --now utbd 2>/dev/null
	rm -f $(DESTDIR)$(BINDIR)/utbd $(DESTDIR)$(UNITDIR)/utbd.service
	rm -rf $(DESTDIR)$(DATADIR)
	-systemctl daemon-reload 2>/dev/null
	@echo "Kept $(CONFDIR) and the utbd user"

clean:
	rm -f utbd
