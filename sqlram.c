#include "sqlram_internal.h"
#include <strings.h>

DatabaseArray db_array = {0};

static char sqlram_errbuf[512];

void sqlram_set_error (const char *fmt, ...) {
    va_list ap;
    va_start (ap, fmt);
    vsnprintf (sqlram_errbuf, sizeof (sqlram_errbuf), fmt, ap);
    va_end (ap);
}

const char *sqlram_error (void) {
    return sqlram_errbuf;
}

sqlram_value value_dup (sqlram_value v) {
    sqlram_value out = v;
    if (v.type == SQLRAM_TEXT) {
        out.v.s_val = v.v.s_val ? strdup (v.v.s_val) : NULL;
    }
    return out;
}

sqlram_result *result_new (int ncols) {
    sqlram_result *r = calloc (1, sizeof (*r));
    if (!r) {
        return NULL;
    }
    r->num_cols = ncols;
    if (ncols > 0) {
        r->col_names = calloc (ncols, sizeof (char *));
        if (!r->col_names) {
            free (r);
            return NULL;
        }
    }
    return r;
}

void result_set_col (sqlram_result *r, int i, const char *name) {
    if (!r || i < 0 || i >= r->num_cols) {
        return;
    }
    r->col_names[i] = strdup (name);
}

void result_add_row (sqlram_result *r, Field *vals, int n) {
    if (!r) {
        return;
    }

    if (r->num_rows == r->row_cap) {
        int new_cap = r->row_cap ? r->row_cap * 2 : 16;
        sqlram_value **rows = realloc (r->rows, new_cap * sizeof (sqlram_value *));
        if (!rows) {
            return;
        }
        r->rows = rows;
        r->row_cap = new_cap;
    }

    int width = n > 0 ? n : 1;
    sqlram_value *row = calloc (width, sizeof (sqlram_value));
    if (!row) {
        return;
    }
    for (int i = 0; i < n; i++) {
        row[i] = value_dup (vals[i]);
    }
    r->rows[r->num_rows] = row;
    r->num_rows++;
}

void sqlram_result_free (sqlram_result *r) {
    if (!r) {
        return;
    }
    if (r->col_names) {
        for (int i = 0; i < r->num_cols; i++) {
            free (r->col_names[i]);
        }
        free (r->col_names);
    }
    if (r->rows) {
        for (int i = 0; i < r->num_rows; i++) {
            for (int j = 0; j < r->num_cols; j++) {
                field_free (&r->rows[i][j]);
            }
            free (r->rows[i]);
        }
        free (r->rows);
    }
    free (r);
}

void sqlram_init (void) {
    sqlram_close ();
}

/* Scratch arena for one-shot statements; reset at each sqlram_exec(). */
static Arena scratch;

sqlram_result *sqlram_exec (const char *sql) {
    sqlram_errbuf[0] = '\0';
    if (!sql || !*sql) {
        sqlram_set_error ("empty statement");
        return NULL;
    }

    arena_reset (&scratch);
    Token *tk = lexer (&scratch, (char *)sql);
    Node *nd = parser (&scratch, tk);
    if (!nd) {
        sqlram_set_error ("syntax error");
        return NULL;
    }

    sqlram_result *res = NULL;
    if (nd->Nkind == NODE_INVALID) {
        sqlram_set_error ("invalid statement: %s", nd->pos ? nd->pos : sql);
    } else {
        res = exec_dispatch (nd);
    }
    arena_reset (&scratch);
    return res;
}

struct sqlram_stmt {
    Arena arena;
    Node *node;
    sqlram_value *params; /* bound values (real malloc, text strdup) */
    int *bound;           /* 1 if params[i] is set */
    int num_params;
};

sqlram_stmt *sqlram_prepare (const char *sql) {
    sqlram_errbuf[0] = '\0';
    if (!sql || !*sql) {
        sqlram_set_error ("empty statement");
        return NULL;
    }

    sqlram_stmt *st = calloc (1, sizeof (*st));
    if (!st) {
        return NULL;
    }
    arena_init (&st->arena);

    Token *tk = lexer (&st->arena, (char *)sql);
    Node *nd = parser (&st->arena, tk);
    if (!nd) {
        sqlram_set_error ("syntax error");
        arena_free (&st->arena);
        free (st);
        return NULL;
    }
    if (nd->Nkind == NODE_INVALID) {
        sqlram_set_error ("invalid statement: %s", nd->pos ? nd->pos : sql);
        arena_free (&st->arena);
        free (st);
        return NULL;
    }

    st->node = nd;
    st->num_params = (nd->Nkind == NODE_INSERT) ? nd->nodeAST.Insert.numParams : 0;
    if (st->num_params > 0) {
        st->params = calloc (st->num_params, sizeof (sqlram_value));
        st->bound = calloc (st->num_params, sizeof (int));
        if (!st->params || !st->bound) {
            arena_free (&st->arena);
            free (st->params);
            free (st->bound);
            free (st);
            return NULL;
        }
    }
    return st;
}

int sqlram_bind (sqlram_stmt *st, int index, sqlram_value value) {
    if (!st || index < 0 || index >= st->num_params) {
        sqlram_set_error ("invalid parameter index");
        return -1;
    }
    if (st->bound[index] && st->params[index].type == SQLRAM_TEXT) {
        free (st->params[index].v.s_val);
    }
    st->params[index] = value_dup (value);
    st->bound[index] = 1;
    return 0;
}

sqlram_result *sqlram_exec_stmt (sqlram_stmt *st) {
    sqlram_errbuf[0] = '\0';
    if (!st || !st->node) {
        sqlram_set_error ("invalid statement");
        return NULL;
    }

    Node *node = st->node;

    if (node->Nkind == NODE_INSERT) {
        struct InsertS *ins = &node->nodeAST.Insert;

        for (int i = 0; i < ins->numParams; i++) {
            if (!st->bound[i]) {
                sqlram_set_error ("unbound parameter %d", i);
                return NULL;
            }
        }

        Field *vals = malloc (ins->numValues * sizeof (Field));
        if (!vals && ins->numValues > 0) {
            sqlram_set_error ("out of memory");
            return NULL;
        }
        for (int i = 0; i < ins->numValues; i++) {
            int k = ins->param[i];
            vals[i] = (k >= 0) ? st->params[k] : ins->values[i];
        }

        int rc = exec_insert (ins->tblname, vals, ins->numValues);
        free (vals);
        if (rc < 0) {
            return NULL;
        }

        sqlram_result *r = result_new (0);
        if (r) {
            r->affected = 1;
        }
        return r;
    }

    return exec_dispatch (node);
}

void sqlram_stmt_free (sqlram_stmt *st) {
    if (!st) {
        return;
    }
    if (st->params) {
        for (int i = 0; i < st->num_params; i++) {
            if (st->params[i].type == SQLRAM_TEXT) {
                free (st->params[i].v.s_val);
            }
        }
        free (st->params);
    }
    free (st->bound);
    arena_free (&st->arena);
    free (st);
}

void sqlram_close (void) {
    for (size_t i = 0; i < db_array.count; i++) {
        free (db_array.dbs[i].name);
        table_free_list (db_array.dbs[i].tables);
    }
    free (db_array.dbs);
    db_array.dbs = NULL;
    db_array.count = 0;
    db_array.capacity = 0;
    db_array.usedIdx = -1;
    arena_free (&scratch);
}

/* Writes a single value. In SQL mode, timestamps and text are quoted so the
 * output can be re-imported with sqlram_exec(). */
static void dump_value (FILE *f, const Field *v, int is_sql) {
    char ts[32];
    switch (v->type) {
    case SQLRAM_INT:
        fprintf (f, "%ld", v->v.i_val);
        break;
    case SQLRAM_BOOL:
        fprintf (f, "%s", v->v.b_val ? "true" : "false");
        break;
    case SQLRAM_FLOAT:
        fprintf (f, "%.17g", v->v.d_val);
        break;
    case SQLRAM_TIMESTAMP: {
        struct tm tmv;
        localtime_r (&v->v.t_val, &tmv);
        strftime (ts, sizeof (ts), "%Y-%m-%d %H:%M:%S", &tmv);
        if (is_sql) {
            fprintf (f, "\"%s\"", ts);
        } else {
            fprintf (f, "%s", ts);
        }
        break;
    }
    case SQLRAM_TEXT: {
        fputc ('"', f);
        const char *p = v->v.s_val ? v->v.s_val : "";
        for (; *p; p++) {
            if (*p == '"') {
                fputc ('"', f);
            }
            fputc (*p, f);
        }
        fputc ('"', f);
        break;
    }
    }
}

int sqlram_dump (const char *dstName, const char *format, const char *dbName, const char *tblName) {
    sqlram_errbuf[0] = '\0';
    if (!dstName || !format || !dbName) {
        sqlram_set_error ("missing argument");
        return -1;
    }

    int is_sql;
    if (!strcasecmp (format, "csv")) {
        is_sql = 0;
    } else if (!strcasecmp (format, "sql")) {
        is_sql = 1;
    } else {
        sqlram_set_error ("unknown format '%s' (use CSV or SQL)", format);
        return -1;
    }

    Database *db = NULL;
    for (size_t i = 0; i < db_array.count; i++) {
        if (!strcmp (dbName, db_array.dbs[i].name)) {
            db = &db_array.dbs[i];
            break;
        }
    }
    if (!db) {
        sqlram_set_error ("database '%s' not found", dbName);
        return -1;
    }

    if (tblName) {
        int found = 0;
        for (Table *t = db->tables; t; t = t->next) {
            if (!strcmp (tblName, t->name)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            sqlram_set_error ("table '%s' not found", tblName);
            return -1;
        }
    }

    FILE *f = fopen (dstName, "w");
    if (!f) {
        sqlram_set_error ("cannot open '%s'", dstName);
        return -1;
    }

    for (Table *t = db->tables; t; t = t->next) {
        if (tblName && strcmp (tblName, t->name) != 0) {
            continue;
        }

        if (!is_sql) {
            for (int i = 0; i < t->numFields; i++) {
                if (i) {
                    fputc (',', f);
                }
                fprintf (f, "%s", t->fields[i]->fieldName);
            }
            fputc ('\n', f);
        }

        for (Record *r = t->records; r; r = r->next) {
            if (is_sql) {
                fprintf (f, "INSERT INTO %s { ", t->name);
                for (int i = 0; i < t->numFields; i++) {
                    if (i) {
                        fputs (", ", f);
                    }
                    dump_value (f, &r->fields[i], 1);
                }
                fputs (" };\n", f);
            } else {
                for (int i = 0; i < t->numFields; i++) {
                    if (i) {
                        fputc (',', f);
                    }
                    dump_value (f, &r->fields[i], 0);
                }
                fputc ('\n', f);
            }
        }
    }

    if (fclose (f) != 0) {
        sqlram_set_error ("error writing '%s'", dstName);
        return -1;
    }
    return 0;
}
