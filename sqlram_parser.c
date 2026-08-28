#include "sqlram_internal.h"

static Arena *g_arena;

static void *acalloc (size_t n) {
    return arena_calloc (g_arena, n);
}
static char *astrdup (const char *s) {
    return arena_strdup (g_arena, s);
}
static void *arealloc (void *p, size_t old_sz, size_t new_sz) {
    return arena_realloc (g_arena, p, old_sz, new_sz);
}

static void advance (Token **t) {
    if (*t && (*t)->next_token) {
        *t = (*t)->next_token;
    }
}

void field_free (Field *f) {
    if (!f) {
        return;
    }
    if (f->type == SQLRAM_TEXT) {
        free (f->v.s_val);
        f->v.s_val = NULL;
    }
}

/* Keyword tokens keep their source text in Tvalue (make_kw_token), so the
 * words introduced for the upsert clause can still be used as identifiers.
 * Without this, adding "on" to the lexer would break any column named 'on'. */
static int tok_is_ident (Token *t) {
    return t && (t->Tkind == TK_STRING || t->Tkind == TK_ON || t->Tkind == TK_CONFLICT);
}

static int tok_to_cmpop (TokenK k, CmpOp *op) {
    switch (k) {
    case TK_EQ:
        *op = OP_EQ;
        return 1;
    case TK_NE:
        *op = OP_NE;
        return 1;
    case TK_LT:
        *op = OP_LT;
        return 1;
    case TK_LE:
        *op = OP_LE;
        return 1;
    case TK_GT:
        *op = OP_GT;
        return 1;
    case TK_GE:
        *op = OP_GE;
        return 1;
    default:
        return 0;
    }
}

static int parse_value (Token **t, Field *out) {
    Token *tk = *t;
    if (!tk) {
        return 0;
    }
    switch (tk->Tkind) {
    case TK_NUMBER:
        out->type = SQLRAM_INT;
        out->v.i_val = strtol (tk->Tvalue, NULL, 10);
        break;
    case TK_TRUE:
        out->type = SQLRAM_BOOL;
        out->v.b_val = 1;
        break;
    case TK_FALSE:
        out->type = SQLRAM_BOOL;
        out->v.b_val = 0;
        break;
    case TK_FLOAT:
        out->type = SQLRAM_FLOAT;
        out->v.d_val = strtod (tk->Tvalue, NULL);
        break;
    case TK_STRING:
        out->type = SQLRAM_TEXT;
        out->v.s_val = astrdup (tk->Tvalue);
        break;
    default:
        return 0;
    }
    advance (t);
    return 1;
}

/* Parses "WHERE col op val". */
static void parse_where (Token **t, char **whereCol, CmpOp *op, Field *val) {
    advance (t);
    if (!tok_is_ident (*t)) {
        return;
    }
    *whereCol = astrdup ((*t)->Tvalue);
    advance (t);
    if (!*t || !tok_to_cmpop ((*t)->Tkind, op)) {
        return;
    }
    advance (t);
    if (!*t) {
        return;
    }
    parse_value (t, val);
}

static void parse_orderby (Token **t, char **orderCol, int *desc) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_BY) {
        return;
    }
    advance (t);
    if (!tok_is_ident (*t)) {
        return;
    }
    *orderCol = astrdup ((*t)->Tvalue);
    advance (t);
    *desc = 0;
    if (*t && (*t)->Tkind == TK_ASC) {
        advance (t);
    } else if (*t && (*t)->Tkind == TK_DESC) {
        *desc = 1;
        advance (t);
    }
}

static void parse_limit (Token **t, int *limit) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_NUMBER) {
        return;
    }
    *limit = atoi ((*t)->Tvalue);
    advance (t);
}

/* Parses "type name" pairs separated by commas up to '}'.
 * Returns an arena-owned array of TableFieldS*, or NULL on error. */
static TableFieldS **parse_columns (Token **t, int *numFields) {
    TableFieldS **fields = NULL;
    int n = 0;

    while (*t && (*t)->Tkind != TK_END_CURLY_BRACKET && (*t)->Tkind != TK_END) {
        if ((*t)->Tkind == TK_COMMA) {
            advance (t);
            continue;
        }

        tableField type;
        if ((*t)->Tkind == TK_TYPE_INT) {
            type = SQLRAM_INT;
        } else if ((*t)->Tkind == TK_TYPE_BOOL) {
            type = SQLRAM_BOOL;
        } else if ((*t)->Tkind == TK_TYPE_TEXT) {
            type = SQLRAM_TEXT;
        } else if ((*t)->Tkind == TK_TYPE_FLOAT) {
            type = SQLRAM_FLOAT;
        } else if ((*t)->Tkind == TK_TYPE_TIMESTAMP) {
            type = SQLRAM_TIMESTAMP;
        } else {
            return NULL;
        }

        advance (t);
        if (!tok_is_ident (*t)) {
            return NULL;
        }

        TableFieldS *col = acalloc (sizeof (TableFieldS));
        if (!col) {
            return NULL;
        }
        col->fieldName = astrdup ((*t)->Tvalue);
        col->fieldType = type;

        TableFieldS **tmp = arealloc (fields, n * sizeof (TableFieldS *), (n + 1) * sizeof (TableFieldS *));
        if (!tmp) {
            return NULL;
        }
        fields = tmp;
        fields[n++] = col;
        advance (t);
    }

    if (*t && (*t)->Tkind == TK_END_CURLY_BRACKET) {
        advance (t);
    }
    *numFields = n;
    return fields;
}

static void parse_create (Token **t, Node *node) {
    advance (t);
    if (!*t) {
        return;
    }

    if ((*t)->Tkind == TK_DB) {
        advance (t);
        if (!*t || (*t)->Tkind != TK_STRING) {
            return;
        }
        node->Nkind = NODE_CREATE_DATABASE;
        node->nodeAST.CreateDatabase.dbname = astrdup ((*t)->Tvalue);
        return;
    }

    if ((*t)->Tkind == TK_TBL) {
        advance (t);
        if (!*t || (*t)->Tkind != TK_STRING) {
            return;
        }
        char *tblname = astrdup ((*t)->Tvalue);
        advance (t);

        if (!*t || (*t)->Tkind != TK_START_CURLY_BRACKET) {
            node->pos = *t ? (*t)->Tpos : node->pos;
            return;
        }
        advance (t);

        int numFields = 0;
        TableFieldS **fields = parse_columns (t, &numFields);
        if (!fields) {
            return;
        }

        node->Nkind = NODE_CREATE_TABLE;
        node->nodeAST.CreateTable.tblname = tblname;
        node->nodeAST.CreateTable.fields = fields;
        node->nodeAST.CreateTable.numFields = numFields;
        return;
    }

    node->pos = (*t)->Tpos;
}

static void parse_show (Token **t, Node *node) {
    advance (t);
    if (!*t) {
        return;
    }
    if ((*t)->Tkind == TK_DBS) {
        node->Nkind = NODE_SHOW_DATABASES;
    } else if ((*t)->Tkind == TK_TBLS) {
        node->Nkind = NODE_SHOW_TABLES;
    } else {
        node->pos = (*t)->Tpos;
    }
}

static void parse_use (Token **t, Node *node) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }
    node->Nkind = NODE_USE_DATABASE;
    node->nodeAST.UseDatabase.dbname = astrdup ((*t)->Tvalue);
}

static void parse_insert (Token **t, Node *node) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_INTO) {
        return;
    }
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }
    char *tblname = astrdup ((*t)->Tvalue);
    advance (t);
    if (!*t || (*t)->Tkind != TK_START_CURLY_BRACKET) {
        return;
    }
    advance (t);

    int cap = 4;
    int num = 0;
    int nparam = 0;
    Field *values = acalloc (cap * sizeof (Field));
    int *params = acalloc (cap * sizeof (int));
    if (!values || !params) {
        return;
    }

    while (*t && (*t)->Tkind != TK_END_CURLY_BRACKET && (*t)->Tkind != TK_END) {
        if (num >= cap) {
            int old_cap = cap;
            cap *= 2;
            values = arealloc (values, old_cap * sizeof (Field), cap * sizeof (Field));
            params = arealloc (params, old_cap * sizeof (int), cap * sizeof (int));
            if (!values || !params) {
                return;
            }
        }
        if ((*t)->Tkind == TK_PARAM) {
            params[num] = nparam++;
            advance (t);
        } else {
            if (!parse_value (t, &values[num])) {
                return;
            }
            params[num] = -1;
        }
        num++;
        if (*t && (*t)->Tkind == TK_COMMA) {
            advance (t);
        }
    }

    if (*t && (*t)->Tkind == TK_END_CURLY_BRACKET) {
        advance (t);
    }

    /* Optional "ON CONFLICT { col, ... } UPDATE". Note that a plain INSERT
     * historically ignored whatever followed the value list, so accepting this
     * clause cannot break a statement that used to mean something else. */
    char **conflictCols = NULL;
    int nconflict = 0;

    if (*t && (*t)->Tkind == TK_ON) {
        advance (t);
        if (!*t || (*t)->Tkind != TK_CONFLICT) {
            return;
        }
        advance (t);
        if (!*t || (*t)->Tkind != TK_START_CURLY_BRACKET) {
            return;
        }
        advance (t);

        while (tok_is_ident (*t)) {
            char **tmp = arealloc (conflictCols, nconflict * sizeof (char *), (nconflict + 1) * sizeof (char *));
            if (!tmp) {
                return;
            }
            conflictCols = tmp;
            conflictCols[nconflict++] = astrdup ((*t)->Tvalue);
            advance (t);
            if (*t && (*t)->Tkind == TK_COMMA) {
                advance (t);
                continue;
            }
            break;
        }

        if (nconflict == 0) {
            return;
        }
        if (!*t || (*t)->Tkind != TK_END_CURLY_BRACKET) {
            return;
        }
        advance (t);
        if (!*t || (*t)->Tkind != TK_UPDATE) {
            return;
        }
        advance (t);
    }

    node->Nkind = NODE_INSERT;
    node->nodeAST.Insert.tblname = tblname;
    node->nodeAST.Insert.values = values;
    node->nodeAST.Insert.numValues = num;
    node->nodeAST.Insert.param = params;
    node->nodeAST.Insert.numParams = nparam;
    node->nodeAST.Insert.conflictCols = conflictCols;
    node->nodeAST.Insert.numConflictCols = nconflict;
}

static void parse_select (Token **t, Node *node) {
    advance (t);

    char **cols = NULL;
    int numCols = 0;
    if (*t && (*t)->Tkind == TK_STAR) {
        advance (t);
    } else {
        while (tok_is_ident (*t)) {
            char **tmp = arealloc (cols, numCols * sizeof (char *), (numCols + 1) * sizeof (char *));
            if (!tmp) {
                return;
            }
            cols = tmp;
            cols[numCols++] = astrdup ((*t)->Tvalue);
            advance (t);
            if (*t && (*t)->Tkind == TK_COMMA) {
                advance (t);
                continue;
            }
            break;
        }
        if (numCols == 0) {
            return;
        }
    }

    if (!*t || (*t)->Tkind != TK_FROM) {
        return;
    }
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }
    char *tblname = astrdup ((*t)->Tvalue);
    advance (t);

    node->Nkind = NODE_SELECT;
    node->nodeAST.Select.tblname = tblname;
    node->nodeAST.Select.cols = cols;
    node->nodeAST.Select.numCols = numCols;
    node->nodeAST.Select.whereCol = NULL;
    node->nodeAST.Select.orderCol = NULL;
    node->nodeAST.Select.orderDesc = 0;
    node->nodeAST.Select.limit = -1;
    node->nodeAST.Select.whereVal.type = SQLRAM_INT;
    node->nodeAST.Select.whereVal.v.i_val = 0;

    if (*t && (*t)->Tkind == TK_WHERE) {
        parse_where (t, &node->nodeAST.Select.whereCol, &node->nodeAST.Select.whereOp, &node->nodeAST.Select.whereVal);
    }
    if (*t && (*t)->Tkind == TK_ORDER) {
        parse_orderby (t, &node->nodeAST.Select.orderCol, &node->nodeAST.Select.orderDesc);
    }
    if (*t && (*t)->Tkind == TK_LIMIT) {
        parse_limit (t, &node->nodeAST.Select.limit);
    }
}

static void parse_update (Token **t, Node *node) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }
    char *tblname = astrdup ((*t)->Tvalue);
    advance (t);

    if (!*t || (*t)->Tkind != TK_SET) {
        return;
    }
    advance (t);
    if (!tok_is_ident (*t)) {
        return;
    }
    char *col = astrdup ((*t)->Tvalue);
    advance (t);

    if (!*t || (*t)->Tkind != TK_EQ) {
        return;
    }
    advance (t);

    Field val;
    if (!parse_value (t, &val)) {
        return;
    }

    node->Nkind = NODE_UPDATE;
    node->nodeAST.Update.tblname = tblname;
    node->nodeAST.Update.col = col;
    node->nodeAST.Update.val = val;
    node->nodeAST.Update.whereCol = NULL;
    node->nodeAST.Update.whereVal.type = SQLRAM_INT;
    node->nodeAST.Update.whereVal.v.i_val = 0;

    if (*t && (*t)->Tkind == TK_WHERE) {
        parse_where (t, &node->nodeAST.Update.whereCol, &node->nodeAST.Update.whereOp, &node->nodeAST.Update.whereVal);
    }
}

static void parse_delete (Token **t, Node *node) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_FROM) {
        return;
    }
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }

    node->Nkind = NODE_DELETE;
    node->nodeAST.Delete.tblname = astrdup ((*t)->Tvalue);
    advance (t);
    node->nodeAST.Delete.whereCol = NULL;
    node->nodeAST.Delete.whereVal.type = SQLRAM_INT;
    node->nodeAST.Delete.whereVal.v.i_val = 0;

    if (*t && (*t)->Tkind == TK_WHERE) {
        parse_where (t, &node->nodeAST.Delete.whereCol, &node->nodeAST.Delete.whereOp, &node->nodeAST.Delete.whereVal);
    }
}

static void parse_drop (Token **t, Node *node) {
    advance (t);
    if (!*t) {
        return;
    }

    if ((*t)->Tkind == TK_TBL) {
        advance (t);
        if (!*t || (*t)->Tkind != TK_STRING) {
            return;
        }
        node->Nkind = NODE_DROP_TABLE;
        node->nodeAST.Drop.isTable = 1;
        node->nodeAST.Drop.name = astrdup ((*t)->Tvalue);
    } else if ((*t)->Tkind == TK_DB) {
        advance (t);
        if (!*t || (*t)->Tkind != TK_STRING) {
            return;
        }
        node->Nkind = NODE_DROP_DATABASE;
        node->nodeAST.Drop.isTable = 0;
        node->nodeAST.Drop.name = astrdup ((*t)->Tvalue);
    } else {
        node->pos = (*t)->Tpos;
    }
}

static void parse_truncate (Token **t, Node *node) {
    advance (t);
    if (!*t || (*t)->Tkind != TK_STRING) {
        return;
    }
    node->Nkind = NODE_TRUNCATE;
    node->nodeAST.Truncate.tblname = astrdup ((*t)->Tvalue);
}

Node *parser (Arena *a, Token *tkList) {
    g_arena = a;

    if (!tkList) {
        return NULL;
    }

    Node *node = acalloc (sizeof (Node));
    if (!node) {
        return NULL;
    }
    node->Nkind = NODE_INVALID;
    node->pos = tkList->Tpos;

    Token *tk = tkList;

    switch (tk->Tkind) {
    case TK_CREATE:
        parse_create (&tk, node);
        break;
    case TK_SHOW:
        parse_show (&tk, node);
        break;
    case TK_USE:
        parse_use (&tk, node);
        break;
    case TK_INSERT:
        parse_insert (&tk, node);
        break;
    case TK_SELECT:
        parse_select (&tk, node);
        break;
    case TK_UPDATE:
        parse_update (&tk, node);
        break;
    case TK_DELETE:
        parse_delete (&tk, node);
        break;
    case TK_DROP:
        parse_drop (&tk, node);
        break;
    case TK_TRUNCATE:
        parse_truncate (&tk, node);
        break;
    default:
        node->Nkind = NODE_INVALID;
        node->pos = tk->Tpos;
        break;
    }

    return node;
}
