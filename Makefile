CC      = cc
CFLAGS  = -Wall -Wextra -O2 -I.

LIB_SRCS = sqlram.c sqlram_arena.c sqlram_lexer.c sqlram_parser.c sqlram_exec.c \
           sqlram_db.c sqlram_table.c sqlram_record.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

all: libsqlram.a sqlram example 

libsqlram.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

sqlram: repl.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ repl.c libsqlram.a

example: examples/example.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ examples/example.c libsqlram.a

run: sqlram
	./sqlram

clean:
	rm -f $(LIB_OBJS) libsqlram.a sqlram example

.PHONY: all run clean
