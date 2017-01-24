PREFIX ?= /usr/local
libdir ?= /lib

WARNINGS := -Wall -Wextra -Wformat=2 -Winit-self -Wfloat-equal -Wcast-align -Wpointer-arith
CFLAGS += -std=c99 $(WARNINGS) $(shell pkg-config --cflags alsa)

%.so: %.o
	$(LINK.o) -shared $^ $(LDLIBS) -o $@

all: glcapture.so

glcapture.so: LDFLAGS += $(shell pkg-config --libs-only-L --libs-only-other alsa) -Wl,-soname,glcapture.so
glcapture.so: LDLIBS := $(shell pkg-config --libs-only-l alsa)

glcapture.o: CFLAGS += -fPIC
glcapture.o: glcapture.c hooks.h glwrangle.h

install:
	install -Dm644 glcapture.so $(DESTDIR)$(PREFIX)$(libdir)/glcapture.so

clean:
	$(RM) glcapture.*o

.PHONY: all clean install
