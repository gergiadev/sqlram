# sqlram

sqlram is a small in-memory SQL engine written in C. It stores databases, tables and records in RAM (there are no files). You can use it as a library in your own C programs, or run the interactive REPL.

This is a learning project. It is not a full SQL database (see [Limitations](#limitations)).

## Features

- Databases and tables
- `INSERT`, `SELECT`, `UPDATE`, `DELETE`, `TRUNCATE`, `DROP`
- `WHERE`, `ORDER BY`, `LIMIT`
- Data types: `int`, `bigint`, `bool`, `float`, `timestamp`, `text`
- Prepared statements with placeholders (`?`)
- Dump a database (or a table) to CSV or SQL
- A clean C library with a single global instance

## Requirements

- A C compiler (`gcc` or `clang`)
- `make`
- Linux or macOS

## Build

```sh
make
```

This creates:

| File          | What it is                                  |
|---------------|---------------------------------------------|
| `libsqlram.a` | the static library                          |
| `sqlram`      | the interactive REPL                        |
| `example`     | reads SQL from stdin and prints the results |

Clean everything with:

```sh
make clean
```

## Quick start (REPL)

```sh
$ ./sqlram
sqlram> CREATE DATABASE shop;
sqlram> USE shop;
sqlram> CREATE TABLE items { int id, text name, float price, bool active };
sqlram> INSERT INTO items { 1, "apple", 0.99, true };
sqlram> INSERT INTO items { 2, "bread", 1.50, true };
sqlram> SELECT * FROM items;
id|name|price|active
1|apple|0.99|true
2|bread|1.5|true
sqlram> SELECT name, price FROM items WHERE price > 1 ORDER BY price DESC;
name|price
bread|1.5
sqlram> quit
```

Note: sqlram uses curly brackets (`{ }`) for column and value lists, not
round brackets (`( )`).

## Using the library

Include `sqlram.h` and link with `libsqlram.a`:

```sh
cc -I. -o myprogram myprogram.c libsqlram.a
```

Example program:

```c
#include "sqlram.h"
#include <stdio.h>

int main(void) {
    sqlram_result *r;

    sqlram_init();

    r = sqlram_exec("CREATE DATABASE test;");
    sqlram_result_free(r);
    r = sqlram_exec("USE test;");
    sqlram_result_free(r);
    r = sqlram_exec("CREATE TABLE users { int id, text name };");
    sqlram_result_free(r);
    r = sqlram_exec("INSERT INTO users { 1, \"alice\" };");
    sqlram_result_free(r);
    r = sqlram_exec("INSERT INTO users { 2, \"bob\" };");
    sqlram_result_free(r);

    r = sqlram_exec("SELECT * FROM users;");
    if (!r) {
        fprintf(stderr, "error: %s\n", sqlram_error());
        return 1;
    }

    for (int i = 0; i < r->num_rows; i++) {
        printf("%ld %s\n",
               r->rows[i][0].v.i_val,
               r->rows[i][1].v.s_val);
    }

    sqlram_result_free(r);
    sqlram_close();
    return 0;
}
```

### Public API

```c
void sqlram_init(void);                          /* reset the engine */

sqlram_result *sqlram_exec(const char *sql);     /* run one statement */
const char    *sqlram_error(void);               /* last error message */
void           sqlram_result_free(sqlram_result *r);

int sqlram_dump(const char *dstName, const char *format,
                const char *dbName, const char *tblName);

sqlram_stmt  *sqlram_prepare(const char *sql);   /* prepared statement */
int           sqlram_bind(sqlram_stmt *st, int index, sqlram_value value);
sqlram_result *sqlram_exec_stmt(sqlram_stmt *st);
void          sqlram_stmt_free(sqlram_stmt *st);

void sqlram_close(void);                         /* free everything */
```

`sqlram_exec` returns a result set, or `NULL` on error (read `sqlram_error()`). For statements that do not return rows (like `INSERT`), the result has `num_cols == 0` and the number of affected rows in `affected`.

### Result set

```c
typedef struct sqlram_result {
    int            num_cols;
    char         **col_names;
    int            num_rows;
    sqlram_value **rows;   /* rows[i][j], row-major */
    int            affected;
    int            row_cap;
} sqlram_result;
```

### Values

```c
typedef enum {
    SQLRAM_INT,
    SQLRAM_BOOL,
    SQLRAM_FLOAT,
    SQLRAM_TIMESTAMP,
    SQLRAM_TEXT
} sqlram_type;

typedef struct sqlram_value {
    sqlram_type type;
    union {
        long   i_val;
        int    b_val;
        double d_val;
        time_t t_val;
        char  *s_val;
    } v;
} sqlram_value;
```

## SQL reference

### Data types

| SQL keyword      | C type    | Notes                     |
|------------------|-----------|---------------------------|
| `int`, `bigint`  | `long`    | 64-bit integer            |
| `bool`           | `int`     | `true` or `false`         |
| `float`          | `double`  | decimal number            |
| `timestamp`      | `time_t`  | date and time             |
| `text`           | `char *`  | string                    |

String literals use double quotes (`"hello"`) or single quotes (`'hello'`).
To put a quote inside a string, write it twice (`"a""b"` means `a"b`).

Timestamp literals are strings, for example `"2024-01-02"` or `"2024-01-02 10:30:00"`.

### Statements

```sql
CREATE DATABASE name;
SHOW DATABASES;
USE name;

CREATE TABLE name { int id, text name, bool ok };
SHOW TABLES;
DROP TABLE name;
DROP DATABASE name;

INSERT INTO t { 1, "alice", true };

SELECT * FROM t;
SELECT id, name FROM t;
SELECT * FROM t WHERE id >= 2 ORDER BY name DESC LIMIT 10;

UPDATE t SET name = "bob" WHERE id = 1;
DELETE FROM t WHERE id = 1;
TRUNCATE t;
```

Comparison operators: `=`, `!=`, `<`, `<=`, `>`, `>=`.

A `WHERE` clause supports one condition (no `AND` or `OR`).

## Prepared statements

You can prepare a statement once and run it many times. Placeholders (`?`) are supported in `INSERT` values:

```c
sqlram_stmt *st = sqlram_prepare("INSERT INTO t { ?, ?, ? };");

sqlram_value v;
v.type = SQLRAM_INT;
v.v.i_val = 1;
sqlram_bind(st, 0, v);

v.type = SQLRAM_TEXT;
v.v.s_val = "alice";
sqlram_bind(st, 1, v);

v.type = SQLRAM_BOOL;
v.v.b_val = 1;
sqlram_bind(st, 2, v);

sqlram_result *r = sqlram_exec_stmt(st);
sqlram_result_free(r);

sqlram_stmt_free(st);
```

Preparing and binding avoids lexing and parsing on every row. This makes bulk inserts much faster (see `benchmark`).

## Dump

Write a database (or one table) to a file:

```c
/* whole database as CSV */
sqlram_dump("out.csv", "CSV", "shop", NULL);

/* one table as INSERT statements */
sqlram_dump("out.sql", "SQL", "shop", "items");
```

- `format` is `"CSV"` or `"SQL"` (case does not matter).
- `tblName` is `NULL` to dump the whole database, or a table name.
- `SQL` format can be imported again with `sqlram_exec`.

## Project structure

```
sqlram.h          public API
sqlram_internal.h internal types
sqlram.c          engine core, public API, dump
sqlram_arena.c    bump allocator
sqlram_lexer.c    SQL tokenizer
sqlram_parser.c   builds the AST
sqlram_exec.c     dispatcher
sqlram_db.c       database operations
sqlram_table.c    table operations
sqlram_record.c   record operations (CRUD)
repl.c            interactive REPL
examples/         example and benchmark
```

## Limitations

- No `JOIN`, aggregates, `GROUP BY`, or transactions.
- `WHERE` supports one condition only.
- Data is not persisted: everything is lost when the program ends.
- One global instance (no multiple independent engines).

## License

MIT
