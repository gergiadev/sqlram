/* Example: runs SQL from stdin and prints results.
 * Build with: cc -O2 -o example example.c libsqlram.a */
#include "sqlram.h"
#include <stdio.h>
#include <string.h>

static void print_result(sqlram_result *r) {
    if (r->num_cols == 0) {
        printf("Query OK. %d rows affected\n", r->affected);
        return;
    }
    for (int i = 0; i < r->num_cols; i++) {
        if (i > 0) printf("|");
        printf("%s", r->col_names[i]);
    }
    printf("\n");
    for (int i = 0; i < r->num_rows; i++) {
        for (int j = 0; j < r->num_cols; j++) {
            if (j > 0) printf("|");
            sqlram_value *v = &r->rows[i][j];
            switch (v->type) {
                case SQLRAM_INT:  printf("%ld", v->v.i_val); break;
                case SQLRAM_BOOL: printf("%s", v->v.b_val ? "true" : "false"); break;
                case SQLRAM_FLOAT: printf("%g", v->v.d_val); break;
                case SQLRAM_TIMESTAMP: {
                    char buf[32];
                    struct tm tmv;
                    localtime_r(&v->v.t_val, &tmv);
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
                    printf("%s", buf);
                    break;
                }
                case SQLRAM_TEXT: printf("%s", v->v.s_val ? v->v.s_val : ""); break;
            }
        }
        printf("\n");
    }
}

int main(void) {
    sqlram_init();

    char stmt[4096];
    stmt[0] = '\0';

    char buf[512];
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t have = strlen(stmt);
        size_t add  = strlen(buf);
        if (have + add >= sizeof(stmt)) {
            sqlram_exec(stmt);
            stmt[0] = '\0';
            have = 0;
        }
        memcpy(stmt + have, buf, add + 1);

        if (strchr(stmt, ';')) {
            sqlram_result *r = sqlram_exec(stmt);
            if (!r) {
                printf("Error: %s\n", sqlram_error());
            } else {
                print_result(r);
                sqlram_result_free(r);
            }
            stmt[0] = '\0';
        }
    }

    sqlram_close();
    return 0;
}
