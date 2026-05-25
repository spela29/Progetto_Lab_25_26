/*
 * mr_mapper.c
 */

#include "mr_internal.h"


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <threads.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Strutture interne                                                   */
/* ------------------------------------------------------------------ */

/* Un elemento della coda interna al processo mapper */
typedef struct {
    char         *fname_buf;    /* buffer del nome file (heap)         */
    char         *line_buf;     /* buffer del contenuto riga (heap)    */
    mr_file_line_t line;        /* struttura con puntatori a sopra     */
} mr_line_item_t;

/* Argomento passato ai thread worker */
typedef struct {
    struct mr    *mr;
    mr_queue_t   *queue;
    int           stdout_fd;    /* fd protetto dal mutex               */
    mtx_t        *write_mtx;   /* mutex per la scrittura su pipe      */
    atomic_long  *pair_count;  /* contatore atomico coppie prodotte   */
} mapper_worker_arg_t;

/* Argomento passato al thread lettore */
typedef struct {
    mr_queue_t *queue;
    int         stdin_fd;
    atomic_long *line_count;   /* contatore righe lette               */
} mapper_reader_arg_t;

/* ------------------------------------------------------------------ */
/* Funzione emit: chiamata dalla funzione mapper applicativa           */
/* ------------------------------------------------------------------ */

typedef struct {
    int          stdout_fd;
    mtx_t       *write_mtx;
    atomic_long *pair_count;
} emit_pair_ctx_t;

static int emit_pair_fn(const char *token,
                         const void *value, size_t value_size,
                         void *emit_arg)
{
    emit_pair_ctx_t *ctx = (emit_pair_ctx_t *)emit_arg;

    int token_len = (int)strlen(token);
    int value_len = (int)value_size;

    if (token_len <= 0 || token_len > MR_MAX_TOKEN_LEN) {
        errno = EINVAL; return -1;
    }
    if (value_len < 0 || value_len > MR_MAX_VALUE_LEN) {
        errno = EINVAL; return -1;
    }

    /* La scrittura sulla pipe deve essere atomica, a livello logico:
     * un intero messaggio non deve essere intervallato da messaggi
     * di altri thread. Usiamo il mutex dedicato. */
    mtx_lock(ctx->write_mtx);   
    int ret = proto_write_pair(ctx->stdout_fd,
                               token, token_len,
                               value, value_len);
    mtx_unlock(ctx->write_mtx);

    if (ret == 0)
        atomic_fetch_add(ctx->pair_count, 1);

    return ret;
}

/* ------------------------------------------------------------------ */
/* Thread lettore                                                      */
/* ------------------------------------------------------------------ */

static int mapper_reader_main(void *arg)
{
    mapper_reader_arg_t *a = (mapper_reader_arg_t *)arg;

    LOG_INFO("mapper reader thread avviato");

    while(1) {
        mr_line_item_t *item = malloc(sizeof(mr_line_item_t));
        if (!item) {
            LOG_ERROR("malloc item fallita nel reader");
            break;
        }
        item->fname_buf = NULL;
        item->line_buf  = NULL;

        int r = proto_read_line(STDIN_FILENO,
                                &item->line,
                                &item->fname_buf,
                                &item->line_buf);
        if (r == 0) {
            /* EOF: fine input */
            free(item);
            break;
        }
        if (r < 0) {
            LOG_ERROR("proto_read_line fallita: %s", strerror(errno));
            free(item->fname_buf);
            free(item->line_buf);
            free(item);
            break;
        }

        atomic_fetch_add(a->line_count, 1);

        if (queue_push(a->queue, item) < 0) {
            /* Coda chiusa (non dovrebbe succedere qui) */
            free(item->fname_buf);
            free(item->line_buf);
            free(item);
            break;
        }
    }

    queue_close(a->queue);
    LOG_INFO("mapper reader thread terminato");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread worker mapper                                                */
/* ------------------------------------------------------------------ */

static int mapper_worker_main(void *arg)
{
    mapper_worker_arg_t *a = (mapper_worker_arg_t *)arg;

    LOG_INFO("mapper worker thread avviato");

    emit_pair_ctx_t ctx = {
        .stdout_fd  = a->stdout_fd,
        .write_mtx  = a->write_mtx,
        .pair_count = a->pair_count,
    };

    while(1) {
        void *raw;
        int r = queue_pop(a->queue, &raw);
        if (r == 0) break;  /* coda chiusa e vuota */

        mr_line_item_t *item = (mr_line_item_t *)raw;

        /* Invoca la funzione mapper applicativa */
        int ret = a->mr->mapper(&item->line,
                                emit_pair_fn, &ctx,
                                a->mr->user_arg);
        if (ret < 0) {
            LOG_WARN("funzione mapper ha restituito errore per riga %lu",
                     item->line.line_number);
        }

        free(item->fname_buf);
        free(item->line_buf);
        free(item);
    }

    LOG_INFO("mapper worker thread terminato");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Funzione principale del processo mapper                             */
/* ------------------------------------------------------------------ */

void mapper_process_main(struct mr *mr)
{
    LOG_INFO("processo mapper avviato (PID %d)", (int)getpid());

    /* Coda produttore-consumatore tra reader e worker */
    mr_queue_t queue;
    if (queue_init(&queue, mr->queue_size) < 0) {
        LOG_ERROR("queue_init fallita: %s", strerror(errno));
        _exit(1);
    }

    /* Mutex per la scrittura serializzata su stdout */
    mtx_t write_mtx;
    if (mtx_init(&write_mtx, mtx_plain) != thrd_success) {
        LOG_ERROR("mtx_init fallita");
        queue_destroy(&queue);
        _exit(1);
    }

    /* Contatori atomici */
    atomic_long line_count = 0;
    atomic_long pair_count = 0;

    /* Avvio thread lettore  */
    mapper_reader_arg_t reader_arg = {
        .queue      = &queue,
        .stdin_fd   = STDIN_FILENO,
        .line_count = &line_count,
    };
    thrd_t reader_tid;
    if (thrd_create(&reader_tid, mapper_reader_main, &reader_arg)
            != thrd_success) {
        LOG_ERROR("thrd_create reader fallita");
        queue_destroy(&queue);
        mtx_destroy(&write_mtx);
        _exit(1);
    }
    LOG_INFO("thread reader mapper creato");

    /* Avvio thread worker  */
    size_t n = mr->mapper_threads;
    thrd_t *workers = malloc(n * sizeof(thrd_t));
    mapper_worker_arg_t *wargs = malloc(n * sizeof(mapper_worker_arg_t));
    if (!workers || !wargs) {
        LOG_ERROR("malloc worker array fallita");
        free(workers); free(wargs);
        queue_close(&queue);
        thrd_join(reader_tid, NULL);
        queue_destroy(&queue);
        mtx_destroy(&write_mtx);
        _exit(1);
    }

    for (size_t i = 0; i < n; i++) {
        wargs[i].mr         = mr;
        wargs[i].queue      = &queue;
        wargs[i].stdout_fd  = STDOUT_FILENO;
        wargs[i].write_mtx  = &write_mtx;
        wargs[i].pair_count = &pair_count;

        if (thrd_create(&workers[i], mapper_worker_main, &wargs[i])
                != thrd_success) {
            LOG_ERROR("thrd_create worker %zu fallita", i);
            /* Chiudiamo la coda per sbloccare i worker già avviati */
            queue_close(&queue);
            for (size_t j = 0; j < i; j++)
                thrd_join(workers[j], NULL);
            thrd_join(reader_tid, NULL);
            free(workers); free(wargs);
            queue_destroy(&queue);
            mtx_destroy(&write_mtx);
            _exit(1);
        }
        LOG_INFO("mapper worker thread %zu creato", i);
    }

    /* Attesa terminazione reader å */
    thrd_join(reader_tid, NULL);

    /* Attesa terminazione di tutti i worker  */
    for (size_t i = 0; i < n; i++)
        thrd_join(workers[i], NULL);

    LOG_INFO("tutti i thread mapper terminati; righe=%ld coppie=%ld",
             (long)atomic_load(&line_count),
             (long)atomic_load(&pair_count));

    free(workers);
    free(wargs);
    queue_destroy(&queue);
    mtx_destroy(&write_mtx);

    /*
     * Solo ORA chiudiamo stdout (la pipe verso il reducer).
     * Questo invia EOF al reducer e lo informa che non
     * arriveranno più coppie.
     */
    close(STDOUT_FILENO);
    LOG_INFO("processo mapper: pipe verso reducer chiusa");

    LOG_INFO("processo mapper terminato");
}
