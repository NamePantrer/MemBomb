CC = gcc
CFLAGS = -Wall -O2
SRC = src/membomb.c

.PHONY: all clean

all: src/membomb src/membomb_raw

src/membomb: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

src/membomb_raw: $(SRC)
	$(CC) $(CFLAGS) -DMEMBOMB_RAW -o $@ $(SRC)

clean:
	rm -f src/membomb src/membomb.exe src/membomb_raw src/membomb_raw.exe
