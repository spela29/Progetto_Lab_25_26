/*
 * mr_reducer.c
 *
 * Implementazione del processo reducer del framework libmr.
 *
 * Organizzazione interna:
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ Processo reducer                                            │
 *   │                                                             │
 *   │  Fase 1 – raccolta (single-thread):                         │
 *   │    un thread lettore legge tutte le coppie da stdin         │
 *   │    e le raggruppa in una hash-table per token               │
 *   │    (struttura: lista di gruppi ordinata per token)          │
 *   │                                                             │
 *   │  Fase 2 – riduzione (multithread):                          │
 *   │    i gruppi completati vengono inseriti in una coda         │
 *   │    N thread worker estraggono gruppi dalla coda,            │
 *   │    invocano la funzione reducer applicativa e               │
 *   │    scrivono i risultati su stdout (sincronizzato)           │
 *   │                                                             │
 *   │  Fase 3 – finalizzazione:                                   │
 *   │    il processo principale aspetta tutti i worker,           │
 *   │    poi chiude stdout per segnalare la fine al processo main │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Struttura dati per il raggruppamento:
 *   Una semplice hash-table open addressing con liste di collisione
 *   (separate chaining). La chiave è il token (stringa).
 *   Ogni bucket contiene una lista collegata di token_group_t.
 *
 * Output deterministico:
 *   I gruppi vengono ordinati lessicograficamente per token prima
 *   di essere inviati ai thread worker. In questo modo, a parità
 *   di input, l'output è identico tra esecuzioni diverse.
 *   Se il reducer emette più risultati per lo stesso token,
 *   l'ordine relativo è quello di emissione della funzione reducer.
 */

#include "mr_internal.h"


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <threads.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Hash-table per il raggruppamento                                    */
/* ------------------------------------------------------------------ */

#define HTABLE_INIT_BUCKETS 4096

/* Un singolo valore associato a un token */
typedef struct value_node {
    void             *data;
    size_t            size;
    struct value_node *next;
} value_node_t;

/* Gruppo: un token con tutti i suoi valori */
typedef struct token_group {
    char             *token;        /* stringa '\0'-terminata          */
    size_t            token_len;
    value_node_t     *values_head;  /* lista valori (ordine di arrivo) */
    value_node_t     *values_tail;
    size_t            values_count;
    struct token_group *ht_next;    /* catena nella hash-table         */
} token_group_t;

typedef struct {
    token_group_t **buckets;
    size_t          nbuckets;
    size_t          ngroups;
} htable_t;

static size_t ht_hash(const char *token, size_t nbuckets)
{
    /* DJB2 */
    size_t h = 5381;
    const unsigned char *p = (const unsigned char *)token;
    while (*p) { h = h * 33 + *p++; }
    return h % nbuckets;
}

static int ht_init(htable_t *ht, size_t nbuckets)
{
    ht->buckets = calloc(nbuckets, sizeof(token_group_t *));
    if (!ht->buckets) return -1;
    ht->nbuckets = nbuckets;
    ht->ngroups  = 0;
    return 0;
}

/* Trova o crea il gruppo per 'token'. */
static token_group_t *ht_get_or_create(htable_t *ht, const char *token,
                                        size_t token_len)
{
    size_t idx = ht_hash(token, ht->nbuckets);
    token_group_t *g = ht->buckets[idx];

    while (g) {
        if (strcmp(g->token, token) == 0) return g;
        g = g->ht_next;
    }

    /* Crea un nuovo gruppo */
    g = malloc(sizeof(token_group_t));
    if (!g) return NULL;
    g->token = malloc(token_len + 1);
    if (!g->token) { free(g); return NULL; }
    memcpy(g->token, token, token_len + 1);
    g->token_len    = token_len;
    g->values_head  = NULL;
    g->values_tail  = NULL;
    g->values_count = 0;
    g->ht_next      = ht->buckets[idx];
    ht->buckets[idx] = g;
    ht->ngroups++;
    return g;
}

/* Aggiunge un valore (copia dei byte) al gruppo */
static int group_add_value(token_group_t *g, const void *data, size_t size)
{
    value_node_t *vn = malloc(sizeof(value_node_t));
    if (!vn) return -1;
    vn->data = NULL;
    vn->size = size;
    vn->next = NULL;
    if (size > 0) {
        vn->data = malloc(size);
        if (!vn->data) { free(vn); return -1; }
        memcpy(vn->data, data, size);
    }
    if (g->values_tail) {
        g->values_tail->next = vn;
        g->values_tail = vn;
    } else {
        g->values_head = g->values_tail = vn;
    }
    g->values_count++;
    return 0;
}

/* Libera tutti i valori di un gruppo e poi il gruppo stesso */
static void group_free(token_group_t *g)
{
    value_node_t *v = g->values_head;
    while (v) {
        value_node_t *next = v->next;
        free(v->data);
        free(v);
        v = next;
    }
    free(g->token);
    free(g);
}

/* Raccoglie tutti i gruppi in un array (per l'ordinamento) */
static token_group_t **ht_to_array(htable_t *ht)
{
    token_group_t **arr = malloc(ht->ngroups * sizeof(token_group_t *));
    if (!arr) return NULL;
    size_t idx = 0;
    for (size_t i = 0; i < ht->nbuckets; i++) {
        token_group_t *g = ht->buckets[i];
        while (g) {
            arr[idx++] = g;
            g = g->ht_next;
        }
    }
    return arr;
}

static int cmp_groups(const void *a, const void *b)
{
    const token_group_t *ga = *(const token_group_t **)a;
    const token_group_t *gb = *(const token_group_t **)b;
    return strcmp(ga->token, gb->token);
}

static void ht_destroy(htable_t *ht)
{
    free(ht->buckets);
    ht->buckets  = NULL;
    ht->nbuckets = 0;
    ht->ngroups  = 0;
}

/* ------------------------------------------------------------------ */
/* Strutture per i thread worker del reducer                          */
/* ------------------------------------------------------------------ */

typedef struct {
    struct mr    *mr;
    mr_queue_t   *queue;
    int           stdout_fd;
    mtx_t        *write_mtx;
    atomic_long  *result_count;
} reducer_worker_arg_t;

/* ------------------------------------------------------------------ */
/* Funzione emit_result: chiamata dalla funzione reducer applicativa   */
/* ------------------------------------------------------------------ */

typedef struct {
    int          stdout_fd;
    mtx_t       *write_mtx;
    atomic_long *result_count;
} emit_result_ctx_t;

/*
 * emit_result_fn – scrive il risultato direttamente sulla pipe.
 * Il processo principale raccoglierà tutti i risultati e li
 * riordinerà lessicograficamente prima di scrivere il file di output.
 */
static int emit_result_fn(const char *token,
                           const void *result, size_t result_size,
                           void *emit_arg)
{
    emit_result_ctx_t *ctx = (emit_result_ctx_t *)emit_arg;

    int tok_len = (int)strlen(token);
    int res_len = (int)result_size;

    if (tok_len <= 0 || tok_len > MR_MAX_TOKEN_LEN) {
        errno = EINVAL; return -1;
    }
    if (res_len < 0 || res_len > MR_MAX_RESULT_LEN) {
        errno = EINVAL; return -1;
    }

    mtx_lock(ctx->write_mtx);
    int r = proto_write_result(ctx->stdout_fd,
                               token, tok_len,
                               result, res_len);
    mtx_unlock(ctx->write_mtx);

    if (r == 0)
        atomic_fetch_add(ctx->result_count, 1);

    return r;
}

/* ------------------------------------------------------------------ */
/* Thread worker reducer                                               */
/* ------------------------------------------------------------------ */

static int reducer_worker_main(void *arg)
{
    reducer_worker_arg_t *a = (reducer_worker_arg_t *)arg;

    LOG_INFO("reducer worker thread avviato");

    for (;;) {
        void *raw;
        int r = queue_pop(a->queue, &raw);
        if (r == 0) break;  /* coda chiusa e vuota */

        token_group_t *g = (token_group_t *)raw;

        /* Costruisce l'array mr_value_t per la funzione reducer */
        mr_value_t *vals = malloc(g->values_count * sizeof(mr_value_t));
        if (!vals) {
            LOG_ERROR("malloc vals fallita per token '%s'", g->token);
            group_free(g);
            continue;
        }

        size_t i = 0;
        value_node_t *vn = g->values_head;
        while (vn) {
            vals[i].data = vn->data;
            vals[i].size = vn->size;
            i++;
            vn = vn->next;
        }

        emit_result_ctx_t ctx = {
            .stdout_fd    = a->stdout_fd,
            .write_mtx    = a->write_mtx,
            .result_count = a->result_count,
        };

        int ret = a->mr->reducer(g->token,
                                  vals, g->values_count,
                                  emit_result_fn, &ctx,
                                  a->mr->user_arg);
        if (ret < 0) {
            LOG_WARN("funzione reducer ha restituito errore per token '%s'",
                     g->token);
        }

        free(vals);
        group_free(g);
    }

    LOG_INFO("reducer worker thread terminato");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Funzione principale del processo reducer                            */
/* ------------------------------------------------------------------ */

void reducer_process_main(struct mr *mr, const char *output_path)
{
    (void)output_path; /* usato solo dal processo principale */

    LOG_INFO("processo reducer avviato (PID %d)", (int)getpid());

    /* ── Fase 1: raccolta di tutte le coppie in memoria ─────────── */

    htable_t ht;
    if (ht_init(&ht, HTABLE_INIT_BUCKETS) < 0) {
        LOG_ERROR("ht_init fallita: %s", strerror(errno));
        _exit(1);
    }

    long pairs_received = 0;

    for (;;) {
        char *token = NULL;
        void *value = NULL;
        int   token_len, value_len;

        int r = proto_read_pair(STDIN_FILENO,
                                &token, &token_len,
                                &value, &value_len);
        if (r == 0) break;   /* EOF: fine coppie */
        if (r < 0) {
            LOG_ERROR("proto_read_pair fallita: %s", strerror(errno));
            /* Continuiamo a leggere per non bloccare il mapper */
            break;
        }

        token_group_t *g = ht_get_or_create(&ht, token, (size_t)token_len);
        if (!g) {
            LOG_ERROR("ht_get_or_create fallita per token '%s'", token);
            free(token); free(value);
            continue;
        }

        if (group_add_value(g, value, (size_t)value_len) < 0) {
            LOG_ERROR("group_add_value fallita");
        }

        free(token);
        free(value);
        pairs_received++;
    }

    LOG_INFO("reducer: ricevute %ld coppie, %zu token distinti",
             pairs_received, ht.ngroups);

    /* ── Fase 2: ordinamento e riduzione multithread ─────────────── */

    if (ht.ngroups == 0) {
        LOG_INFO("reducer: nessun gruppo da ridurre");
        ht_destroy(&ht);
        close(STDOUT_FILENO);
        return;
    }

    /* Raccoglie i gruppi e li ordina lessicograficamente per token */
    token_group_t **arr = ht_to_array(&ht);
    if (!arr) {
        LOG_ERROR("ht_to_array fallita: %s", strerror(errno));
        ht_destroy(&ht);
        _exit(1);
    }
    qsort(arr, ht.ngroups, sizeof(token_group_t *), cmp_groups);

    /* Coda e mutex per i thread worker */
    mr_queue_t queue;
    if (queue_init(&queue, mr->queue_size) < 0) {
        LOG_ERROR("queue_init reducer fallita: %s", strerror(errno));
        free(arr); ht_destroy(&ht); _exit(1);
    }

    mtx_t write_mtx;
    if (mtx_init(&write_mtx, mtx_plain) != thrd_success) {
        LOG_ERROR("mtx_init reducer fallita");
        queue_destroy(&queue); free(arr); ht_destroy(&ht); _exit(1);
    }

    atomic_long result_count = 0;

    /* Avvio thread worker */
    size_t n = mr->reducer_threads;
    thrd_t *workers = malloc(n * sizeof(thrd_t));
    reducer_worker_arg_t *wargs = malloc(n * sizeof(reducer_worker_arg_t));
    if (!workers || !wargs) {
        LOG_ERROR("malloc worker reducer fallita");
        free(workers); free(wargs);
        queue_destroy(&queue); mtx_destroy(&write_mtx);
        free(arr); ht_destroy(&ht); _exit(1);
    }

    for (size_t i = 0; i < n; i++) {
        wargs[i].mr           = mr;
        wargs[i].queue        = &queue;
        wargs[i].stdout_fd    = STDOUT_FILENO;
        wargs[i].write_mtx    = &write_mtx;
        wargs[i].result_count = &result_count;

        if (thrd_create(&workers[i], reducer_worker_main, &wargs[i])
                != thrd_success) {
            LOG_ERROR("thrd_create reducer worker %zu fallita", i);
            queue_close(&queue);
            for (size_t j = 0; j < i; j++) thrd_join(workers[j], NULL);
            free(workers); free(wargs);
            queue_destroy(&queue); mtx_destroy(&write_mtx);
            free(arr); ht_destroy(&ht); _exit(1);
        }
        LOG_INFO("reducer worker thread %zu creato", i);
    }

    /* Inserisce i gruppi ordinati nella coda */
    for (size_t i = 0; i < ht.ngroups; i++) {
        if (queue_push(&queue, arr[i]) < 0) {
            LOG_ERROR("queue_push reducer fallita all'elemento %zu", i);
            /* Il gruppo non sarà elaborato; lo liberiamo */
            group_free(arr[i]);
        }
    }

    /* Segnala la fine dei gruppi e attende i worker */
    queue_close(&queue);
    for (size_t i = 0; i < n; i++)
        thrd_join(workers[i], NULL);

    LOG_INFO("tutti i thread reducer terminati; risultati=%ld",
             (long)atomic_load(&result_count));

    free(workers);
    free(wargs);
    free(arr);
    queue_destroy(&queue);
    mtx_destroy(&write_mtx);
    ht_destroy(&ht);

    /*
     * Chiudiamo stdout (la pipe verso il processo principale).
     * Questo invia EOF al processo principale, che sa così che
     * non arriveranno altri risultati.
     */
    close(STDOUT_FILENO);
    LOG_INFO("processo reducer: pipe verso main chiusa");
    LOG_INFO("processo reducer terminato");
}
