#ifndef MR_INT_H
#define MR_INT_H

#include "mr.h"
#include <threads.h>
#include <stddef.h>
#include <sys/types.h>


//==========================SEZIONE_PIPE=========================================
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
e quindi chiamare queste funzioni wrapper che a sua volta useranno le readn() e writen()
che voglio definire come funzioni che leggono o scrivono n byte
qundi facedo cosi si creano piu livelli di astrazione 
*/


/*
legge esattamente n byte dal file descriptor fd nel buffer buf e si ferma in 3 casi
- trova EOF quidni la pipe è stat chiusa -> ritorno 0
- c'è stato un problema nella read e la read ritorna val < 0 -> ritorno -1
- ha finito di leggere tutti i byte richiesti -> ritorno numero byte letti 
*/
ssize_t readn(int fd, void *buf, size_t n);

/*
scrive esattemente n byte dal buffer buf al file puntato dal file descriptor fd
e si ferma in 2 casi
- errore nella scrittura allora la write ritorna un val < 0 -> ritorno -1
- finisce di scrivere tutti i byte -> ritorno il numero di byte scritti
*/
ssize_t writen(int fd, const void *buf, size_t n);

/* 
mr_send_line è quella funzione che si occupa di inviare dal processo principale 
tutte le informazione per la costruzione delle linee logiche ai thread mapper
si ferma in 2 casi:
- se abbiamo un'errore nella scrittura da parte di writen()-> ritorno -1 
- se abbiamo mandato tutti i byte richiesti -> ritorno 0 
*/
int mr_send_line(int fd,
                 const char *file_name, size_t file_name_len,
                 unsigned long line_number,
                 const char *line, size_t line_len);

/*
mr_recv_line e la funzione che fa coppia con mr_send_line perche è quella che si occupa 
della ricezione e dell'organizzazione delle informazione che poi serviranno 
ai thread worker mapper per la risoctruzione della riga logica prima dell'invio 
della stessa nella funzione mapper passata dal programma utente 
si ferma in 4 casi:
- se fallisce la readn() con un errore quindi ritornando un val < 0 -> ritorno -1
- se fallisce la readn() perche la pipe è chusa ritornado 0 -> ritorno 1
- se fallisce l'allocazione di memoria -> ritorno -1
- va tutto a buon fine -> ritorno 0
*/
int mr_recv_line(int fd, mr_line_header_t *hdr,
                 char **file_name_out, char **line_out);

/*
mr_send_pair ha lo stesso fuznionamanto della fuznione mr_send_pair perche 
quello che fanno in sostazna è la solita cosa prima mandano un header e dopo le 
informazioni necessarie 
*/
int mr_send_pair(int fd,
                 const char *token, size_t token_len,
                 const void *value, size_t value_len);

/*
mr_recv_pair ha lo stesso funzionamento in sostanza della funzione mr_recv_line 
perche prendono le informazioni lasciate su pipe dalla fuzione di invio e le danno
poi al chiamante che riscostruria e dati come piu opportuno e ci fara le operazioni sopra
pero questa a differenza di mr_recv_line ha un controllo in piu perche la lunghezza del
dato opaco puo essere 0 ma il principio è lo stesso
*/
int mr_recv_pair(int fd,
                 char **token_out, size_t *token_len_out,
                 void **value_out, size_t *value_len_out);

/* 
questa coppia di funzioni che va da Reducer->Principale funzionano
nello stesso modo di quelle sopra che vanno Mapper->Reducer
*/
int mr_send_result(int fd,
                   const char *token, size_t token_len,
                   const void *result, size_t result_len);
int mr_recv_result(int fd,
                   char **token_out,  size_t *token_len_out,
                   void **result_out, size_t *result_len_out);


//==========================SEZIONE_QUEUE=========================================
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
} mr_queue_t;


/*
questa funzione pernde in input il puntatore alla struct per la gestione della coda
e la capacita della coda, e inizializza i valori iniziali della coda
ritorna 0 se tutto è andato bene -1 altrimenti.
*/
int mr_queue_init(mr_queue_t* q,size_t num_items);

/*
questa funzione prende in input il puntatore alla struct per la gestione della 
coda e libera tutte le risorse usate da essa.
*/
int mr_queue_destroy(mr_queue_t* q);

/*
funzione che prende in input le info della coda e un nuovo item da inserire
e lo inserisce nella coda ritora 0 se è andato tutto bene -1 altrimenti
*/
int mr_queue_push(mr_queue_t* q,void* item);

/*
funzione che prende in input una coda e estrae l'elemento in coda
ritorna l'elemento se è andato tutto bene NULL altrimenti 
*/
void* mr_queue_pop(mr_queue_t* q);

/*
funzione che prende in input una coda e la chiude 
non ritorna
*/
void mr_queue_close(mr_queue_t* q);

//=======================SEZIONE_MAPPER=====================================
void mapper_process_main(mr_mapper_t mapper_fn, void *user_arg,
                         size_t num_threads, size_t queue_size);


//=======================SEZIONE_REDUCER=======================================

void reducer_process_main(mr_reducer_t reducer_fn, void *user_arg,
                          size_t num_threads, size_t queue_size);

//=======================SEZIONE_ANCORA_DA_IMPLEMENTARE=====================================
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


/*
Un gruppo raccoglie tutti i valori associati a uno stesso token,
pronti per essere passati alla funzione reducer utente.
*/
typedef struct {
    char        *token;         // stringa C terminata da '\0' 
    mr_value_t  *values;        //array di valori opachi 
    size_t       values_count;
} mr_group_t;

#endif