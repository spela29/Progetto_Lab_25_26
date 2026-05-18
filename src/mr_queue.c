#include "mr_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
/*
file sorgente per la gestione di code con modello produttore consumatore
*/
int mr_queue_init(mr_queue_t* q,size_t capacita){
    if(capacita == 0){
        errno = EINVAL;
        perror("errore capaicta coda <1\n");
        return -1;
    }
    if((q->items = malloc(capacita*sizeof(void*))) == NULL){
        perror("errore nell'alocazione della coda");
        return -1;
    }
    q->testa = 0;
    q->coda = 0;
    q->elementi = 0;
    q->capacita = capacita;
    if(mtx_init(&q->mutex,mtx_plain) != thrd_success){
        free(q->items);
        errno = EINVAL;
       perror("errore nell'inizializzazione del mutex\n");
       return -1;
    }
    if(cnd_init(&q->not_full)!=thrd_success){
        free(q->items);
        mtx_destroy(&q->mutex);
        errno = EINVAL;
        perror("errore nell'inizializzazione della cnd not_full\n");
        return -1;
    }
    if(cnd_init(&q->not_empty)!=thrd_success){
        free(q->items);
        mtx_destroy(&q->mutex);
        cnd_destroy(&q->not_full);
        errno = EINVAL;
        perror("errore nell'inizializzazione della cnd not_empty\n");
        return -1;
    }
    q->close = 0;
    return 0;
}

int mr_queue_destroy(mr_queue_t* q){
    free(q->items);
    q->items = NULL;
    mtx_destroy(&q->mutex);
    cnd_destroy(&q->not_full);
    cnd_destroy(&q->not_empty);
    return 0;
}

int mr_queue_push(mr_queue_t* q,void* item){
    mtx_lock(&q->mutex);
    while(q->elementi == q->capacita && q->close == 0){
        cnd_wait(&q->not_full,&q->mutex);
    }
    if(q->close == 1){
        mtx_unlock(&q->mutex);
        return -1;
    }
    q->items[q->coda] = item;
    q->coda = (q->coda + 1) % q->capacita;
    q->elementi++;
    cnd_broadcast(&q->not_empty);
    mtx_unlock(&q->mutex);
    return 0;
}

void* mr_queue_pop(mr_queue_t* q){
    mtx_lock(&q->mutex);
    while(q->elementi == 0 && q->close == 0){
        cnd_wait(&q->not_empty,&q->mutex);
    }
    if(q->close == 1){
        mtx_unlock(&q->mutex);
        return NULL;
    }
    void* item = q->items[q->coda];
    q->coda = (q->coda + 1) % q->capacita;
    mtx_unlock(&q->mutex);
    return item;

}

void mr_queue_close(mr_queue_t* q){
    mtx_lock(&q->mutex);
    q->close = 1;
    cnd_broadcast(&q->not_empty);
    cnd_broadcast(&q->not_full);
    mtx_unlock(&q->mutex);
    
}

