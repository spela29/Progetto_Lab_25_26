#ifndef MR_H
#define MR_H

#include <stddef.h>

// struttura di unsecuzione del modello non nota all'utente
typedef struct mr *mr_t;

/*
attributi impostabili dall'utenete per la strutture di mapper
reducer e code di sincornizzazione dei thread e possibilita di 
impostare nome al file di log, se si inserice NULL il file usera
il nome default.
*/
typedef struct {
    size_t      mapper_threads;//numero di mapper
    size_t      reducer_threads;// numero di reducer
    size_t      queue_size; // lunghezza coda di sync threads
    const char *log_file;//nome file di log NULL = nome default
} mr_attr_t;

/*
struttura della righa logicha passata alla funzione mapper 

sia file_name che line sono puntatori validi soltanto durante l'invocazione
della fuznione mapper.

sia file_name che line non devono necessariamente finire con '\0'
*/
typedef struct {
    const char   *file_name;
    size_t        file_name_len;
    unsigned long line_number;
    const char   *line;
    size_t        line_len;
} mr_file_line_t;

/*
valore opaco associato ad un token

il framework non interpreta data siccome non fa assuznioni sui dati 
ma deve funzionare sempre indipendentemente dai essi infatti se size
vale 0 il dato potrebbe anche essere NULL
*/
typedef struct {
    const void *data;
    size_t      size;//lunghezza del dato in byte
} mr_value_t;

/*
funzione (dopo campo emit) presa in input e usata dal mapper che prende una stringa 
valida alfanumerica ASCII terminata con '\0'

funzione usata dal mapper per prudurre una coppia <token,value>

value è una sequenza opaca di byte di lunghezza value_size se value_size = 0
allora value puo essere NULL
*/
typedef int (*mr_emit_pair_t)(
    const char *token,
    const void *value,// puntatore al valore di ritorno valutato dalla funzione
    size_t      value_size,// grandezza del valore di ritorno 
    void       *emit_arg
);

/*
funzione (dopo campo emit) usata dal reducer per emettere il risultato finale 

il token emesso dal reducer deve essere come quello ricevuto
siccome deve emettere il risutato finale

se result_size = 0 allora il risultato puo essere NULL
*/
typedef int (*mr_emit_result_t)(
    const char *token,
    const void *result,
    size_t      result_size,
    void       *emit_arg
);

/*
funzione mapper che usa la funzione emit per produrre coppie <token,value>

token deve essere una stringa C valide ASCII terminata da '\0'

value e’ una sequenza opaca di byte di lunghezza value_size
Se value_size vale 0, value puo’ essere NULL.
*/
typedef int (*mr_mapper_t)(
    const mr_file_line_t *line,//puntatore a struct con riga da analizzare
    mr_emit_pair_t        emit,// funzione per l'emissione di <token,value>
    void                 *emit_arg,
    void                 *user_arg
);

/*
tipo della funzione reducer fornita dal programma utente 

questa funzione prende un token e tutti i risultati associati a quel token
e puo restituire zero o piu risultati usando la funzione emit

*/
typedef int (*mr_reducer_t)(
    const char       *token,//token usato dalla fun reducer
    const mr_value_t *values,// puntatore a struct con all'interno 
                             // i risultati associati al token
    size_t            values_count,// numero di value passati
    mr_emit_result_t  emit,//funzione per produrre l'output finale <token,value>
    void             *emit_arg,
    void             *user_arg
);

/* Inizializzazione e distruzione degli attributi */
int mr_attr_init(mr_attr_t *attr);//deve inizializzare gli attributi con valori default
                                  // validi in particolare i mapper e reducer devono
                                  // essere almeno 1
int mr_attr_destroy(mr_attr_t *attr);// distrugge gli attributi puntati da attr

/* Setter degli attributi – restituiscono -1 con errno=EINVAL se il valore non è valido */
int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n);
int mr_attr_set_log_file(mr_attr_t *attr, const char *path);

/*
la fuzione create crea un istanza di elaborazione del framework in mr

attr è un puntatore agli attrubuti per l'elaborazione del framwork

mapper e reducer sono rispettivamente le fuznioni mapper e reducer 

user_arg è un puntatore agli argomenti passati alle funzioni mapper e reducer
al framework non interessa e li tratta solo come un puntatore come una specie 
di corrire che consegna il pacco il framework consegna gli argomeni alle funzioni
mapper e reducer

dopo il ritorno con successo da mr_create l'utente puo distruggere o modificare 
gli attributi in attr senza modificare l'elaborazione gia creata perchè 
tutto il necessario è salvato in mr
*/
int mr_create(
    mr_t        *mr,
    const mr_attr_t *attr,
    mr_mapper_t  mapper,
    mr_reducer_t reducer,
    void        *user_arg
);

/*
 mr_start – avvia l'elaborazione (bloccante).
 input_path  : percorso di un file regolare o di una directory.
 output_path : percorso del file di output da produrre.
 Restituisce 0 al completamento, -1 in caso di errore.
 */
int mr_start(mr_t mr, const char *input_path, const char *output_path);

/* mr_destroy – rilascia le risorse associate all'elaborazione. */
int mr_destroy(mr_t mr);

#endif 
