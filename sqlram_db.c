#include "sqlram_internal.h"

extern DatabaseArray db_array;

int exec_create_database (char *dbname) {
    for (size_t i = 0; i < db_array.count; i++) {
        if (!strcmp (dbname, db_array.dbs[i].name)) {
            sqlram_set_error ("database '%s' already exists", dbname);
            return -1;
        }
    }

    if (db_array.count == db_array.capacity) {
        size_t new_cap = db_array.capacity ? db_array.capacity * 2 : 4;
        Database *tmp = realloc (db_array.dbs, new_cap * sizeof (Database));
        if (!tmp) {
            sqlram_set_error ("out of memory");
            return -1;
        }
        db_array.dbs = tmp;
        db_array.capacity = new_cap;
    }

    db_array.dbs[db_array.count].name = strdup (dbname);
    db_array.dbs[db_array.count].tables = NULL;
    db_array.dbs[db_array.count].tail = NULL;
    db_array.count++;
    return 0;
}

sqlram_result *exec_show_databases (void) {
    sqlram_result *r = result_new (1);
    if (!r) {
        return NULL;
    }
    result_set_col (r, 0, "database");

    for (size_t i = 0; i < db_array.count; i++) {
        Field v = {0};
        v.type = SQLRAM_TEXT;
        v.v.s_val = db_array.dbs[i].name;
        result_add_row (r, &v, 1);
    }
    return r;
}

int exec_use_database (char *dbname) {
    for (size_t i = 0; i < db_array.count; i++) {
        if (!strcmp (dbname, db_array.dbs[i].name)) {
            db_array.usedIdx = (int)i;
            return 0;
        }
    }
    sqlram_set_error ("database '%s' not found", dbname);
    return -1;
}

int exec_drop_database (char *dbname) {
    for (size_t i = 0; i < db_array.count; i++) {
        if (!strcmp (dbname, db_array.dbs[i].name)) {
            Database *db = &db_array.dbs[i];
            table_free_list (db->tables);
            free (db->name);

            db_array.dbs[i] = db_array.dbs[db_array.count - 1];
            db_array.count--;
            db_array.usedIdx = -1;
            return 0;
        }
    }
    sqlram_set_error ("database '%s' not found", dbname);
    return -1;
}

Database *current_db (void) {
    if (db_array.usedIdx < 0 || (size_t)db_array.usedIdx >= db_array.count) {
        return NULL;
    }
    return &db_array.dbs[db_array.usedIdx];
}
