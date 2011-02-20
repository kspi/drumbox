PACKAGES := jack fftw3f
CFLAGS := $(shell pkg-config --cflags $(PACKAGES)) -std=c99 -Wall -pedantic -g -ggdb -g3
LDFLAGS := $(shell pkg-config --libs $(PACKAGES))

all: drumbox

clean:
	rm -f drumbox