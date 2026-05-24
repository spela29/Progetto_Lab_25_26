#include "mr_internal.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>

int queue_init(mr_queue_t *q, size_t cap)
{
    if (!q || cap == 0) { errno = EINVAL; return -1; }

    q->buf = malloc(cap * sizeof(void *));
    if (!q->buf) return -1;

    q->cap    = cap;
    q->head   = 0;
    q->tail   = 0;
    q->count  = 0;
    q->closed = 0;

    if (mtx_init(&q->mtx, mtx_plain) != thrd_success) {
        free(q->buf); errno = ENOMEM; return -1;
    }
    if (cnd_init(&q->not_full) != thrd_success) {
        mtx_destroy(&q->mtx); free(q->buf); errno = ENOMEM; return -1;
    }
    if (cnd_init(&q->not_empty) != thrd_success) {
        cnd_destroy(&q->not_full); mtx_destroy(&q->mtx);
        free(q->buf); errno = ENOMEM; return -1;
    }
    return 0;
}

void queue_destroy(mr_queue_t *q)
{
    if (!q) return;
    cnd_destroy(&q->not_empty);
    cnd_destroy(&q->not_full);
    mtx_destroy(&q->mtx);
    free(q->buf);
    q->buf = NULL;
}

int queue_push(mr_queue_t *q, void *item)
{
    mtx_lock(&q->mtx);

    /* Attende finché c'è spazio o la coda è chiusa */
    while (q->count == q->cap && !q->closed) {
        cnd_wait(&q->not_full, &q->mtx);
    }

    if (q->closed) {
        mtx_unlock(&q->mtx);
        errno = EPIPE;
        return -1; 
    }

    q->buf[q->tail] = item;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    cnd_signal(&q->not_empty);
    mtx_unlock(&q->mtx);
    return 0;
}

int queue_pop(mr_queue_t *q, void **item)
{
    mtx_lock(&q->mtx);

    /* Attende finché ci sono elementi o la coda è chiusa */
    while (q->count == 0 && !q->closed) {
        cnd_wait(&q->not_empty, &q->mtx);
    }

    if (q->count == 0) {
        /* Coda chiusa e vuota: segnale di fine */
        mtx_unlock(&q->mtx);
        *item = NULL;
        return 0;
    }

    *item = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;

    cnd_signal(&q->not_full);
    mtx_unlock(&q->mtx);
    return 1;
}

void queue_close(mr_queue_t *q)
{
    mtx_lock(&q->mtx);
    q->closed = 1;
    cnd_broadcast(&q->not_empty);
    cnd_broadcast(&q->not_full);
    mtx_unlock(&q->mtx);
}
