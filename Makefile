PREFIX ?= /usr/local
PKGS    = gtk+-3.0 json-glib-1.0 libsoup-3.0 ayatana-appindicator3-0.1
CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -Wl,-O1,--as-needed

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

wellide: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/wellide.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: wellide
	install -Dm755 wellide $(DESTDIR)$(PREFIX)/bin/wellide
	install -Dm644 data/wellide.desktop $(DESTDIR)$(PREFIX)/share/applications/wellide.desktop
	install -Dm644 data/wellide.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wellide \
	      $(DESTDIR)$(PREFIX)/share/applications/wellide.desktop \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/wellide.svg

clean:
	rm -f wellide $(OBJ)

.PHONY: install uninstall clean
