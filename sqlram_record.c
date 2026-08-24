#define _XOPEN_SOURCE 700

#include "sqlram_internal.h"

static void record_free (Table *t, Record *r) {
    if (!r) {
        return;
    }
    for (int i = 0; i < t->numFields; i++) {
        field_free (&r->fields[i]);
    }
    free (r);
}

void record_free_all (Table *t) {
    Record *r = t->records;
    while (r) {
        Record *next = r->next;
        record_free (t, r);
        r = next;
    }
    t->records = NULL;
    t->tail = NULL;
}

static int value_cmp (sqlram_type ty, const sqlram_value *a, const sqlram_value *b) {
    switch (ty) {
    case SQLRAM_INT: {
        long x = a->v.i_val, y = b->v.i_val;
        return (x > y) - (x < y);
    }
    case SQLRAM_BOOL: {
        int x = a->v.b_val, y = b->v.b_val;
        return (x > y) - (x < y);
    }
    case SQLRAM_FLOAT: {
        double x = a->v.d_val, y = b->v.d_val;
        return (x > y) - (x < y);
    }
    case SQLRAM_TIMESTAMP: {
        time_t x = a->v.t_val, y = b->v.t_val;
        return (x > y) - (x < y);
    }
    case SQLRAM_TEXT:
        return strcmp (a->v.s_val ? a->v.s_val : "", b->v.s_val ? b->v.s_val : "");
    default:
        return 0;
    }
}

static int cmp_satisfies (int c, CmpOp op) {
    switch (op) {
    case OP_EQ:
        return c == 0;
    case OP_NE:
        return c != 0;
    case OP_LT:
        return c < 0;
    case OP_LE:
        return c <= 0;
    case OP_GT:
        return c > 0;
    case OP_GE:
        return c >= 0;
    }
    return 0;
}

static int find_col (Table *t, const char *name) {
    for (int i = 0; i < t->numFields; i++) {
        if (!strcmp (name, t->fields[i]->fieldName)) {
            return i;
        }
    }
    return -1;
}

static int record_matches (Table *t, Record *r, int colIdx, CmpOp op, const sqlram_value *lit) {
    int c = value_cmp (t->fields[colIdx]->fieldType, &r->fields[colIdx], lit);
    return cmp_satisfies (c, op);
}

/* Parses an ISO-ish date/time string into a time_t (local time). */
static int ts_from_string (const char *s, time_t *out) {
    static const char *const formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%d %H:%M",
        "%Y-%m-%d",
    };
    struct tm tm;
    for (size_t i = 0; i < sizeof (formats) / sizeof (formats[0]); i++) {
        memset (&tm, 0, sizeof (tm));
        if (strptime (s, formats[i], &tm)) {
            tm.tm_isdst = -1;
            *out = mktime (&tm);
            return 1;
        }
    }
    return 0;
}

/* Converts a literal field to a column type in place. Returns 0 on
 * incompatible type. The old string (if any) is not freed: it is owned by
 * the arena (or the caller) and reclaimed there. */
static int field_coerce (Field *f, sqlram_type target) {
    if (f->type == target) {
        return 1;
    }

    switch (target) {
    case SQLRAM_FLOAT: {
        double d;
        if (f->type == SQLRAM_INT) {
            d = (double)f->v.i_val;
        } else if (f->type == SQLRAM_TEXT) {
            char *end;
            d = strtod (f->v.s_val, &end);
            if (end == f->v.s_val || *end != '\0') {
                return 0;
            }
        } else {
            return 0;
        }
        f->type = SQLRAM_FLOAT;
        f->v.d_val = d;
        return 1;
    }
    case SQLRAM_TIMESTAMP: {
        time_t t;
        if (f->type == SQLRAM_INT) {
            t = (time_t)f->v.i_val;
        } else if (f->type == SQLRAM_TEXT) {
            if (!ts_from_string (f->v.s_val, &t)) {
                return 0;
            }
        } else {
            return 0;
        }
        f->type = SQLRAM_TIMESTAMP;
        f->v.t_val = t;
        return 1;
    }
    default:
        return 0; /* INT, BOOL, TEXT literals must match exactly */
    }
}

static Table *sort_table;
static int sort_col;
static int sort_desc;

static int sort_cmp (const void *a, const void *b) {
    Record *ra = *(Record *const *)a;
    Record *rb = *(Record *const *)b;
    int c = value_cmp (sort_table->fields[sort_col]->fieldType, &ra->fields[sort_col], &rb->fields[sort_col]);
    return sort_desc ? -c : c;
}

int exec_insert (char *tblname, Field *values, int numValues) {
    Table *t = find_table (tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", tblname);
        return -1;
    }
    if (numValues != t->numFields) {
        sqlram_set_error ("expected %d values, got %d", t->numFields, numValues);
        return -1;
    }
    for (int i = 0; i < t->numFields; i++) {
        if (!field_coerce (&values[i], t->fields[i]->fieldType)) {
            sqlram_set_error ("type mismatch on column '%s'", t->fields[i]->fieldName);
            return -1;
        }
    }

    Record *r = malloc (sizeof (Record) + (size_t)t->numFields * sizeof (Field));
    if (!r) {
        sqlram_set_error ("out of memory");
        return -1;
    }
    r->fields = (Field *)(r + 1);
    for (int i = 0; i < t->numFields; i++) {
        r->fields[i] = value_dup (values[i]);
    }
    r->next = NULL;

    if (!t->records) {
        t->records = r;
        t->tail = r;
    } else {
        t->tail->next = r;
        t->tail = r;
    }
    return 0;
}

sqlram_result *exec_select (struct SelectS *sel) {
    Table *t = find_table (sel->tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", sel->tblname);
        return NULL;
    }

    int nproj;
    int *proj;
    if (sel->cols) {
        nproj = sel->numCols;
        proj = calloc (nproj, sizeof (int));
        if (!proj) {
            sqlram_set_error ("out of memory");
            return NULL;
        }
        for (int i = 0; i < nproj; i++) {
            int ci = find_col (t, sel->cols[i]);
            if (ci < 0) {
                sqlram_set_error ("unknown column '%s'", sel->cols[i]);
                free (proj);
                return NULL;
            }
            proj[i] = ci;
        }
    } else {
        nproj = t->numFields;
        proj = calloc (nproj, sizeof (int));
        if (!proj) {
            sqlram_set_error ("out of memory");
            return NULL;
        }
        for (int i = 0; i < nproj; i++) {
            proj[i] = i;
        }
    }

    int whereIdx = -1;
    if (sel->whereCol) {
        whereIdx = find_col (t, sel->whereCol);
        if (whereIdx < 0) {
            sqlram_set_error ("unknown column '%s'", sel->whereCol);
            free (proj);
            return NULL;
        }
        if (!field_coerce (&sel->whereVal, t->fields[whereIdx]->fieldType)) {
            sqlram_set_error ("type mismatch on column '%s'", sel->whereCol);
            free (proj);
            return NULL;
        }
    }

    int orderIdx = -1;
    if (sel->orderCol) {
        orderIdx = find_col (t, sel->orderCol);
        if (orderIdx < 0) {
            sqlram_set_error ("unknown column '%s'", sel->orderCol);
            free (proj);
            return NULL;
        }
    }

    Record **matches = NULL;
    int nmatch = 0;
    int mcap = 0;
    for (Record *r = t->records; r; r = r->next) {
        if (whereIdx >= 0 && !record_matches (t, r, whereIdx, sel->whereOp, &sel->whereVal)) {
            continue;
        }
        if (nmatch == mcap) {
            int new_cap = mcap ? mcap * 2 : 16;
            Record **tmp = realloc (matches, new_cap * sizeof (Record *));
            if (!tmp) {
                free (proj);
                free (matches);
                sqlram_set_error ("out of memory");
                return NULL;
            }
            matches = tmp;
            mcap = new_cap;
        }
        matches[nmatch++] = r;
    }

    if (orderIdx >= 0 && nmatch > 1) {
        sort_table = t;
        sort_col = orderIdx;
        sort_desc = sel->orderDesc;
        qsort (matches, nmatch, sizeof (Record *), sort_cmp);
    }

    if (sel->limit >= 0 && nmatch > sel->limit) {
        nmatch = sel->limit;
    }

    sqlram_result *res = result_new (nproj);
    if (!res) {
        free (proj);
        free (matches);
        return NULL;
    }
    for (int i = 0; i < nproj; i++) {
        result_set_col (res, i, t->fields[proj[i]]->fieldName);
    }

    for (int k = 0; k < nmatch; k++) {
        Record *r = matches[k];
        Field *vals = calloc (nproj, sizeof (Field));
        if (!vals) {
            continue;
        }
        for (int i = 0; i < nproj; i++) {
            vals[i] = r->fields[proj[i]];
        }
        result_add_row (res, vals, nproj);
        free (vals);
    }

    free (proj);
    free (matches);
    return res;
}

int exec_update (struct UpdateS *upd) {
    Table *t = find_table (upd->tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", upd->tblname);
        return -1;
    }

    int setIdx = find_col (t, upd->col);
    if (setIdx < 0) {
        sqlram_set_error ("unknown column '%s'", upd->col);
        return -1;
    }
    if (!field_coerce (&upd->val, t->fields[setIdx]->fieldType)) {
        sqlram_set_error ("type mismatch on column '%s'", upd->col);
        return -1;
    }

    int whereIdx = -1;
    if (upd->whereCol) {
        whereIdx = find_col (t, upd->whereCol);
        if (whereIdx < 0) {
            sqlram_set_error ("unknown column '%s'", upd->whereCol);
            return -1;
        }
        if (!field_coerce (&upd->whereVal, t->fields[whereIdx]->fieldType)) {
            sqlram_set_error ("type mismatch on column '%s'", upd->whereCol);
            return -1;
        }
    }

    int affected = 0;
    for (Record *r = t->records; r; r = r->next) {
        if (whereIdx >= 0 && !record_matches (t, r, whereIdx, upd->whereOp, &upd->whereVal)) {
            continue;
        }
        field_free (&r->fields[setIdx]);
        r->fields[setIdx] = value_dup (upd->val);
        affected++;
    }
    return affected;
}

int exec_delete (struct DeleteS *del) {
    Table *t = find_table (del->tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", del->tblname);
        return -1;
    }

    int whereIdx = -1;
    if (del->whereCol) {
        whereIdx = find_col (t, del->whereCol);
        if (whereIdx < 0) {
            sqlram_set_error ("unknown column '%s'", del->whereCol);
            return -1;
        }
        if (!field_coerce (&del->whereVal, t->fields[whereIdx]->fieldType)) {
            sqlram_set_error ("type mismatch on column '%s'", del->whereCol);
            return -1;
        }
    }

    int affected = 0;
    Record **pp = &t->records;
    while (*pp) {
        Record *r = *pp;
        if (whereIdx >= 0 && !record_matches (t, r, whereIdx, del->whereOp, &del->whereVal)) {
            pp = &r->next;
            continue;
        }
        *pp = r->next;
        record_free (t, r);
        affected++;
    }

    if (affected > 0) {
        if (!t->records) {
            t->tail = NULL;
        } else {
            Record *cur = t->records;
            while (cur->next) {
                cur = cur->next;
            }
            t->tail = cur;
        }
    }
    return affected;
}

int exec_truncate (char *tblname) {
    Table *t = find_table (tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", tblname);
        return -1;
    }

    int affected = 0;
    for (Record *r = t->records; r; r = r->next) {
        affected++;
    }
    record_free_all (t);
    return affected;
}
