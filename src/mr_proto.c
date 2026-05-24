/*
 * mr_proto.c
 *
 * Protocollo di serializzazione dei messaggi sulle pipe.
 *
 * ── Pipe 1: processo principale → processo mapper ──────────────────
 *
 *   Per ogni riga logica:
 *     [int fname_len]           (lunghezza nome file, SENZA '\0')
 *     [fname_len byte]          (nome file)
 *     [unsigned long line_num]  (numero riga, base 1)
 *     [int line_len]            (lunghezza contenuto riga, SENZA '\n')
 *     [line_len byte]           (contenuto riga)
 *
 *   EOF sulla pipe = fine input.
 *
 * ── Pipe 2: processo mapper → processo reducer ──────────────────────
 *
 *   Per ogni coppia <token, valore>:
 *     [mr_pair_header_t]        { int token_len; int value_len; }
 *     [token_len byte]          (token, SENZA '\0')
 *     [value_len byte]          (valore opaco)
 *
 *   EOF sulla pipe = fine coppie.
 *
 * ── Pipe 3: processo reducer → processo principale ──────────────────
 *
 *   Per ogni risultato:
 *     [mr_result_header_t]      { int token_len; int result_len; }
 *     [token_len byte]          (token, SENZA '\0')
 *     [result_len byte]         (risultato opaco)
 *
 *   EOF sulla pipe = fine risultati.
 *
 * Tutte le lunghezze sono di tipo int. Vengono validate prima dell'uso:
 * devono essere >= 0 e non superare i limiti definiti in mr_internal.h.
 * Letture e scritture parziali sono gestite da readn()/writen().
 */

#include "mr_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/*Chiusura sicura dei fd*/
void safe_close(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
    }        
}

/* ------------------------------------------------------------------ */
/* I/O robusto                                                         */
/* ------------------------------------------------------------------ */

ssize_t readn(int fd, void *buf, size_t n)
{
    size_t  total = 0;
    ssize_t r;
    char   *p = (char *)buf;

    while (total < n) {
        r = read(fd, p + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            /* EOF prima di aver letto n byte */
            if (total == 0) return 0;   /* EOF pulito all'inizio */
            errno = EIO;
            return -1;                  /* EOF a metà messaggio  */
        }
        total += (size_t)r;
    }
    return (ssize_t)total;
}

ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t       total = 0;
    ssize_t      w;
    const char  *p = (const char *)buf;

    while (total < n) {
        w = write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

/* ------------------------------------------------------------------ */
/* Pipe 1: riga logica (main → mapper)                                 */
/* ------------------------------------------------------------------ */

int proto_write_line(int fd, const mr_file_line_t *line)
{
    int fname_len = (int)line->file_name_len;
    int line_len  = (int)line->line_len;

    if (writen(fd, &fname_len, sizeof(fname_len)) != sizeof(fname_len))
        return -1;
    if (fname_len > 0)
        if (writen(fd, line->file_name, (size_t)fname_len) != fname_len)
            return -1;
    if (writen(fd, &line->line_number, sizeof(line->line_number))
            != sizeof(line->line_number))
        return -1;
    if (writen(fd, &line_len, sizeof(line_len)) != sizeof(line_len))
        return -1;
    if (line_len > 0)
        if (writen(fd, line->line, (size_t)line_len) != line_len)
            return -1;
    return 0;
}

/*
 * proto_read_line – legge una riga serializzata dalla pipe.
 *
 * Alloca *fname_buf e *line_buf (il chiamante deve liberarli).
 * Popola *out con puntatori verso quei buffer.
 *
 * Restituisce:
 *   1  = riga letta con successo
 *   0  = EOF (nessun altro messaggio)
 *  -1  = errore
 */
int proto_read_line(int fd, mr_file_line_t *out,
                    char **fname_buf, char **line_buf)
{
    int fname_len;
    ssize_t r;

    /* Primo campo: fname_len.  Se EOF qui, il flusso è terminato. */
    r = readn(fd, &fname_len, sizeof(fname_len));
    if (r == 0) return 0;  /* EOF pulito */
    if (r < 0)  return -1;

    if (fname_len < 0 || fname_len > MR_MAX_FNAME_LEN) {
        errno = EPROTO; return -1;
    }

    /* Alloca e legge il nome file */
    *fname_buf = malloc((size_t)fname_len + 1);
    if (!*fname_buf) return -1;
    if (fname_len > 0) {
        if (readn(fd, *fname_buf, (size_t)fname_len) != fname_len) {
            free(*fname_buf); *fname_buf = NULL; return -1;
        }
    }
    (*fname_buf)[fname_len] = '\0';

    /* Numero riga */
    unsigned long line_number;
    if (readn(fd, &line_number, sizeof(line_number))
            != (ssize_t)sizeof(line_number)) {
        free(*fname_buf); *fname_buf = NULL; return -1;
    }

    /* Lunghezza contenuto riga */
    int line_len;
    if (readn(fd, &line_len, sizeof(line_len)) != sizeof(line_len)) {
        free(*fname_buf); *fname_buf = NULL; return -1;
    }
    if (line_len < 0 || line_len > MR_MAX_LINE_LEN) {
        free(*fname_buf); *fname_buf = NULL; errno = EPROTO; return -1;
    }

    /* Alloca e legge il contenuto */
    *line_buf = malloc((size_t)line_len + 1);
    if (!*line_buf) {
        free(*fname_buf); *fname_buf = NULL; return -1;
    }
    if (line_len > 0) {
        if (readn(fd, *line_buf, (size_t)line_len) != line_len) {
            free(*fname_buf); *fname_buf = NULL;
            free(*line_buf);  *line_buf  = NULL;
            return -1;
        }
    }
    (*line_buf)[line_len] = '\0';  /* aggiunto per comodità interna */

    out->file_name     = *fname_buf;
    out->file_name_len = (size_t)fname_len;
    out->line_number   = line_number;
    out->line          = *line_buf;
    out->line_len      = (size_t)line_len;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Pipe 2: coppia <token, valore> (mapper → reducer)                   */
/* ------------------------------------------------------------------ */

int proto_write_pair(int fd, const char *token, int token_len,
                     const void *value, int value_len)
{
    mr_pair_header_t hdr = { token_len, value_len };
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (token_len > 0)
        if (writen(fd, token, (size_t)token_len) != token_len) return -1;
    if (value_len > 0)
        if (writen(fd, value, (size_t)value_len) != value_len) return -1;
    return 0;
}

/*
 * proto_read_pair
 * Restituisce 1 = coppia letta, 0 = EOF, -1 = errore.
 * Alloca *token_out (stringa '\0'-terminata) e *value_out (byte opachi).
 */
int proto_read_pair(int fd, char **token_out, int *token_len_out,
                    void **value_out, int *value_len_out)
{
    mr_pair_header_t hdr;
    ssize_t r = readn(fd, &hdr, sizeof(hdr));
    if (r == 0) return 0;
    if (r < 0)  return -1;

    if (hdr.token_len <= 0 || hdr.token_len > MR_MAX_TOKEN_LEN) {
        errno = EPROTO; return -1;
    }
    if (hdr.value_len < 0 || hdr.value_len > MR_MAX_VALUE_LEN) {
        errno = EPROTO; return -1;
    }

    char *tok = malloc((size_t)hdr.token_len + 1);
    if (!tok) return -1;
    if (readn(fd, tok, (size_t)hdr.token_len) != hdr.token_len) {
        free(tok); return -1;
    }
    tok[hdr.token_len] = '\0';

    void *val = NULL;
    if (hdr.value_len > 0) {
        val = malloc((size_t)hdr.value_len);
        if (!val) { free(tok); return -1; }
        if (readn(fd, val, (size_t)hdr.value_len) != hdr.value_len) {
            free(tok); free(val); return -1;
        }
    }

    *token_out     = tok;
    *token_len_out = hdr.token_len;
    *value_out     = val;
    *value_len_out = hdr.value_len;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pipe 3: risultato (reducer → main)                                  */
/* ------------------------------------------------------------------ */

int proto_write_result(int fd, const char *token, int token_len,
                       const void *result, int result_len)
{
    mr_result_header_t hdr = { token_len, result_len };
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (token_len > 0)
        if (writen(fd, token, (size_t)token_len) != token_len) return -1;
    if (result_len > 0)
        if (writen(fd, result, (size_t)result_len) != result_len) return -1;
    return 0;
}

/*
 * proto_read_result
 * Restituisce 1 = risultato letto, 0 = EOF, -1 = errore.
 */
int proto_read_result(int fd, char **token_out, int *token_len_out,
                      void **result_out, int *result_len_out)
{
    mr_result_header_t hdr;
    ssize_t r = readn(fd, &hdr, sizeof(hdr));
    if (r == 0) return 0;
    if (r < 0)  return -1;

    if (hdr.token_len <= 0 || hdr.token_len > MR_MAX_TOKEN_LEN) {
        errno = EPROTO; return -1;
    }
    if (hdr.result_len < 0 || hdr.result_len > MR_MAX_RESULT_LEN) {
        errno = EPROTO; return -1;
    }

    char *tok = malloc((size_t)hdr.token_len + 1);
    if (!tok) return -1;
    if (readn(fd, tok, (size_t)hdr.token_len) != hdr.token_len) {
        free(tok); return -1;
    }
    tok[hdr.token_len] = '\0';

    void *res = NULL;
    if (hdr.result_len > 0) {
        res = malloc((size_t)hdr.result_len);
        if (!res) { free(tok); return -1; }
        if (readn(fd, res, (size_t)hdr.result_len) != hdr.result_len) {
            free(tok); free(res); return -1;
        }
    }

    *token_out      = tok;
    *token_len_out  = hdr.token_len;
    *result_out     = res;
    *result_len_out = hdr.result_len;
    return 1;
}
