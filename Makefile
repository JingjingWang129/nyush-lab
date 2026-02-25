CC=gcc
CFLAGS=-g -pedantic -std=gnu17 -Wall -Werror -Wextra

.PHONY: all
all: nyush

nyush: nyush.o
	$(CC) $(CFLAGS) -o nyush nyush.o

nyush.o: nyush.c
	$(CC) $(CFLAGS) -c nyush.c

.PHONY: clean
clean:
	rm -f *.o nyush