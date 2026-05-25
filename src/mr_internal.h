#ifndef MR_INTERNAL_H
#define MR_INTERNAL_H

/*
 * Header interno del framework libmr.
 * Non fa parte dell'interfaccia pubblica e non deve essere incluso
 * dai programmi applicativi.
 */

#include "mr.h"

#include <stddef.h>
#include <unistd.h>
#include <sys/types.h>
#include <threads.h>
#include <semaphore.h>

/* ------------------------------------------------------------------ */
/* Limiti ragionevoli per le lunghezze ricevute dal protocollo         */
/* ------------------------------------------------------------------ */
#define MR_MAX_TOKEN_LEN     4096   /* lunghezza massima di un token   */
#define MR_MAX_VALUE_LEN  (1024*1024) /* 1 MiB per valore opaco          */
#define MR_MAX_RESULT_LEN (1024*1024) /* 1 MiB per un risultato finale   */
#define MR_MAX_LINE_LEN  (1024*1024*4)  /* 4 MiB per una riga logica       */
#define MR_MAX_FNAME_LEN  4096      /* lunghezza massima di un nome file*/

/* ------------------------------------------------------------------ */
/* Struttura interna dell'istanza mr                                   */
/* ------------------------------------------------------------------ */
struct mr {
    /* Configurazione copiata da mr_attr_t */
    size_t  mapper_threads;
    size_t  reducer_threads;
    size_t  queue_size;
    char   *log_file;           /* stringa allocata, NULL = default    */

    /* Puntatori alle funzioni applicative fornite dall'utente*/
    mr_mapper_t  mapper;
    mr_reducer_t reducer;
    void        *user_arg;
};

/* ------------------------------------------------------------------------------- */
/* Sezione dedicata alla definizione di utility per le code produttore consumatore */
/* ------------------------------------------------------------------------------- */

typedef struct {
    void   **buf;       /* array circolare di puntatori                */
    size_t   cap;       /* capacità massima (numero di elementi)       */
    size_t   head;      /* indice di lettura                           */
    size_t   tail;      /* indice di scrittura                         */
    size_t   count;     /* elementi presenti                           */
    int      closed;    /* 1 quando il produttore ha chiuso la coda    */
    mtx_t    mtx;
    cnd_t    not_full;
    cnd_t    not_empty;
} mr_queue_t;

/*
 * Inizializza la coda con capacità cap > 0.
 * Restituisce 0 in caso di successo, -1 in caso di errore.
 */
int  queue_init(mr_queue_t *q, size_t cap);

/*
 * Distrugge la coda (non libera gli elementi residui: il chiamante
 * deve farlo prima di chiamare queue_destroy).
 */
void queue_destroy(mr_queue_t *q);

/*
 * Inserisce item nella coda.
 * Blocca se la coda è piena.
 * Restituisce 0, oppure -1 se la coda è già chiusa.
 */
int  queue_push(mr_queue_t *q, void *item);

/*
 * Estrae un elemento dalla coda.
 * Blocca se la coda è vuota E non è chiusa.
 * Restituisce 1 e pone *item = elemento estratto se disponibile.
 * Restituisce 0 e pone *item = NULL se la coda è chiusa e vuota
 * (segnale di fine per il consumatore).
 */
int  queue_pop(mr_queue_t *q, void **item);

/*
 * Marca la coda come chiusa.
 * Sveglia tutti i consumatori bloccati in attesa.
 * Dopo questa chiamata, queue_push restituisce -1.
 */
void queue_close(mr_queue_t *q);


/* ------------------------------------------------------------------ */
/* Struttura passata al processo mapper (tramite fork – memoria locale)*/
/* ------------------------------------------------------------------ */
typedef struct {
    struct mr *mr;          /* puntatore all'istanza                   */
    int        stdin_fd;    /* fd già impostato su stdin dal padre     */
    int        stdout_fd;   /* fd già impostato su stdout dal padre    */
} mapper_proc_args_t;

/* ------------------------------------------------------------------ */
/* Struttura passata al processo reducer                               */
/* ------------------------------------------------------------------ */
typedef struct {
    struct mr *mr;
    int        stdin_fd;
    int        stdout_fd;
    const char *output_path;
} reducer_proc_args_t;

/* ------------------------------------------------------------------ */
/* Funzioni dei sotto-processi (definite in mr_mapper.c / mr_reducer.c)*/
/* ------------------------------------------------------------------ */
void mapper_process_main(struct mr *mr);
void reducer_process_main(struct mr *mr, const char *output_path);

/* ------------------------------------------------------------------ */
/* Funzioni di I/O robuste (definite in mr_proto.c)                   */
/* ------------------------------------------------------------------ */
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

/* ------------------------------------------------------------------ */
/* Protocollo serializzazione riga logica (main -> mapper)             */
/* ------------------------------------------------------------------ */
/*
 * Formato:
 *   [int fname_len][fname_len byte del nome file]
 *   [unsigned long line_number]
 *   [int line_len][line_len byte del contenuto]
 *
 * Le lunghezze sono in byte. Non includono il terminatore '\0'.
 * Lettura: EOF sulla pipe = nessun altro messaggio.
 */
int proto_write_line(int fd, const mr_file_line_t *line);
int proto_read_line(int fd, mr_file_line_t *out,
                    char **fname_buf, char **line_buf);

/* ------------------------------------------------------------------ */
/* Protocollo serializzazione coppia (mapper -> reducer)               */
/* ------------------------------------------------------------------ */
/*
 * Header:
 *   typedef struct { int token_len; int value_len; } mr_pair_header_t;
 * Seguito da:
 *   [token_len byte del token]
 *   [value_len byte del valore opaco]
 */
typedef struct {
    int token_len;
    int value_len;
} mr_pair_header_t;

int proto_write_pair(int fd, const char *token, int token_len,
                     const void *value, int value_len);
int proto_read_pair(int fd, char **token_out, int *token_len_out,
                    void **value_out, int *value_len_out);

/* ------------------------------------------------------------------ */
/* Protocollo serializzazione risultato (reducer -> main)              */
/* ------------------------------------------------------------------ */
/*
 * Formato:
 *   [int token_len][token_len byte del token]
 *   [int result_len][result_len byte del risultato]
 */
typedef struct {
    int token_len;
    int result_len;
} mr_result_header_t;

int proto_write_result(int fd, const char *token, int token_len,
                       const void *result, int result_len);
int proto_read_result(int fd, char **token_out, int *token_len_out,
                      void **result_out, int *result_len_out);

/* ------------------------------------------------------------------ */
/* Funzioni di log (definite in mr_log.c)                             */
/* ------------------------------------------------------------------ */

/*
 * Apre il file di log. Da chiamare UNA volta per processo
 * (main, mapper, reducer) subito dopo il fork.
 * Se path == NULL viene usato "mr.log".
 * La scrittura è sincronizzata tramite semaforo POSIX named.
 */
int  log_open(const char *path);
void log_close(void);

/* Scrive una riga di log nel formato:
 *   [TEMPO] [PID] [TID] [LEVEL] messaggio
 */
void log_write(const char *level, const char *fmt, ...);

#define LOG_INFO(...)  log_write("INFO", __VA_ARGS__)
#define LOG_ERROR(...) log_write("ERROR", __VA_ARGS__)
#define LOG_WARN(...)  log_write("WARN", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Utilità generali                                                    */
/* ------------------------------------------------------------------ */
/* Chiude un fd e lo pone a -1 se non è già -1 */
void safe_close(int *fd);

#endif
