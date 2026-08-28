CC      = cc
CFLAGS  = -Wall -Wextra -O2 -I.

LIB_SRCS = sqlram.c sqlram_arena.c sqlram_lexer.c sqlram_parser.c sqlram_exec.c \
           sqlram_db.c sqlram_table.c sqlram_record.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

TEST_BIN = tests/test_prepared_upsert

all: libsqlram.a sqlram example 

libsqlram.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

sqlram: repl.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ repl.c libsqlram.a

example: examples/example.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ examples/example.c libsqlram.a

run: sqlram
	./sqlram

# sqlram has no comment syntax, so '--' lines are stripped before feeding
# the script to the engine.
test: example $(TEST_BIN)
	@grep -v '^[[:space:]]*--' tests/upsert.sql | ./example | diff -u tests/upsert.expected - \
	    && echo "ok: tests/upsert.sql"
	@./$(TEST_BIN)

$(TEST_BIN): tests/test_prepared_upsert.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ tests/test_prepared_upsert.c libsqlram.a

clean:
	rm -f $(LIB_OBJS) libsqlram.a sqlram example $(TEST_BIN)

.PHONY: all run test clean
