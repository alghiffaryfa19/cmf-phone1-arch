HYBRIS ?= /opt/libhybris
CFLAGS ?= -O2 -Wall

tetris-fp: tetris-fp.c
	$(CC) $(CFLAGS) -o $@ $< -I$(HYBRIS)/include $(HYBRIS)/lib/libhybris-common.so.1 \
		-Wl,-rpath,$(HYBRIS)/lib -lpthread

.PHONY: clean install
clean:
	rm -f tetris-fp

install: tetris-fp
	install -Dm755 tetris-fp $(DESTDIR)/usr/local/bin/tetris-fp
