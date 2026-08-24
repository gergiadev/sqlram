#include "sqlram.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static void print_result (sqlram_result *r) {
    if (r->num_cols == 0) {
        printf ("Query OK. %d rows affected\n", r->affected);
        return;
    }
    for (int i = 0; i < r->num_cols; i++) {
        if (i > 0) {
            printf ("|");
        }
        printf ("%s", r->col_names[i]);
    }
    printf ("\n");
    for (int i = 0; i < r->num_rows; i++) {
        for (int j = 0; j < r->num_cols; j++) {
            if (j > 0) {
                printf ("|");
            }
            sqlram_value *v = &r->rows[i][j];
            switch (v->type) {
            case SQLRAM_INT:
                printf ("%ld", v->v.i_val);
                break;
            case SQLRAM_BOOL:
                printf ("%s", v->v.b_val ? "true" : "false");
                break;
            case SQLRAM_FLOAT:
                printf ("%g", v->v.d_val);
                break;
            case SQLRAM_TIMESTAMP: {
                char buf[32];
                struct tm tmv;
                localtime_r (&v->v.t_val, &tmv);
                strftime (buf, sizeof (buf), "%Y-%m-%d %H:%M:%S", &tmv);
                printf ("%s", buf);
                break;
            }
            case SQLRAM_TEXT:
                printf ("%s", v->v.s_val ? v->v.s_val : "");
                break;
            }
        }
        printf ("\n");
    }
}

static void trim_newline (char *s) {
    size_t n = strlen (s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int is_quit (const char *s) {
    while (isspace ((unsigned char)*s)) {
        s++;
    }
    if (strncasecmp (s, "quit", 4) != 0 && strncasecmp (s, "exit", 4) != 0) {
        return 0;
    }
    s += 4;
    while (isspace ((unsigned char)*s) || *s == ';') {
        s++;
    }
    return *s == '\0';
}

int main (void) {
    sqlram_init ();

    time_t start_time, end_time;
    time (&start_time);

    int interactive = isatty (STDIN_FILENO);

    char *line = NULL;
    size_t cap = 0;

    while (1) {
        if (interactive) {
            printf ("sqlram> ");
            fflush (stdout);
        }
        if (getline (&line, &cap, stdin) == -1) {
            break;
        }
        trim_newline (line);

        if (is_quit (line)) {
            break;
        }

        char *stmt = strdup (line);
        while (strchr (stmt, ';') == NULL) {
            if (interactive) {
                printf ("...> ");
                fflush (stdout);
            }
            if (getline (&line, &cap, stdin) == -1) {
                break;
            }
            trim_newline (line);

            size_t n = strlen (stmt) + strlen (line) + 2;
            char *tmp = realloc (stmt, n);
            if (!tmp) {
                free (stmt);
                stmt = NULL;
                break;
            }
            strcat (tmp, " ");
            strcat (tmp, line);
            stmt = tmp;
        }

        if (!stmt) {
            continue;
        }
        if (*stmt == '\0') {
            free (stmt);
            continue;
        }

        sqlram_result *r = sqlram_exec (stmt);
        if (!r) {
            printf ("Error: %s\n", sqlram_error ());
        } else {
            print_result (r);
            sqlram_result_free (r);
        }
        free (stmt);
    }

    free (line);

    time (&end_time);
    printf ("You used sqlram for %.2f seconds\n", difftime (end_time, start_time));
    printf ("Bye\n");

    sqlram_close ();
    return 0;
}
