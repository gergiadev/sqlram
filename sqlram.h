#ifndef SQLRAM_H
#define SQLRAM_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sqlram: in-memory SQL engine exposed as a C library.
 * Single global instance; see sqlram_exec() and sqlram_error().
 * Result sets are freed with sqlram_result_free(). */

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

typedef struct sqlram_result {
    int            num_cols;
    char         **col_names;
    int            num_rows;
    sqlram_value **rows;
    int            affected;
    int            row_cap;
} sqlram_result;

void sqlram_init(void);

/* Executes an SQL statement, returning a result to free with
 * sqlram_result_free(), or NULL on error (see sqlram_error()). */
sqlram_result *sqlram_exec(const char *sql);

const char *sqlram_error(void);

void sqlram_result_free(sqlram_result *r);

void sqlram_close(void);

/* Dumps a database (or a single table when tblName != NULL) to dstName.
 * format is "CSV" (header + comma-separated rows) or "SQL" (INSERT stmts).
 * Returns 0 on success, -1 on error (see sqlram_error()). */
int sqlram_dump(const char *dstName, const char *format,
                const char *dbName, const char *tblName);

/* Prepared statement: lex + parse once, then execute many times with
 * bound parameters. Placeholders '?' are supported in INSERT values. */
typedef struct sqlram_stmt sqlram_stmt;

sqlram_stmt  *sqlram_prepare(const char *sql);
int           sqlram_bind(sqlram_stmt *st, int index, sqlram_value value);
sqlram_result *sqlram_exec_stmt(sqlram_stmt *st);
void          sqlram_stmt_free(sqlram_stmt *st);

#ifdef __cplusplus
}
#endif

#endif
