CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -Iinclude
LDFLAGS = -lcurl -lsqlite3 -lpthread

SRC     = src/main.c src/db.c src/checker.c src/parser.c
OBJ     = $(SRC:.c=.o)
BIN     = strainer

.PHONY: all clean

all: data $(BIN)

data:
	mkdir -p data

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(BIN)
