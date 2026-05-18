#ifndef MR_INT_H
#define MR_INT_H

#include "mr.h"
#include <threads.h>
#include <stddef.h>

/*
questa struct mr_queue_info contiene la coda in items che è un array di puntatori
e tutte le strutture necessarie per la gestione di essa.
*/
typedef struct 
{
    void** items;//questo è il nostro array di puntatori agli oggitti
    size_t testa; // testa della coda dove inseriamo gli elementi 
    size_t coda; // coda della coda da dove estraiamo gli elementi
    size_t elementi; // elementi presenti nella cosa 
    size_t capacita;//capacita max della coda
    mtx_t mutex; // mutex per regolare l'accesso alla coda
    cnd_t not_full;
    cnd_t not_empty;
    int  close; //indiacatore 0 default 1 = non arrivano piu elementi nella coda
} mr_queue_info;

/*
questa funzione pernde in input il puntatore alla struct per la gestione della coda
e la capacita della coda, e inizializza i valori iniziali della coda
ritorna 0 se tutto è andato bene -1 altrimenti.
*/
int mr_queue_init(mr_queue_info* q,size_t num_items);

/*
questa funzione prende in input il puntatore alla struct per la gestione della 
coda e libera tutte le risorse usate da essa.
*/
int mr_queue_destroy(mr_queue_info* q);

/*
funzione che prende in input le info della coda e un nuovo item da inserire
e lo inserisce nella coda ritora 0 se è andato tutto bene -1 altrimenti
*/
int mr_queue_push(mr_queue_info* q,void* item);

/*
funzione che prende in input una coda e estrae l'elemento in coda
ritorna l'elemento se è andato tutto bene NULL altrimenti 
*/
void* mr_queue_pop(mr_queue_info* q);

/*
funzione che prende in input una coda e la chiude 
non ritorna
*/
void mr_queue_close(mr_queue_info* q);
#endif