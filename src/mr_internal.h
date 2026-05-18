#ifndef MR_INT_H
#define MR_INT_H

#include "mr.h"
#include <threads.h>
#include <stddef.h>
#include <sys/types.h>

/*
questa struct detta la struttura interna di un'elaboriazione 
(struct mr)
*/
struct mr
{
    mr_attr_t attr;//copia gli attributi di config
    mr_mapper_t mapper;//callback mapper fornita dall'utente
    mr_reducer_t reducer;//callback reducer fornita dall'utente
    void* user_arg;// argomento opaco passato alle callback
};


//==========================SEZIONE-PIPE=========================================
/*
imposto i limiti ragionevoli per gli header passati su pipe 
*/
#define MR_MAX_TOKEN_LEN (1024) //1KB
#define MR_MAX_VALUE_LEN (1024*1024) //1 MB
#define MR_MAX_LINE_LEN (1024*1024)// 1MB
#define MR_MAX_FNAME_LEN (4096) //4KB

/*
ora voglio definire delle struct che rapresentano gli header dei 
messaggi scritti su pipe cosi che quando si va a leggere da pipe
allora il proscesso che legge sa esattamente cosa legge e quanto 
deve leggere
*/

/*
struct che rappresenta l'header che va sulla pipe
[principale -> mapper]
*/
typedef struct {
    int           file_name_len;// lunghezza nome file, senza '\0' 
    unsigned long line_number; //numero di riga
    int           line_len;    //lunghezza contenuto riga, senza '\n' 
} mr_line_header_t;


/*
struct che rappresenta l'header che va sulla pipe
[mapper -> reducer]
*/
typedef struct {
    int token_len;  //lunghezza token senza '\0' 
    int value_len; //lunghezza in byte del val opaco nessun terminatore
} mr_pair_header_t;

/*
struct che rappresenta l'header che va sulla pipe
[reducer-> principale (risultato)]
*/
typedef struct {
    int token_len;//lunghezza token senza '\0' 
    int result_len;//lunghezza valore dopo la valutazione del reducer
} mr_result_header_t;


/*
adesso voglio definire i prototipi delle funzioni che uso nel file sorgente 
mr_pipe.c e voglio definire delle funzioni che fanno praticamente da wrapper
per le fuznioni readn() e writen() cosi da sfruttare gli header appena definiti
e le entita del mio framework che voglioni scrivere e leggere su pipe devono appoggiarsi
e quidni chiamare queste funzioni wrapper che a sua volta useranno le readn() e writen()
qundi facedo cosi si creano piu livelli di astrazione 
*/

/* 
Livello basso 
*/
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

/* 
Principale -> Mapper 
*/
int mr_send_line(int fd,
                 const char *file_name, size_t file_name_len,
                 unsigned long line_number,
                 const char *line, size_t line_len);
int mr_recv_line(int fd, mr_line_header_t *hdr,
                 char **file_name_out, char **line_out);

/* 
Mapper -> Reducer 
*/
int mr_send_pair(int fd,
                 const char *token, size_t token_len,
                 const void *value, size_t value_len);
int mr_recv_pair(int fd,
                 char **token_out, size_t *token_len_out,
                 void **value_out, size_t *value_len_out);

/* 
Reducer -> Principale 
*/
int mr_send_result(int fd,
                   const char *token, size_t token_len,
                   const void *result, size_t result_len);
int mr_recv_result(int fd,
                   char **token_out,  size_t *token_len_out,
                   void **result_out, size_t *result_len_out);


//==========================SEZIONE-QUEUE=========================================
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