# Built on the phone (needs /opt/libhybris and gtk4): make && sudo make install
CFLAGS ?= -O2 -Wall
LDFLAGS += -Wl,--as-needed
# Link libhybris-common by path (RUNPATH only covers our own deps): -L/opt/libhybris/lib
# or LD_LIBRARY_PATH would let its libvulkan/libEGL shadow the system ones gtk4 uses
HYBRIS = -I/opt/libhybris/include /opt/libhybris/lib/libhybris-common.so.1 -Wl,-rpath,/opt/libhybris/lib

all: kamera tetris-cam2probe

kamera: app.c ndkcam.c ndkcam.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ app.c ndkcam.c $(shell pkg-config --cflags --libs gtk4) $(HYBRIS)

tetris-cam2probe: probe.c ndkcam.c ndkcam.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ probe.c ndkcam.c $(HYBRIS)

install: all
	install -Dm755 kamera /usr/local/lib/tetris-camera-app/kamera
	install -Dm755 tetris-cam2probe /usr/local/bin/tetris-cam2probe

clean:
	rm -f kamera tetris-cam2probe
