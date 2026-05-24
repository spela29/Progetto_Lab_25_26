/*
 * test_mr.c
 *
 * Suite di test automatizzati per il framework libmr.
 *
 * Ogni test è una funzione che restituisce 0 in caso di successo,
 * 1 in caso di fallimento.
 *
 * Test implementati:
 *   1. attr_defaults    – mr_attr_init imposta valori di default validi
 *   2. attr_invalid     – mr_attr_set_* rifiutano valori non validi
 *   3. empty_file       – file di input vuoto
 *   4. single_line      – file con una sola riga senza '\n' finale
 *   5. empty_lines      – righe vuote non causano crash
 *   6. wordcount        – conteggio parole su testo semplice
 *   7. multifile_dir    – elaborazione di una directory con più file
 *   8. determinism      – due esecuzioni identiche producono lo stesso output
 *   9. opaque_values    – valori opachi con byte nulli
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include "mr.h"

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* Legge il file di output e restituisce un array di coppie
 * {token, count_as_int}. Il chiamante libera *tokens e *counts. */
static int read_output_int(const char *path,
                           char ***tokens_out, int **counts_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char **toks  = NULL;
    int  *counts = NULL;
    int   n = 0, cap = 0;

    for (;;) {
        int token_len;
        if (fread(&token_len, sizeof(token_len), 1, f) != 1) {
            if (feof(f)) break;
            fclose(f); return -1;
        }
        if (token_len <= 0 || token_len > 4096) { fclose(f); return -1; }
        char *tok = malloc((size_t)token_len + 1);
        if (!tok) { fclose(f); return -1; }
        if (fread(tok, (size_t)token_len, 1, f) != 1) {
            free(tok); fclose(f); return -1;
        }
        tok[token_len] = '\0';

        int result_len;
        if (fread(&result_len, sizeof(result_len), 1, f) != 1) {
            free(tok); fclose(f); return -1;
        }
        int count = 0;
        if (result_len == (int)sizeof(int)) {
            if (fread(&count, sizeof(int), 1, f) != 1) {
                free(tok); fclose(f); return -1;
            }
        } else {
            fseek(f, result_len, SEEK_CUR);
        }

        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            char **tt = realloc(toks,  (size_t)nc * sizeof(char *));
            int  *ct  = realloc(counts,(size_t)nc * sizeof(int));
            if (!tt || !ct) { free(tok); fclose(f); return -1; }
            toks = tt; counts = ct; cap = nc;
        }
        toks[n]   = tok;
        counts[n] = count;
        n++;
    }
    fclose(f);
    *tokens_out = toks;
    *counts_out = counts;
    return n;
}

static void free_output(char **tokens, int *counts, int n)
{
    for (int i = 0; i < n; i++) free(tokens[i]);
    free(tokens);
    free(counts);
}

/* ------------------------------------------------------------------ */
/* Mapper / Reducer per il word count (usati nei test)                */
/* ------------------------------------------------------------------ */

static int wc_mapper(const mr_file_line_t *line,
                     mr_emit_pair_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    const char *p = line->line;
    size_t len = line->line_len, i = 0;
    while (i < len) {
        while (i < len && !isalnum((unsigned char)p[i])) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && isalnum((unsigned char)p[i])) i++;
        size_t tl = i - start;
        char token[tl + 1];
        memcpy(token, p + start, tl);
        token[tl] = '\0';
        int one = 1;
        emit(token, &one, sizeof(one), emit_arg);
    }
    return 0;
}

static int wc_reducer(const char *token,
                      const mr_value_t *values, size_t count,
                      mr_emit_result_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    int total = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i].size == sizeof(int)) {
            int v; memcpy(&v, values[i].data, sizeof(int));
            total += v;
        }
    }
    emit(token, &total, sizeof(total), emit_arg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mapper che emette valori con byte nulli (test opaco)               */
/* ------------------------------------------------------------------ */
static int opaque_mapper(const mr_file_line_t *line,
                         mr_emit_pair_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    /* Emette un valore di 4 byte con un byte nullo in mezzo */
    unsigned char val[4] = { 0x01, 0x00, 0x02, 0x03 };
    /* Usa il nome del file come token (solo caratteri alfanumerici,
     * quindi estraiamo il primo token dalla riga se disponibile) */
    if (line->line_len == 0) return 0;
    const char *p = line->line;
    size_t len = line->line_len, i = 0;
    while (i < len && !isalnum((unsigned char)p[i])) i++;
    if (i >= len) return 0;
    size_t start = i;
    while (i < len && isalnum((unsigned char)p[i])) i++;
    size_t tl = i - start;
    char token[tl + 1];
    memcpy(token, p + start, tl);
    token[tl] = '\0';
    emit(token, val, sizeof(val), emit_arg);
    return 0;
}

static int opaque_reducer(const char *token,
                          const mr_value_t *values, size_t count,
                          mr_emit_result_t emit, void *emit_arg, void *user_arg)
{
    (void)user_arg;
    /* Verifica che i valori abbiano la lunghezza attesa */
    for (size_t i = 0; i < count; i++) {
        if (values[i].size != 4) {
            fprintf(stderr, "ERRORE: valore opaco di dimensione inattesa %zu\n",
                    values[i].size);
        }
    }
    /* Emette il numero di valori ricevuti come int */
    int n = (int)count;
    emit(token, &n, sizeof(n), emit_arg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: esegue il framework su un input e output dati               */
/* ------------------------------------------------------------------ */

static int run_mr(const char *input, const char *output,
                  mr_mapper_t mapper, mr_reducer_t reducer,
                  size_t mapper_th, size_t reducer_th)
{
    mr_t mr;
    mr_attr_t attr;
    if (mr_attr_init(&attr) < 0) return -1;
    mr_attr_set_mapper_threads(&attr, mapper_th);
    mr_attr_set_reducer_threads(&attr, reducer_th);
    mr_attr_set_queue_size(&attr, 16);
    mr_attr_set_log_file(&attr, "test_mr.log");
    if (mr_create(&mr, &attr, mapper, reducer, NULL) < 0) {
        mr_attr_destroy(&attr); return -1;
    }
    int r = mr_start(mr, input, output);
    mr_destroy(mr);
    mr_attr_destroy(&attr);
    return r;
}

/* ------------------------------------------------------------------ */
/* TEST 1: attributi di default                                        */
/* ------------------------------------------------------------------ */
static int test_attr_defaults(void)
{
    mr_attr_t attr;
    if (mr_attr_init(&attr) < 0) { printf("FAIL: mr_attr_init\n"); return 1; }
    if (attr.mapper_threads == 0)  { printf("FAIL: mapper_threads == 0\n"); return 1; }
    if (attr.reducer_threads == 0) { printf("FAIL: reducer_threads == 0\n"); return 1; }
    if (attr.queue_size == 0)      { printf("FAIL: queue_size == 0\n"); return 1; }
    mr_attr_destroy(&attr);
    printf("PASS: test_attr_defaults\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 2: valori non validi rifiutati                                 */
/* ------------------------------------------------------------------ */
static int test_attr_invalid(void)
{
    mr_attr_t attr;
    mr_attr_init(&attr);
    if (mr_attr_set_mapper_threads(&attr, 0) != -1) {
        printf("FAIL: set_mapper_threads(0) avrebbe dovuto fallire\n"); return 1;
    }
    if (mr_attr_set_reducer_threads(&attr, 0) != -1) {
        printf("FAIL: set_reducer_threads(0) avrebbe dovuto fallire\n"); return 1;
    }
    if (mr_attr_set_queue_size(&attr, 0) != -1) {
        printf("FAIL: set_queue_size(0) avrebbe dovuto fallire\n"); return 1;
    }
    mr_attr_destroy(&attr);
    printf("PASS: test_attr_invalid\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 3: file vuoto                                                  */
/* ------------------------------------------------------------------ */
static int test_empty_file(void)
{
    write_file("/tmp/mr_test_empty.txt", "");
    int r = run_mr("/tmp/mr_test_empty.txt", "/tmp/mr_test_empty.mro",
                   wc_mapper, wc_reducer, 2, 2);
    if (r < 0) { printf("FAIL: test_empty_file mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_empty.mro", &toks, &counts);
    if (n < 0) { printf("FAIL: lettura output vuoto\n"); return 1; }
    if (n != 0) { printf("FAIL: output non vuoto (%d record)\n", n); return 1; }
    free_output(toks, counts, n);
    printf("PASS: test_empty_file\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 4: riga singola senza '\n' finale                              */
/* ------------------------------------------------------------------ */
static int test_single_line_no_newline(void)
{
    /* Scriviamo senza '\n' finale */
    int fd = open("/tmp/mr_test_single.txt",
                  O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { printf("FAIL: open\n"); return 1; }
    const char *content = "hello world";
    write(fd, content, strlen(content));
    close(fd);

    int r = run_mr("/tmp/mr_test_single.txt", "/tmp/mr_test_single.mro",
                   wc_mapper, wc_reducer, 2, 2);
    if (r < 0) { printf("FAIL: test_single_line mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_single.mro", &toks, &counts);
    if (n != 2) { printf("FAIL: attesi 2 token, trovati %d\n", n); return 1; }
    /* Verifica hello=1 world=1 (ordinati: hello, world) */
    if (strcmp(toks[0], "hello") != 0 || counts[0] != 1) {
        printf("FAIL: atteso hello=1, trovato %s=%d\n", toks[0], counts[0]);
        free_output(toks, counts, n); return 1;
    }
    if (strcmp(toks[1], "world") != 0 || counts[1] != 1) {
        printf("FAIL: atteso world=1, trovato %s=%d\n", toks[1], counts[1]);
        free_output(toks, counts, n); return 1;
    }
    free_output(toks, counts, n);
    printf("PASS: test_single_line_no_newline\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 5: righe vuote                                                 */
/* ------------------------------------------------------------------ */
static int test_empty_lines(void)
{
    write_file("/tmp/mr_test_emptylines.txt",
               "\n\nhello\n\nworld\n\n");
    int r = run_mr("/tmp/mr_test_emptylines.txt",
                   "/tmp/mr_test_emptylines.mro",
                   wc_mapper, wc_reducer, 2, 2);
    if (r < 0) { printf("FAIL: test_empty_lines mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_emptylines.mro", &toks, &counts);
    if (n != 2) { printf("FAIL: attesi 2 token, trovati %d\n", n); return 1; }
    free_output(toks, counts, n);
    printf("PASS: test_empty_lines\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 6: word count su testo più lungo                               */
/* ------------------------------------------------------------------ */
static int test_wordcount(void)
{
    write_file("/tmp/mr_test_wc.txt",
               "the cat sat on the mat\n"
               "the cat in the hat\n"
               "cat cat cat\n");
    int r = run_mr("/tmp/mr_test_wc.txt", "/tmp/mr_test_wc.mro",
                   wc_mapper, wc_reducer, 4, 4);
    if (r < 0) { printf("FAIL: test_wordcount mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_wc.mro", &toks, &counts);
    if (n <= 0) { printf("FAIL: nessun risultato\n"); return 1; }

    /* Cerca 'cat' (deve valere 6) e 'the' (deve valere 4) */
    int cat_count = -1, the_count = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], "cat") == 0) cat_count = counts[i];
        if (strcmp(toks[i], "the") == 0) the_count = counts[i];
    }
    if (cat_count != 5) {
        printf("FAIL: cat=%d (atteso 5)\n", cat_count);
        free_output(toks, counts, n); return 1;
    }
    if (the_count != 4) {
        printf("FAIL: the=%d (atteso 4)\n", the_count);
        free_output(toks, counts, n); return 1;
    }
    free_output(toks, counts, n);
    printf("PASS: test_wordcount\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 7: directory con più file                                       */
/* ------------------------------------------------------------------ */
static int test_multifile_dir(void)
{
    mkdir("/tmp/mr_test_dir", 0755);
    write_file("/tmp/mr_test_dir/a.txt", "apple banana apple\n");
    write_file("/tmp/mr_test_dir/b.txt", "banana cherry\n");
    write_file("/tmp/mr_test_dir/c.txt", "apple\n");

    int r = run_mr("/tmp/mr_test_dir", "/tmp/mr_test_dir_out.mro",
                   wc_mapper, wc_reducer, 2, 2);
    if (r < 0) { printf("FAIL: test_multifile_dir mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_dir_out.mro", &toks, &counts);
    if (n != 3) { printf("FAIL: attesi 3 token, trovati %d\n", n); return 1; }

    int apple_c = -1, banana_c = -1, cherry_c = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], "apple")  == 0) apple_c  = counts[i];
        if (strcmp(toks[i], "banana") == 0) banana_c = counts[i];
        if (strcmp(toks[i], "cherry") == 0) cherry_c = counts[i];
    }
    if (apple_c != 3 || banana_c != 2 || cherry_c != 1) {
        printf("FAIL: apple=%d banana=%d cherry=%d (atteso 3 2 1)\n",
               apple_c, banana_c, cherry_c);
        free_output(toks, counts, n); return 1;
    }
    free_output(toks, counts, n);
    printf("PASS: test_multifile_dir\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 8: determinismo (due esecuzioni identiche)                     */
/* ------------------------------------------------------------------ */
static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) { if(fa) fclose(fa); if(fb) fclose(fb); return 0; }
    int eq = 1;
    while (!feof(fa) && !feof(fb)) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) { eq = 0; break; }
    }
    if (!feof(fa) || !feof(fb)) eq = 0;
    fclose(fa); fclose(fb);
    return eq;
}

static int test_determinism(void)
{
    write_file("/tmp/mr_test_det.txt",
               "one two three one two one\n"
               "four five six four\n");

    run_mr("/tmp/mr_test_det.txt", "/tmp/mr_test_det1.mro",
           wc_mapper, wc_reducer, 3, 3);
    run_mr("/tmp/mr_test_det.txt", "/tmp/mr_test_det2.mro",
           wc_mapper, wc_reducer, 3, 3);

    if (!files_equal("/tmp/mr_test_det1.mro", "/tmp/mr_test_det2.mro")) {
        printf("FAIL: test_determinism – output diversi\n"); return 1;
    }
    printf("PASS: test_determinism\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST 9: valori opachi con byte nulli                                */
/* ------------------------------------------------------------------ */
static int test_opaque_values(void)
{
    write_file("/tmp/mr_test_opaque.txt", "hello\nhello\nworld\n");
    int r = run_mr("/tmp/mr_test_opaque.txt", "/tmp/mr_test_opaque.mro",
                   opaque_mapper, opaque_reducer, 2, 2);
    if (r < 0) { printf("FAIL: test_opaque_values mr_start\n"); return 1; }

    char **toks; int *counts;
    int n = read_output_int("/tmp/mr_test_opaque.mro", &toks, &counts);
    if (n != 2) { printf("FAIL: attesi 2 token, trovati %d\n", n); return 1; }

    int hello_c = -1, world_c = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], "hello") == 0) hello_c = counts[i];
        if (strcmp(toks[i], "world") == 0) world_c = counts[i];
    }
    if (hello_c != 2 || world_c != 1) {
        printf("FAIL: hello=%d world=%d (atteso 2 1)\n", hello_c, world_c);
        free_output(toks, counts, n); return 1;
    }
    free_output(toks, counts, n);
    printf("PASS: test_opaque_values\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    int failures = 0;

    printf("=== libmr test suite ===\n\n");

    failures += test_attr_defaults();
    failures += test_attr_invalid();
    failures += test_empty_file();
    failures += test_single_line_no_newline();
    failures += test_empty_lines();
    failures += test_wordcount();
    failures += test_multifile_dir();
    failures += test_determinism();
    failures += test_opaque_values();

    printf("\n=== %d test falliti su 9 ===\n", failures);
    return failures ? 1 : 0;
}
