/*
 * print_output.c
 *
 * Programma ausiliario per stampare in forma leggibile il contenuto
 * del file di output prodotto dal framework libmr.
 *
 * Formato del file di output (come definito in mr.c):
 *   Per ogni record:
 *     [int token_len]    lunghezza del token (senza '\0')
 *     [token_len byte]   token
 *     [int result_len]   lunghezza del risultato
 *     [result_len byte]  risultato (opaco)
 *
 * Questo programma interpreta il campo result come int (contatore).
 * Per l'uso con reducer diversi, modificare la stampa del risultato.
 *
 * Uso:
 *   ./print_output <file.mro>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <file.mro>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    long n = 0;

    for (;;) {
        int token_len;
        if (fread(&token_len, sizeof(token_len), 1, f) != 1) {
            if (feof(f)) break;
            perror("lettura token_len");
            fclose(f); return 1;
        }

        if (token_len <= 0 || token_len > 4096) {
            fprintf(stderr, "token_len non valido: %d\n", token_len);
            fclose(f); return 1;
        }

        char *token = malloc((size_t)token_len + 1);
        if (!token) { perror("malloc"); fclose(f); return 1; }
        if (fread(token, (size_t)token_len, 1, f) != 1) {
            perror("lettura token"); free(token); fclose(f); return 1;
        }
        token[token_len] = '\0';

        int result_len;
        if (fread(&result_len, sizeof(result_len), 1, f) != 1) {
            perror("lettura result_len"); free(token); fclose(f); return 1;
        }

        void *result = NULL;
        if (result_len > 0) {
            result = malloc((size_t)result_len);
            if (!result) { perror("malloc"); free(token); fclose(f); return 1; }
            if (fread(result, (size_t)result_len, 1, f) != 1) {
                perror("lettura result");
                free(token); free(result); fclose(f); return 1;
            }
        }

        /* Stampa: interpreta il risultato come int (per wordcount) */
        if (result_len == (int)sizeof(int)) {
            int v;
            memcpy(&v, result, sizeof(int));
            printf("%s\t%d\n", token, v);
        } else {
            /* Stampa esadecimale per risultati di altro tipo */
            printf("%s\t[%d byte:", token, result_len);
            for (int i = 0; i < result_len && i < 16; i++)
                printf(" %02x", ((unsigned char *)result)[i]);
            if (result_len > 16) printf(" ...");
            printf("]\n");
        }

        free(token);
        free(result);
        n++;
    }

    fclose(f);
    fprintf(stderr, "Totale record letti: %ld\n", n);
    return 0;
}
