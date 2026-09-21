#include "sqlram_internal.h"

/* Bump allocator over a list of chunks. Memory is reclaimed all at once with
 * arena_reset()/arena_free(); no individual frees are performed.
 *
 * Chunks are never resized. A pointer handed out by arena_alloc() stays valid
 * until the next reset, which is what lets the lexer and the parser hold
 * interior pointers (Token.next_token, every char * in the AST) while they
 * keep allocating. */

#define ARENA_CHUNK_MIN 4096

static size_t align8 (size_t n) {
    return (n + 7) & ~(size_t)7;
}

/* Header and payload in one allocation, as with Record. */
static ArenaChunk *chunk_new (size_t need, size_t prev_cap) {
    size_t cap = prev_cap ? prev_cap * 2 : ARENA_CHUNK_MIN;
    while (cap < need) {
        cap *= 2;
    }

    ArenaChunk *c = malloc (sizeof (ArenaChunk) + cap);
    if (!c) {
        return NULL;
    }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    c->data = (char *)(c + 1);
    return c;
}

void arena_init (Arena *a) {
    a->first = NULL;
    a->cur = NULL;
}

void *arena_alloc (Arena *a, size_t size) {
    size_t aligned = align8 (size);

    if (!a->cur || a->cur->used + aligned > a->cur->cap) {
        /* arena_reset() keeps the chunks, so look for a spent one further down
         * the list before allocating another. */
        ArenaChunk *c = a->cur ? a->cur->next : a->first;
        while (c && c->used + aligned > c->cap) {
            c = c->next;
        }
        if (!c) {
            c = chunk_new (aligned, a->cur ? a->cur->cap : 0);
            if (!c) {
                return NULL;
            }
            if (a->cur) {
                c->next = a->cur->next;
                a->cur->next = c;
            } else {
                c->next = a->first;
                a->first = c;
            }
        }
        a->cur = c;
    }

    void *p = a->cur->data + a->cur->used;
    a->cur->used += aligned;
    return p;
}

void *arena_calloc (Arena *a, size_t size) {
    void *p = arena_alloc (a, size);
    if (p) {
        memset (p, 0, size);
    }
    return p;
}

char *arena_strdup (Arena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen (s) + 1;
    char *p = arena_alloc (a, n);
    if (p) {
        memcpy (p, s, n);
    }
    return p;
}

char *arena_strndup (Arena *a, const char *s, size_t n) {
    char *p = arena_alloc (a, n + 1);
    if (p) {
        memcpy (p, s, n);
        p[n] = '\0';
    }
    return p;
}

/* Allocates a new block and copies min(old,new) bytes; the old block is left
 * as garbage and reclaimed at reset time. */
void *arena_realloc (Arena *a, void *ptr, size_t old_size, size_t new_size) {
    void *p = arena_alloc (a, new_size);
    if (p && ptr && old_size) {
        memcpy (p, ptr, old_size < new_size ? old_size : new_size);
    }
    return p;
}

void arena_reset (Arena *a) {
    for (ArenaChunk *c = a->first; c; c = c->next) {
        c->used = 0;
    }
    a->cur = a->first;
}

void arena_free (Arena *a) {
    ArenaChunk *c = a->first;
    while (c) {
        ArenaChunk *next = c->next;
        free (c);
        c = next;
    }
    a->first = NULL;
    a->cur = NULL;
}
