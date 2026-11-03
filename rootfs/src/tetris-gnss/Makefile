HYBRIS ?= /opt/libhybris
CFLAGS ?= -O2 -Wall

tetris-gnss: tetris-gnss.c
	$(CC) $(CFLAGS) -o $@ $< -I$(HYBRIS)/include $(HYBRIS)/lib/libhybris-common.so.1 \
		-Wl,-rpath,$(HYBRIS)/lib -lpthread

.PHONY: clean install
clean:
	rm -f tetris-gnss

install: tetris-gnss
	install -Dm755 tetris-gnss $(DESTDIR)/usr/local/bin/tetris-gnss
