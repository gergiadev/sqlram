#include "sqlram_internal.h"

extern DatabaseArray db_array;

Table *find_table (const char *name) {
    Database *db = current_db ();
    if (!db) {
        return NULL;
    }
    for (Table *t = db->tables; t; t = t->next) {
        if (!strcmp (name, t->name)) {
            return t;
        }
    }
    return NULL;
}

void table_free (Table *t) {
    if (!t) {
        return;
    }
    record_free_all (t);
    for (int i = 0; i < t->numFields; i++) {
        free (t->fields[i]->fieldName);
        free (t->fields[i]);
    }
    free (t->fields);
    free (t->name);
    free (t);
}

void table_free_list (Table *t) {
    while (t) {
        Table *next = t->next;
        table_free (t);
        t = next;
    }
}

int exec_create_table (char *tblname, TableFieldS **fields, int numFields) {
    Database *db = current_db ();
    if (!db) {
        sqlram_set_error ("no database in use");
        return -1;
    }
    if (find_table (tblname)) {
        sqlram_set_error ("table '%s' already exists", tblname);
        return -1;
    }

    Table *t = calloc (1, sizeof (Table));
    if (!t) {
        sqlram_set_error ("out of memory");
        return -1;
    }
    t->name = strdup (tblname);
    t->numFields = numFields;
    t->fields = calloc (numFields, sizeof (TableFieldS *));
    if (!t->fields) {
        sqlram_set_error ("out of memory");
        free (t->name);
        free (t);
        return -1;
    }
    for (int i = 0; i < numFields; i++) {
        t->fields[i] = calloc (1, sizeof (TableFieldS));
        t->fields[i]->fieldName = strdup (fields[i]->fieldName);
        t->fields[i]->fieldType = fields[i]->fieldType;
    }
    t->records = NULL;
    t->tail = NULL;
    t->next = NULL;

    if (!db->tables) {
        db->tables = t;
        db->tail = t;
    } else {
        db->tail->next = t;
        db->tail = t;
    }
    return 0;
}

sqlram_result *exec_show_tables (void) {
    Database *db = current_db ();
    if (!db) {
        sqlram_set_error ("no database in use");
        return NULL;
    }

    sqlram_result *r = result_new (1);
    if (!r) {
        return NULL;
    }
    result_set_col (r, 0, "table");

    for (Table *t = db->tables; t; t = t->next) {
        Field v = {0};
        v.type = SQLRAM_TEXT;
        v.v.s_val = t->name;
        result_add_row (r, &v, 1);
    }
    return r;
}

int exec_drop_table (char *tblname) {
    Database *db = current_db ();
    if (!db) {
        sqlram_set_error ("no database in use");
        return -1;
    }

    Table **pp = &db->tables;
    while (*pp) {
        if (!strcmp ((*pp)->name, tblname)) {
            Table *victim = *pp;
            *pp = victim->next;
            table_free (victim);

            if (!db->tables) {
                db->tail = NULL;
            } else {
                Table *cur = db->tables;
                while (cur->next) {
                    cur = cur->next;
                }
                db->tail = cur;
            }
            return 0;
        }
        pp = &(*pp)->next;
    }
    sqlram_set_error ("table '%s' not found", tblname);
    return -1;
}
