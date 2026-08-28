/* Covers the prepared-statement upsert path, which plain SQL text cannot
 * reach: sqlram_exec_stmt() has its own NODE_INSERT branch.
 * Build with: cc -I. -o tests/test_prepared_upsert tests/test_prepared_upsert.c libsqlram.a */
#include "sqlram.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check (int cond, const char *what) {
    printf ("%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

static void run (const char *sql) {
    sqlram_result *r = sqlram_exec (sql);
    if (!r) {
        printf ("FAIL: %s -> %s\n", sql, sqlram_error ());
        failures++;
        return;
    }
    sqlram_result_free (r);
}

/* Number of rows currently in t. */
static int count_rows (void) {
    sqlram_result *r = sqlram_exec ("SELECT * FROM t;");
    if (!r) {
        printf ("FAIL: select -> %s\n", sqlram_error ());
        failures++;
        return -1;
    }
    int n = r->num_rows;
    sqlram_result_free (r);
    return n;
}

/* Value of column v for the row with id == id, or NULL if absent. */
static char *value_of (long id, char *buf, size_t bufsz) {
    sqlram_result *r = sqlram_exec ("SELECT id, v FROM t;");
    if (!r) {
        return NULL;
    }
    char *found = NULL;
    for (int i = 0; i < r->num_rows; i++) {
        if (r->rows[i][0].v.i_val == id) {
            snprintf (buf, bufsz, "%s", r->rows[i][1].v.s_val ? r->rows[i][1].v.s_val : "");
            found = buf;
            break;
        }
    }
    sqlram_result_free (r);
    return found;
}

int main (void) {
    sqlram_init ();

    run ("CREATE DATABASE p;");
    run ("USE p;");
    run ("CREATE TABLE t { int id, text v };");

    sqlram_stmt *st = sqlram_prepare ("INSERT INTO t { ?, ? } ON CONFLICT { id } UPDATE;");
    if (!st) {
        printf ("FAIL: prepare -> %s\n", sqlram_error ());
        return 1;
    }

    /* 100 distinct keys, each written three times: the table must end up with
     * 100 rows, holding the value from the last pass. */
    const int keys = 100;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < keys; i++) {
            char text[32];
            snprintf (text, sizeof (text), "pass%d-key%d", pass, i);

            sqlram_value v;
            v.type = SQLRAM_INT;
            v.v.i_val = i;
            if (sqlram_bind (st, 0, v) < 0) {
                printf ("FAIL: bind 0 -> %s\n", sqlram_error ());
                return 1;
            }
            v.type = SQLRAM_TEXT;
            v.v.s_val = text;
            if (sqlram_bind (st, 1, v) < 0) {
                printf ("FAIL: bind 1 -> %s\n", sqlram_error ());
                return 1;
            }

            sqlram_result *r = sqlram_exec_stmt (st);
            if (!r) {
                printf ("FAIL: exec_stmt -> %s\n", sqlram_error ());
                return 1;
            }
            if (r->affected != 1) {
                check (0, "exec_stmt reports one affected row");
            }
            sqlram_result_free (r);
        }
    }

    check (count_rows () == keys, "300 upserts over 100 keys leave 100 rows");

    char buf[64];
    check (value_of (0, buf, sizeof (buf)) && !strcmp (buf, "pass2-key0"), "first key holds the last value written");
    check (value_of (99, buf, sizeof (buf)) && !strcmp (buf, "pass2-key99"), "last key holds the last value written");

    /* A prepared upsert must still see rows an interleaved plain INSERT added. */
    run ("INSERT INTO t { 500, \"plain\" };");
    check (count_rows () == keys + 1, "plain INSERT appends alongside the index");

    sqlram_value v;
    v.type = SQLRAM_INT;
    v.v.i_val = 500;
    sqlram_bind (st, 0, v);
    v.type = SQLRAM_TEXT;
    v.v.s_val = "upserted";
    sqlram_bind (st, 1, v);
    sqlram_result *r = sqlram_exec_stmt (st);
    if (r) {
        sqlram_result_free (r);
    }
    check (count_rows () == keys + 1, "upsert finds the plainly inserted row");
    check (value_of (500, buf, sizeof (buf)) && !strcmp (buf, "upserted"), "and overwrites it");

    /* An interleaved DELETE drops the index; the next upsert must rebuild it. */
    run ("DELETE FROM t WHERE id = 500;");
    sqlram_bind (st, 0, v = (sqlram_value){.type = SQLRAM_INT, .v.i_val = 7});
    v.type = SQLRAM_TEXT;
    v.v.s_val = "after-delete";
    sqlram_bind (st, 1, v);
    r = sqlram_exec_stmt (st);
    if (r) {
        sqlram_result_free (r);
    }
    check (count_rows () == keys, "upsert after DELETE rebuilds the index without duplicating");
    check (value_of (7, buf, sizeof (buf)) && !strcmp (buf, "after-delete"), "and still updates in place");

    sqlram_stmt_free (st);
    sqlram_close ();

    printf (failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
