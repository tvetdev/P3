CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99

mysh: mysh.c
        $(CC) $(CFLAGS) -o mysh mysh.c

clean:
        rm -f mysh

.PHONY: clean
