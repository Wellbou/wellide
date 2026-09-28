PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS    = gtk+-3.0 json-glib-1.0 libsoup-3.0

# system tray: libayatana-appindicator on Linux if present, GtkStatusIcon otherwise
ifeq ($(OS),Windows_NT)
  EXE      = .exe
  LDLIBS  += -lwininet -mwindows
  WINRES   = build/wellide.res.o
else
  ifeq ($(shell $(PKG_CONFIG) --exists ayatana-appindicator3-0.1 && echo y),y)
    PKGS   += ayatana-appindicator3-0.1
    CPPFLAGS += -DHAVE_APPINDICATOR
  endif
endif

CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
ifneq ($(OS),Windows_NT)
  LDFLAGS += -Wl,-O1,--as-needed
endif

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

wellide$(EXE): $(OBJ) $(WINRES)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(WINRES) $(LDLIBS)

build/%.o: src/%.c src/wellide.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/wellide.res.o: packaging/windows/wellide.rc packaging/windows/wellide.ico packaging/windows/wellide.manifest | build
	windres -I packaging/windows $< -O coff -o $@

build:
	mkdir -p build

install: wellide
	install -Dm755 wellide $(DESTDIR)$(PREFIX)/bin/wellide
	install -Dm644 data/wellide.desktop $(DESTDIR)$(PREFIX)/share/applications/wellide.desktop
	install -Dm644 data/icons/wellide.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide.svg
	install -Dm644 data/icons/wellide-on.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide-on.svg
	for s in 16 32 48 64 128 256; do \
	  install -Dm644 data/icons/wellide-$$s.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/wellide.png; \
	done
	install -Dm644 data/io.github.wellbou.wellide.metainfo.xml \
	  $(DESTDIR)$(PREFIX)/share/metainfo/io.github.wellbou.wellide.metainfo.xml

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wellide \
	      $(DESTDIR)$(PREFIX)/share/applications/wellide.desktop \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide.svg \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide-on.svg \
	      $(DESTDIR)$(PREFIX)/share/metainfo/io.github.wellbou.wellide.metainfo.xml
	for s in 16 32 48 64 128 256; do rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/wellide.png; done

art:
	python3 tools/art.py

clean:
	rm -rf build wellide wellide.exe

.PHONY: install uninstall clean art
