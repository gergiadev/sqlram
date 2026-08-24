#include "sqlram_internal.h"

static sqlram_result *empty_ok (void) {
    return result_new (0);
}

/* Routes NodeK to the matching domain function; returns a result
 * set or NULL on error (message via sqlram_set_error()). */
sqlram_result *exec_dispatch (Node *node) {
    if (!node) {
        return NULL;
    }

    switch (node->Nkind) {
    case NODE_INVALID:
        sqlram_set_error ("invalid statement: %s", node->pos ? node->pos : "");
        return NULL;

    case NODE_CREATE_DATABASE:
        if (exec_create_database (node->nodeAST.CreateDatabase.dbname) < 0) {
            return NULL;
        }
        return empty_ok ();

    case NODE_CREATE_TABLE:
        if (exec_create_table (node->nodeAST.CreateTable.tblname, node->nodeAST.CreateTable.fields, node->nodeAST.CreateTable.numFields) < 0) {
            return NULL;
        }
        return empty_ok ();

    case NODE_USE_DATABASE:
        if (exec_use_database (node->nodeAST.UseDatabase.dbname) < 0) {
            return NULL;
        }
        return empty_ok ();

    case NODE_SHOW_DATABASES:
        return exec_show_databases ();

    case NODE_SHOW_TABLES:
        return exec_show_tables ();

    case NODE_INSERT: {
        if (exec_insert (node->nodeAST.Insert.tblname, node->nodeAST.Insert.values, node->nodeAST.Insert.numValues) < 0) {
            return NULL;
        }
        sqlram_result *r = empty_ok ();
        if (r) {
            r->affected = 1;
        }
        return r;
    }

    case NODE_SELECT:
        return exec_select (&node->nodeAST.Select);

    case NODE_UPDATE: {
        int n = exec_update (&node->nodeAST.Update);
        if (n < 0) {
            return NULL;
        }
        sqlram_result *r = empty_ok ();
        if (r) {
            r->affected = n;
        }
        return r;
    }

    case NODE_DELETE: {
        int n = exec_delete (&node->nodeAST.Delete);
        if (n < 0) {
            return NULL;
        }
        sqlram_result *r = empty_ok ();
        if (r) {
            r->affected = n;
        }
        return r;
    }

    case NODE_DROP_TABLE:
        if (exec_drop_table (node->nodeAST.Drop.name) < 0) {
            return NULL;
        }
        return empty_ok ();

    case NODE_DROP_DATABASE:
        if (exec_drop_database (node->nodeAST.Drop.name) < 0) {
            return NULL;
        }
        return empty_ok ();

    case NODE_TRUNCATE: {
        int n = exec_truncate (node->nodeAST.Truncate.tblname);
        if (n < 0) {
            return NULL;
        }
        sqlram_result *r = empty_ok ();
        if (r) {
            r->affected = n;
        }
        return r;
    }
    }
    return NULL;
}
