/*
 * wordcount.c
 *
 * Programma di esempio che usa libmr per contare le occorrenze dei token
 * in un file o in una directory di file testuali.
 *
 * Uso:
 *   ./wordcount <input> <output>
 *
 * Il mapper emette, per ogni token alfanumerico trovato nella riga,
 * il valore intero 1 (serializzato come int).
 *
 * Il reducer somma tutti gli interi ricevuti per ogni token e
 * emette il totale (serializzato come int).
 *
 * Il file di output è in formato binario (mr_result_header_t + dati).
 * Usare il programma 'print_output' per leggerne il contenuto.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mr.h"

/* ------------------------------------------------------------------ */
/* Funzione mapper                                                     */
/* ------------------------------------------------------------------ */

static int wc_mapper(const mr_file_line_t *line,
                     mr_emit_pair_t emit, void *emit_arg,
                     void *user_arg)
{
    (void)user_arg;

    const char *p   = line->line;
    size_t      len = line->line_len;
    size_t      i   = 0;

    while (i < len) {
        /* Salta caratteri non alfanumerici in teoria per la specifica
        ovvero input di file alfanumerici ASCII non dovrebbe essere necessario */
        while (i < len && !isalnum((unsigned char)p[i])) i++;
        if (i >= len) break;

        /* Raccoglie il token */
        size_t start = i;
        while (i < len && isalnum((unsigned char)p[i])) i++;
        size_t tok_len = i - start;

        /* Costruisce una stringa '\0'-terminata */
        char token[tok_len + 1];
        memcpy(token, p + start, tok_len);
        token[tok_len] = '\0';

        /* Emette <token, 1> */
        int one = 1;
        if (emit(token, &one, sizeof(one), emit_arg) < 0) {
            fprintf(stderr, "emit fallita per token '%s'\n", token);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Funzione reducer                                                    */
/* ------------------------------------------------------------------ */

static int wc_reducer(const char *token,
                      const mr_value_t *values, size_t values_count,
                      mr_emit_result_t emit, void *emit_arg,
                      void *user_arg)
{
    (void)user_arg;

    int total = 0;
    for (size_t i = 0; i < values_count; i++) {
        if (values[i].size == sizeof(int)) {
            int v;
            memcpy(&v, values[i].data, sizeof(int));
            total += v;
        }
    }

    if (emit(token, &total, sizeof(total), emit_arg) < 0) {
        fprintf(stderr, "emit risultato fallita per token '%s'\n", token);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <input> <output>\n", argv[0]);
        return 1;
    }

    mr_t     mr;
    mr_attr_t attr;

    if (mr_attr_init(&attr) < 0) {
        perror("mr_attr_init");
        return 1;
    }
    if (mr_attr_set_mapper_threads(&attr, 4) < 0) {
        perror("mr_attr_set_mapper_threads");
        mr_attr_destroy(&attr);
        return 1;
    }
    if (mr_attr_set_reducer_threads(&attr, 4) < 0) {
        perror("mr_attr_set_reducer_threads");
        mr_attr_destroy(&attr);
        return 1;
    }
    if (mr_attr_set_queue_size(&attr, 64) < 0) {
        perror("mr_attr_set_queue_size");
        mr_attr_destroy(&attr);
        return 1;
    }
    if (mr_attr_set_log_file(&attr, "mr.log") < 0) {
        perror("mr_attr_set_log_file");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_create(&mr, &attr, wc_mapper, wc_reducer, NULL) < 0) {
        perror("mr_create");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_start(mr, argv[1], argv[2]) < 0) {
        perror("mr_start");
        mr_destroy(mr);
        mr_attr_destroy(&attr);
        return 1;
    }

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf("Elaborazione completata. Output in '%s'.\n", argv[2]);
    return 0;
}
