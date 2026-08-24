#include "sqlram_internal.h"

/* Bump allocator. Memory is reclaimed all at once with arena_reset()/arena_free();
 * no individual frees are performed. */
void arena_init (Arena *a) {
    a->buf = NULL;
    a->used = 0;
    a->cap = 0;
}

void *arena_alloc (Arena *a, size_t size) {
    size_t aligned = (size + 7) & ~(size_t)7;

    if (a->used + aligned > a->cap) {
        size_t new_cap = a->cap ? a->cap : 4096;
        while (new_cap < a->used + aligned) {
            new_cap *= 2;
        }
        char *nb = realloc (a->buf, new_cap);
        if (!nb) {
            return NULL;
        }
        a->buf = nb;
        a->cap = new_cap;
    }

    void *p = a->buf + a->used;
    a->used += aligned;
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
    a->used = 0;
}

void arena_free (Arena *a) {
    free (a->buf);
    a->buf = NULL;
    a->used = 0;
    a->cap = 0;
}
