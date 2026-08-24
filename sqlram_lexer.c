#include "sqlram_internal.h"

static Arena *g_arena;

static void *acalloc (size_t n) {
    return arena_calloc (g_arena, n);
}
static char *astrdup (const char *s) {
    return arena_strdup (g_arena, s);
}
static char *astrndup (const char *s, size_t n) {
    return arena_strndup (g_arena, s, n);
}

static void str2low (char *dst, const char *src, size_t n) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) {
        dst[i] = (char)tolower ((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int match_kw (const char *stmt, const char *kw) {
    char low[64];
    str2low (low, stmt, sizeof (low));
    size_t len = strlen (kw);
    if (len >= sizeof (low)) {
        return 0;
    }
    if (strncmp (low, kw, len) != 0) {
        return 0;
    }
    if (isalnum ((unsigned char)stmt[len]) || stmt[len] == '_') {
        return 0;
    }
    return 1;
}

static const struct {
    const char *kw;
    size_t len;
    TokenK kind;
} KEYWORDS[] = {
    {"create", 6, TK_CREATE},    {"show", 4, TK_SHOW},
    {"drop", 4, TK_DROP},        {"use", 3, TK_USE},
    {"insert", 6, TK_INSERT},    {"into", 4, TK_INTO},
    {"select", 6, TK_SELECT},    {"from", 4, TK_FROM},
    {"update", 6, TK_UPDATE},    {"set", 3, TK_SET},
    {"where", 5, TK_WHERE},      {"delete", 6, TK_DELETE},
    {"order", 5, TK_ORDER},      {"by", 2, TK_BY},
    {"asc", 3, TK_ASC},          {"desc", 4, TK_DESC},
    {"limit", 5, TK_LIMIT},      {"truncate", 8, TK_TRUNCATE},
    {"database", 8, TK_DB},      {"databases", 9, TK_DBS},
    {"table", 5, TK_TBL},        {"tables", 6, TK_TBLS},
    {"int", 3, TK_TYPE_INT},     {"bigint", 6, TK_TYPE_INT},
    {"bool", 4, TK_TYPE_BOOL},   {"text", 4, TK_TYPE_TEXT},
    {"float", 5, TK_TYPE_FLOAT}, {"timestamp", 9, TK_TYPE_TIMESTAMP},
    {"true", 4, TK_TRUE},        {"false", 5, TK_FALSE},
};

static Token *make_kw_token (const char *stmt, size_t len, TokenK kind) {
    Token *tok = acalloc (sizeof (Token));
    tok->Tkind = kind;
    tok->Tvalue = astrndup (stmt, len);
    tok->TLen = (int)len;
    tok->Tpos = (char *)stmt;
    return tok;
}

static Token *make_punct (TokenK kind, char *pos, int len) {
    Token *tok = acalloc (sizeof (Token));
    tok->Tkind = kind;
    tok->TLen = len;
    tok->Tpos = pos;
    return tok;
}

static TokenK scan_number (const char *s, const char **end) {
    const char *p = s;
    int is_float = 0;

    while (isdigit ((unsigned char)*p)) {
        p++;
    }
    if (*p == '.') {
        is_float = 1;
        p++;
        while (isdigit ((unsigned char)*p)) {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') {
            q++;
        }
        if (isdigit ((unsigned char)*q)) {
            is_float = 1;
            p = q;
            while (isdigit ((unsigned char)*p)) {
                p++;
            }
        }
    }

    *end = p;
    return is_float ? TK_FLOAT : TK_NUMBER;
}

Token *lexer (Arena *a, char *stmt) {
    g_arena = a;

    Token head = {0};
    Token *cur = &head;
    Token *tok;
    int posCounter;
    char stmtBuf[CONTEXT_SIZE];

    while (*stmt) {
        if (isspace ((unsigned char)*stmt)) {
            stmt++;
            continue;
        }

        if (*stmt == '"' || *stmt == '\'') {
            char quote = *stmt++;
            tok = acalloc (sizeof (Token));
            tok->Tkind = TK_STRING;
            tok->Tpos = stmt;
            posCounter = 0;
            /* "" (or '') is an escaped quote inside the literal. */
            while (*stmt && posCounter < CONTEXT_SIZE - 1) {
                if (*stmt == quote) {
                    if (stmt[1] == quote) {
                        stmtBuf[posCounter++] = *stmt;
                        stmt += 2;
                        continue;
                    }
                    break;
                }
                stmtBuf[posCounter++] = *stmt++;
            }
            stmtBuf[posCounter] = '\0';
            if (*stmt == quote) {
                stmt++;
            }
            tok->Tvalue = astrdup (stmtBuf);
            tok->TLen = posCounter;
            cur->next_token = tok;
            cur = tok;
            continue;
        }

        if (isalpha ((unsigned char)*stmt)) {
            tok = NULL;
            for (size_t i = 0; i < sizeof (KEYWORDS) / sizeof (KEYWORDS[0]); i++) {
                if (match_kw (stmt, KEYWORDS[i].kw)) {
                    tok = make_kw_token (stmt, KEYWORDS[i].len, KEYWORDS[i].kind);
                    stmt += KEYWORDS[i].len;
                    break;
                }
            }
            if (!tok) {
                tok = acalloc (sizeof (Token));
                tok->Tkind = TK_STRING;
                tok->Tpos = stmt;
                posCounter = 0;
                while ((isalpha ((unsigned char)*stmt) || isdigit ((unsigned char)*stmt)) && posCounter < CONTEXT_SIZE - 1) {
                    stmtBuf[posCounter++] = *stmt++;
                }
                stmtBuf[posCounter] = '\0';
                tok->Tvalue = astrdup (stmtBuf);
                tok->TLen = posCounter;
            }
            cur->next_token = tok;
            cur = tok;
            continue;
        }

        if (isdigit ((unsigned char)*stmt)) {
            const char *end;
            TokenK kind = scan_number (stmt, &end);
            size_t len = (size_t)(end - stmt);
            if (len > CONTEXT_SIZE - 1) {
                len = CONTEXT_SIZE - 1;
            }

            tok = acalloc (sizeof (Token));
            tok->Tkind = kind;
            tok->Tpos = stmt;
            memcpy (stmtBuf, stmt, len);
            stmtBuf[len] = '\0';
            tok->Tvalue = astrdup (stmtBuf);
            tok->TLen = (int)len;
            stmt += len;
            cur->next_token = tok;
            cur = tok;
            continue;
        }

        if (*stmt == '=') {
            tok = make_punct (TK_EQ, stmt, 1);
            stmt++;
        } else if (*stmt == '!') {
            if (stmt[1] == '=') {
                tok = make_punct (TK_NE, stmt, 2);
                stmt += 2;
            } else {
                stmt++;
                continue;
            }
        } else if (*stmt == '<') {
            if (stmt[1] == '=') {
                tok = make_punct (TK_LE, stmt, 2);
                stmt += 2;
            } else {
                tok = make_punct (TK_LT, stmt, 1);
                stmt++;
            }
        } else if (*stmt == '>') {
            if (stmt[1] == '=') {
                tok = make_punct (TK_GE, stmt, 2);
                stmt += 2;
            } else {
                tok = make_punct (TK_GT, stmt, 1);
                stmt++;
            }
        } else if (*stmt == '{') {
            tok = make_punct (TK_START_CURLY_BRACKET, stmt, 1);
            stmt++;
        } else if (*stmt == '}') {
            tok = make_punct (TK_END_CURLY_BRACKET, stmt, 1);
            stmt++;
        } else if (*stmt == ',') {
            tok = make_punct (TK_COMMA, stmt, 1);
            stmt++;
        } else if (*stmt == '*') {
            tok = make_punct (TK_STAR, stmt, 1);
            stmt++;
        } else if (*stmt == '?') {
            tok = make_punct (TK_PARAM, stmt, 1);
            stmt++;
        } else if (*stmt == ';') {
            break;
        } else {
            stmt++;
            continue;
        }

        cur->next_token = tok;
        cur = tok;
    }

    tok = acalloc (sizeof (Token));
    tok->Tkind = TK_END;
    tok->TLen = 1;
    tok->Tpos = stmt;
    cur->next_token = tok;

    return head.next_token;
}
