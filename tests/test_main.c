/*
 * test_mr.c
 * Suite di test per il framework libmr.
 * Ogni test restituisce 0 se passa, 1 se fallisce.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "mr.h"

/* ------------------------------------------------------------------ */
/* Funzioni di supporto usate da tutti i test                          */
/* ------------------------------------------------------------------ */

/* Scrive una stringa in un file */
static void crea_file(const char *path, const char *contenuto)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(contenuto, f);
    fclose(f);
}

/* Avvia il framework con i parametri dati */
static int esegui(const char *input, const char *output,
                  mr_mapper_t mapper, mr_reducer_t reducer,
                  size_t nth_mapper, size_t nth_reducer)
{
    mr_t mr;
    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, nth_mapper);
    mr_attr_set_reducer_threads(&attr, nth_reducer);
    mr_attr_set_queue_size(&attr, 16);
    mr_attr_set_log_file(&attr, "test_mr.log");
    if (mr_create(&mr, &attr, mapper, reducer, NULL) < 0) {
        mr_attr_destroy(&attr);
        return -1;
    }
    int r = mr_start(mr, input, output);
    mr_destroy(mr);
    mr_attr_destroy(&attr);
    return r;
}

/*
 * Legge il file di output binario e restituisce i token e i conteggi
 * in due array paralleli chi chiama questa funzione deve liberarli con libera_output().
 * Restituisce il numero di record letti, -1 in caso di errore.
 */
static int leggi_output(const char *path, char ***token_out, int **count_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char **token = NULL;
    int  *count  = NULL;
    int   n = 0, cap = 0;

    while(1) {
        /* Legge token_len */
        int tlen;
        if (fread(&tlen, sizeof(tlen), 1, f) != 1) break;
        if (tlen <= 0 || tlen > 4096) { fclose(f); return -1; }

        /* Legge il token */
        char *tok = malloc((size_t)tlen + 1);
        if (!tok) { fclose(f); return -1; }
        if (fread(tok, (size_t)tlen, 1, f) != 1) {
            free(tok); fclose(f); return -1;
        }
        tok[tlen] = '\0';

        /* Legge result_len e il risultato (interpretato come int) */
        int rlen;
        if (fread(&rlen, sizeof(rlen), 1, f) != 1) {
            free(tok); fclose(f); return -1;
        }
        int val = 0;
        if (rlen == (int)sizeof(int)) {
            if (fread(&val, sizeof(int), 1, f) != 1) {
                free(tok); fclose(f); return -1;
            }
        } else {
            fseek(f, rlen, SEEK_CUR);
        }

        /* Aggiunge agli array (raddoppia la capacità se serve) */
        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            char **tt = realloc(token, (size_t)nc * sizeof(char *));
            int  *cc  = realloc(count, (size_t)nc * sizeof(int));
            if (!tt || !cc) { free(tok); fclose(f); return -1; }
            token = tt; count = cc; cap = nc;
        }
        token[n] = tok;
        count[n] = val;
        n++;
    }

    fclose(f);
    *token_out = token;
    *count_out = count;
    return n;
}

static void libera_output(char **token, int *count, int n)
{
    for (int i = 0; i < n; i++) free(token[i]);
    free(token);
    free(count);
}

/* ------------------------------------------------------------------ */
/* Mapper e reducer per il word count (usati dalla maggior parte       */
/* dei test)                                                           */
/* ------------------------------------------------------------------ */

static int mapper_wc(const mr_file_line_t *line,
                     mr_emit_pair_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    const char *p = line->line;
    size_t len = line->line_len, i = 0;

    while (i < len) {
        /* Salta caratteri non alfanumerici */
        while (i < len && !isalnum((unsigned char)p[i])) i++;
        if (i >= len) break;
    
        /* raccoglie il token */
        size_t start = i;
        while (i < len && isalnum((unsigned char)p[i])) i++;
        size_t tl = i - start;

        char tok[tl + 1];
        memcpy(tok, p + start, tl);
        tok[tl] = '\0';

        /* emette <token, 1> */
        int uno = 1;
        emit(tok, &uno, sizeof(uno), emit_arg);
    }
    return 0;
}

static int reducer_wc(const char *token,
                      const mr_value_t *values, size_t n,
                      mr_emit_result_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    int totale = 0;
    for (size_t i = 0; i < n; i++) {
        if (values[i].size == sizeof(int)) {
            int v;
            memcpy(&v, values[i].data, sizeof(int));
            totale += v;
        }
    }
    emit(token, &totale, sizeof(totale), emit_arg);
    return 0;
}


/* ================================================================== */
/* TEST 1  */
/* ================================================================== */
static int test_1_attr_defaults(void)
{
    mr_attr_t attr;
    if (mr_attr_init(&attr) < 0){ printf("FAIL: mr_attr_init\n"); return 1; }
    if (attr.mapper_threads  <= 0){ printf("FAIL: mapper_threads == 0\n"); return 1; }
    if (attr.reducer_threads <= 0){ printf("FAIL: reducer_threads == 0\n"); return 1; }
    if (attr.queue_size      <= 0){ printf("FAIL: queue_size == 0\n"); return 1; }
    mr_attr_destroy(&attr);
    printf("PASS: test_1_attr_defaults\n");
    return 0;
}

/* ================================================================== */
/* TEST 2 */
/* ================================================================== */
static int test_2_attr_invalid(void)
{
    mr_attr_t attr;
    mr_attr_init(&attr);
    if (mr_attr_set_mapper_threads(&attr, 0) != -1) { printf("FAIL: set_mapper_threads(0)\n");  return 1; }
    if (mr_attr_set_reducer_threads(&attr, 0) != -1) { printf("FAIL: set_reducer_threads(0)\n"); return 1; }
    if (mr_attr_set_queue_size(&attr, 0) != -1) { printf("FAIL: set_queue_size(0)\n"); return 1; }
    mr_attr_destroy(&attr);
    printf("PASS: test_2_attr_invalid\n");
    return 0;
}


/* ================================================================== */
/* TEST 3 */
/* ================================================================== */
static int test_3_directory(void)
{
    mkdir("/tmp/mr_test_dir", 0755);
    crea_file("/tmp/mr_test_dir/a.txt", "pokemon scarlatto schifo\n");
    crea_file("/tmp/mr_test_dir/b.txt", "scarlatto pokemon\n");
    crea_file("/tmp/mr_test_dir/c.txt", "pokemon\n");

    int r = esegui("/tmp/mr_test_dir", "/tmp/mr_test_dir.mro",
                   mapper_wc, reducer_wc, 2, 2);
    if (r < 0) { printf("FAIL: mr_start\n"); return 1; }

    char **tok; int *cnt;
    int n = leggi_output("/tmp/mr_test_dir.mro", &tok, &cnt);
    if (n != 3) { printf("FAIL: attesi 3 token, trovati %d\n", n); return 1; }

    int pokemon = -1, scarlatto = -1, schifo = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(tok[i], "pokemon")  == 0) pokemon  = cnt[i];
        if (strcmp(tok[i], "scarlatto") == 0) scarlatto = cnt[i];
        if (strcmp(tok[i], "schifo") == 0) schifo = cnt[i];
    }
    if (pokemon != 3 || scarlatto != 2 || schifo != 1) {
        printf("FAIL: pokemon=%d scaratto=%d schifo=%d (atteso 3 2 1)\n",
               pokemon, scarlatto, schifo);
        libera_output(tok, cnt, n); return 1;
    }
    libera_output(tok, cnt, n);
    printf("PASS: test_3_directory\n");
    return 0;
}

/* ================================================================== */
/* TEST 4 */
/* ================================================================== */
static int file_uguali(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int uguale = 1;
    while (!feof(fa) && !feof(fb)) {
        if (fgetc(fa) != fgetc(fb)) { uguale = 0; break; }
    }
    if (!feof(fa) || !feof(fb)) uguale = 0;
    fclose(fa); fclose(fb);
    return uguale;
}

static int test_4_determinismo(void)
{
    crea_file("/tmp/mr_test_det.txt",
              "one two three one two one\n"
              "four five six four\n");

    esegui("/tmp/mr_test_det.txt", "/tmp/mr_test_det1.mro",
           mapper_wc, reducer_wc, 3, 3);
    esegui("/tmp/mr_test_det.txt", "/tmp/mr_test_det2.mro",
           mapper_wc, reducer_wc, 3, 3);

    if (file_uguali("/tmp/mr_test_det1.mro", "/tmp/mr_test_det2.mro")==0) {
        printf("FAIL: output diversi tra due esecuzioni identiche\n"); return 1;
    }
    printf("PASS: test_4_determinismo\n");
    return 0;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int main(void)
{
    int falliti = 0;

    printf("=== libmr test suite ===\n\n");

    falliti += test_1_attr_defaults();
    falliti += test_2_attr_invalid();
    falliti += test_3_directory();
    falliti += test_4_determinismo();

    printf("\n=== %d test falliti su 4 ===\n", falliti);
    return falliti ? 1 : 0;
}
