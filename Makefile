CC      = cc
CFLAGS  = -Wall -Wextra -O2 -I.

LIB_SRCS = sqlram.c sqlram_arena.c sqlram_lexer.c sqlram_parser.c sqlram_exec.c \
           sqlram_db.c sqlram_table.c sqlram_record.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# Golden-file tests: every tests/*.sql is piped through ./example and diffed
# against the matching .expected. C tests are every tests/test_*.c.
SQL_TESTS = $(wildcard tests/*.sql)
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(TEST_SRCS:.c=)

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
test: example $(TEST_BINS)
	@for f in $(SQL_TESTS); do \
	    grep -v '^[[:space:]]*--' $$f | ./example | diff -u $${f%.sql}.expected - \
	        && echo "ok: $$f" || exit 1; \
	done
	@for b in $(TEST_BINS); do ./$$b || exit 1; done

tests/%: tests/%.c libsqlram.a
	$(CC) $(CFLAGS) -o $@ $< libsqlram.a

clean:
	rm -f $(LIB_OBJS) libsqlram.a sqlram example $(TEST_BINS)

.PHONY: all run test clean
