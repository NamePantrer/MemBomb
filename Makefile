CC = gcc
CFLAGS = -Wall -Wextra -O2
SRC = src/membomb.c
OUT = src/membomb

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(OUT) $(OUT).exe
