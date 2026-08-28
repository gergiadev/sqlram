#define _XOPEN_SOURCE 700

#include "sqlram_internal.h"

static void index_drop (Table *t);

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
    index_drop (t);
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

/* ---- Hash index over the ON CONFLICT key columns ----------------------
 *
 * Built lazily by the upsert path so that a bulk upsert costs O(1) per row
 * instead of a linear scan. Only one key set is cached per table: an upsert
 * naming different columns rebuilds it.
 *
 * The hashes below must agree with value_cmp(): two values that compare equal
 * have to land in the same bucket. */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t hash_bytes (const void *p, size_t n, uint64_t h) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t hash_field (sqlram_type ty, const Field *f, uint64_t h) {
    switch (ty) {
    case SQLRAM_INT: {
        long v = f->v.i_val;
        return hash_bytes (&v, sizeof (v), h);
    }
    case SQLRAM_BOOL: {
        unsigned char v = f->v.b_val ? 1 : 0;
        return hash_bytes (&v, sizeof (v), h);
    }
    case SQLRAM_FLOAT: {
        double v = f->v.d_val;
        if (v != v) {
            /* NaN: value_cmp() reports it equal to everything, which is not an
             * equivalence relation, so no hash can be consistent with it. All
             * NaNs at least share a bucket. A float is a poor key anyway. */
            unsigned char nan_tag = 0xA5;
            return hash_bytes (&nan_tag, sizeof (nan_tag), h);
        }
        if (v == 0.0) {
            v = 0.0; /* -0.0 and 0.0 compare equal */
        }
        return hash_bytes (&v, sizeof (v), h);
    }
    case SQLRAM_TIMESTAMP: {
        time_t v = f->v.t_val;
        return hash_bytes (&v, sizeof (v), h);
    }
    case SQLRAM_TEXT: {
        const char *v = f->v.s_val ? f->v.s_val : ""; /* as in value_cmp() */
        return hash_bytes (v, strlen (v), h);
    }
    }
    return h;
}

static uint64_t hash_key (Table *t, const int *cols, int n, const Field *base) {
    uint64_t h = FNV_OFFSET;
    for (int i = 0; i < n; i++) {
        h = hash_field (t->fields[cols[i]]->fieldType, &base[cols[i]], h);
        h ^= 0xff; /* separator, so { "ab", "c" } and { "a", "bc" } differ */
        h *= FNV_PRIME;
    }
    return h;
}

static int key_equal (Table *t, const int *cols, int n, const Field *a, const Field *b) {
    for (int i = 0; i < n; i++) {
        if (value_cmp (t->fields[cols[i]]->fieldType, &a[cols[i]], &b[cols[i]]) != 0) {
            return 0;
        }
    }
    return 1;
}

static void index_drop (Table *t) {
    free (t->keyBuckets);
    free (t->keyCols);
    t->keyBuckets = NULL;
    t->keyBucketCap = 0;
    t->keyCount = 0;
    t->keyCols = NULL;
    t->numKeyCols = 0;
}

/* Appends to the tail of the bucket rather than the head. Nothing stops a
 * plain INSERT from creating duplicate keys, so chain order is made to follow
 * insertion order: a lookup then deterministically finds the oldest row. */
static void index_bucket_put (Record **buckets, size_t cap, Table *t, Record *r) {
    size_t b = (size_t)(hash_key (t, t->keyCols, t->numKeyCols, r->fields) & (cap - 1));
    r->hnext = NULL;
    if (!buckets[b]) {
        buckets[b] = r;
        return;
    }
    Record *cur = buckets[b];
    while (cur->hnext) {
        cur = cur->hnext;
    }
    cur->hnext = r;
}

/* Refills the buckets from the record list. Requires t->keyCols to be set. */
static int index_rehash (Table *t, size_t cap) {
    Record **buckets = calloc (cap, sizeof (Record *));
    if (!buckets) {
        return 0;
    }
    size_t n = 0;
    for (Record *r = t->records; r; r = r->next) {
        index_bucket_put (buckets, cap, t, r);
        n++;
    }
    free (t->keyBuckets);
    t->keyBuckets = buckets;
    t->keyBucketCap = cap;
    t->keyCount = n;
    return 1;
}

static int index_ensure (Table *t, const int *cols, int n) {
    if (t->keyBuckets && t->numKeyCols == n && !memcmp (t->keyCols, cols, (size_t)n * sizeof (int))) {
        return 1;
    }
    index_drop (t);

    t->keyCols = malloc ((size_t)n * sizeof (int));
    if (!t->keyCols) {
        return 0;
    }
    memcpy (t->keyCols, cols, (size_t)n * sizeof (int));
    t->numKeyCols = n;

    size_t rows = 0;
    for (Record *r = t->records; r; r = r->next) {
        rows++;
    }
    size_t cap = 16;
    while (cap < rows * 2) {
        cap *= 2;
    }
    if (!index_rehash (t, cap)) {
        index_drop (t);
        return 0;
    }
    return 1;
}

/* Called after r has been linked into the record list. */
static void index_add (Table *t, Record *r) {
    if (!t->keyBuckets) {
        return;
    }
    if (t->keyCount + 1 > t->keyBucketCap - (t->keyBucketCap >> 2)) {
        /* Load factor above 0.75. index_rehash() walks the record list, which
         * already holds r, so it picks it up on the way. */
        if (!index_rehash (t, t->keyBucketCap * 2)) {
            index_drop (t); /* lose the index rather than fail the statement */
        }
        return;
    }
    index_bucket_put (t->keyBuckets, t->keyBucketCap, t, r);
    t->keyCount++;
}

static Record *index_lookup (Table *t, const Field *values) {
    size_t b = (size_t)(hash_key (t, t->keyCols, t->numKeyCols, values) & (t->keyBucketCap - 1));
    for (Record *r = t->keyBuckets[b]; r; r = r->hnext) {
        if (key_equal (t, t->keyCols, t->numKeyCols, r->fields, values)) {
            return r;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */

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

/* Resolves the ON CONFLICT column names to column indices. */
static int resolve_key (Table *t, char **names, int n, int *out) {
    for (int i = 0; i < n; i++) {
        out[i] = find_col (t, names[i]);
        if (out[i] < 0) {
            sqlram_set_error ("unknown column '%s'", names[i]);
            return 0;
        }
        for (int j = 0; j < i; j++) {
            if (out[j] == out[i]) {
                sqlram_set_error ("duplicate conflict column '%s'", names[i]);
                return 0;
            }
        }
    }
    return 1;
}

static int insert_or_upsert (char *tblname, Field *values, int numValues, char **conflictCols, int numConflictCols) {
    Table *t = find_table (tblname);
    if (!t) {
        sqlram_set_error ("table '%s' not found", tblname);
        return -1;
    }
    if (numValues != t->numFields) {
        sqlram_set_error ("expected %d values, got %d", t->numFields, numValues);
        return -1;
    }
    /* Coercion has to happen before the key is hashed, so that an INT literal
     * probing a FLOAT key column matches the stored value. */
    for (int i = 0; i < t->numFields; i++) {
        if (!field_coerce (&values[i], t->fields[i]->fieldType)) {
            sqlram_set_error ("type mismatch on column '%s'", t->fields[i]->fieldName);
            return -1;
        }
    }

    if (numConflictCols > 0) {
        int *cols = malloc ((size_t)numConflictCols * sizeof (int));
        if (!cols) {
            sqlram_set_error ("out of memory");
            return -1;
        }
        if (!resolve_key (t, conflictCols, numConflictCols, cols)) {
            free (cols);
            return -1;
        }
        if (!index_ensure (t, cols, numConflictCols)) {
            sqlram_set_error ("out of memory");
            free (cols);
            return -1;
        }
        free (cols);

        Record *hit = index_lookup (t, values);
        if (hit) {
            /* Overwrite the whole row in place: the record keeps its position
             * in the list, so SELECT * ordering is unchanged, and the key
             * columns are rewritten with equal values, so the index stays
             * valid. */
            for (int i = 0; i < t->numFields; i++) {
                field_free (&hit->fields[i]);
                hit->fields[i] = value_dup (values[i]);
            }
            return 0;
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
    r->hnext = NULL;

    if (!t->records) {
        t->records = r;
        t->tail = r;
    } else {
        t->tail->next = r;
        t->tail = r;
    }
    index_add (t, r);
    return 0;
}

int exec_insert (char *tblname, Field *values, int numValues) {
    return insert_or_upsert (tblname, values, numValues, NULL, 0);
}

int exec_upsert (char *tblname, Field *values, int numValues, char **conflictCols, int numConflictCols) {
    return insert_or_upsert (tblname, values, numValues, conflictCols, numConflictCols);
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

    /* Fields change in place and may belong to the cached key. */
    index_drop (t);

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

    /* Records are about to be unlinked and freed. */
    index_drop (t);

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
