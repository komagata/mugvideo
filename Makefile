PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -g
PREFIX ?= /usr/local

PKGS = gtk4 gstreamer-1.0 gstreamer-app-1.0
TARGET = mugvideo
SRC = src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $$($(PKG_CONFIG) --cflags --libs $(PKGS))

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 data/mugvideo.desktop $(DESTDIR)$(PREFIX)/share/applications/mugvideo.desktop

clean:
	rm -f $(TARGET)

.PHONY: all run install clean
