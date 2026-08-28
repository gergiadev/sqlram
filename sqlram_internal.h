#ifndef SQLRAM_INTERNAL_H
#define SQLRAM_INTERNAL_H

#include "sqlram.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>

#define CONTEXT_SIZE 65536

typedef struct Arena {
    char  *buf;
    size_t used;
    size_t cap;
} Arena;

void   arena_init(Arena *a);
void  *arena_alloc(Arena *a, size_t size);
void  *arena_calloc(Arena *a, size_t size);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, size_t n);
void  *arena_realloc(Arena *a, void *ptr, size_t old_size, size_t new_size);
void   arena_reset(Arena *a);
void   arena_free(Arena *a);

typedef sqlram_value Field;
typedef sqlram_type  tableField;

typedef struct TableFieldS {
    char       *fieldName;
    tableField  fieldType;
} TableFieldS;

typedef struct Record {
    Field         *fields;
    struct Record *next;
    struct Record *hnext; /* next in the same hash bucket (see Table.keyBuckets) */
} Record;

/* A table caches one hash index, built over the key columns of the last
 * "ON CONFLICT { ... }" clause seen for it. keyBuckets == NULL means no index
 * is live; it is dropped whenever records move or change (UPDATE, DELETE,
 * TRUNCATE) and rebuilt lazily on the next upsert. */
typedef struct Table {
    char          *name;
    int            numFields;
    TableFieldS  **fields;
    Record        *records;
    Record        *tail;
    struct Table  *next;

    Record       **keyBuckets;
    size_t         keyBucketCap; /* always a power of two */
    size_t         keyCount;
    int           *keyCols;
    int            numKeyCols;
} Table;

typedef struct Database {
    char  *name;
    Table *tables;
    Table *tail;
} Database;

typedef struct {
    Database *dbs;
    size_t    count;
    size_t    capacity;
    int       usedIdx;
} DatabaseArray;

extern DatabaseArray db_array;

typedef enum {
    NODE_CREATE_DATABASE,
    NODE_SHOW_DATABASES,
    NODE_SHOW_TABLES,
    NODE_CREATE_TABLE,
    NODE_USE_DATABASE,
    NODE_INSERT,
    NODE_SELECT,
    NODE_UPDATE,
    NODE_DELETE,
    NODE_DROP_TABLE,
    NODE_DROP_DATABASE,
    NODE_TRUNCATE,
    NODE_INVALID
} NodeK;

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE
} CmpOp;

struct CreateDatabaseS {
    char *dbname;
};

struct UseDatabaseS {
    char *dbname;
};

struct CreateTableS {
    char *tblname;
    int   numFields;
    TableFieldS **fields;
};

struct InsertS {
    char  *tblname;
    Field *values;
    int    numValues;
    int   *param;      /* param[i] = bind index for value i, or -1 */
    int    numParams;
    char **conflictCols; /* ON CONFLICT key columns, NULL for a plain INSERT */
    int    numConflictCols;
};

struct SelectS {
    char  *tblname;
    char **cols;
    int    numCols;
    char  *whereCol;
    CmpOp  whereOp;
    Field  whereVal;
    char  *orderCol;
    int    orderDesc;
    int    limit;
};

struct UpdateS {
    char  *tblname;
    char  *col;
    Field  val;
    char  *whereCol;
    CmpOp  whereOp;
    Field  whereVal;
};

struct DeleteS {
    char  *tblname;
    char  *whereCol;
    CmpOp  whereOp;
    Field  whereVal;
};

struct DropS {
    int   isTable;
    char *name;
};

struct TruncateS {
    char *tblname;
};

typedef struct Node {
    NodeK Nkind;

    union {
        struct CreateDatabaseS CreateDatabase;
        struct UseDatabaseS     UseDatabase;
        struct CreateTableS     CreateTable;
        struct InsertS          Insert;
        struct SelectS          Select;
        struct UpdateS          Update;
        struct DeleteS          Delete;
        struct DropS            Drop;
        struct TruncateS        Truncate;
    } nodeAST;

    char *pos;
} Node;

typedef enum {
    TK_CREATE,
    TK_SHOW,
    TK_DROP,
    TK_STRING,
    TK_ID,
    TK_DB,
    TK_DBS,
    TK_TBL,
    TK_TBLS,
    TK_TYPE_INT,
    TK_TYPE_TEXT,
    TK_TYPE_BOOL,
    TK_TYPE_FLOAT,
    TK_TYPE_TIMESTAMP,
    TK_START_CURLY_BRACKET,
    TK_END_CURLY_BRACKET,
    TK_COMMA,
    TK_USE,
    TK_INSERT,
    TK_INTO,
    TK_ON,
    TK_CONFLICT,
    TK_SELECT,
    TK_FROM,
    TK_UPDATE,
    TK_SET,
    TK_WHERE,
    TK_DELETE,
    TK_ORDER,
    TK_BY,
    TK_ASC,
    TK_DESC,
    TK_LIMIT,
    TK_EQ,
    TK_NE,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,
    TK_TRUNCATE,
    TK_STAR,
    TK_NUMBER,
    TK_FLOAT,
    TK_TRUE,
    TK_FALSE,
    TK_PARAM,
    TK_END
} TokenK;

typedef struct Token {
    TokenK Tkind;
    char  *Tvalue;
    int    TLen;
    char  *Tpos;
    struct Token *next_token;
} Token;

typedef struct Parser {
    Token *current;
} Parser;

Token *lexer(Arena *a, char *stmt);

Node *parser(Arena *a, Token *tkList);
void  field_free(Field *f);

void         sqlram_set_error(const char *fmt, ...);
sqlram_value value_dup(sqlram_value v);
sqlram_result *result_new(int ncols);
void          result_set_col(sqlram_result *r, int i, const char *name);
void          result_add_row(sqlram_result *r, Field *vals, int n);

sqlram_result *exec_dispatch(Node *node);

int            exec_create_database(char *dbname);
sqlram_result *exec_show_databases(void);
int            exec_use_database(char *dbname);
int            exec_drop_database(char *dbname);
Database      *current_db(void);

int            exec_create_table(char *tblname, TableFieldS **fields, int numFields);
sqlram_result *exec_show_tables(void);
int            exec_drop_table(char *tblname);
Table         *find_table(const char *name);
void           table_free_list(Table *t);

int            exec_insert(char *tblname, Field *values, int numValues);
int            exec_upsert(char *tblname, Field *values, int numValues,
                           char **conflictCols, int numConflictCols);
sqlram_result *exec_select(struct SelectS *sel);
int            exec_update(struct UpdateS *upd);
int            exec_delete(struct DeleteS *del);
int            exec_truncate(char *tblname);
void           record_free_all(Table *t);

#endif
