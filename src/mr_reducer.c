#include "mr_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <threads.h>

/*
come nel file mr_mapper.c definisco direttamente qui le strutture usate soltato da questo file 
cosiche il file mr_internal.h non sia troppo pieno di dichiarazioni inutili


 qui volgio definire un gruppo, un gruppo raccoglie tutti i valori 
 associati a uno stesso token nel campo values che è appunto un array diamico di 
 valori opachi un gruppo viene costruito dal thread lettore e consumato dai thread worker.
 */
typedef struct {
    char       *token;         //stringa C terminata da '\0' 
    mr_value_t *values;        //array dinamico di valori opachi 
    size_t      values_count;  //numero di valori nell'array 
    size_t      values_cap;    // capacità attuale dell'array 
} reducer_group_t;

/*
Struttura usata per passare i risultati del thread lettore
al chiamante (reducer_process_main) quidni la coda sincronizzata
e principalemente la lista dei gruppi appena creati 
*/
typedef struct {
    mr_queue_t      *queue;
    reducer_group_t *groups;      //array dei gruppi costruiti
    size_t           group_count;
} reducer_reader_arg_t;

/*
Argomenti di ciascun thread worker.
*/
typedef struct {
    mr_queue_t   *queue;        //coda da cui estrarre i gruppi
    mr_reducer_t  reducer_fn;   // callback reducer utente 
    void         *user_arg;
    mtx_t        *write_mutex;  // mutex condiviso per scrivere su stdout
} reducer_worker_arg_t;

/*
Argomento della funzione emit passata al reducer utente uguale a quello della funzione
emit del mapper perche il principio è lo stesso.
 */
typedef struct {
    mtx_t *write_mutex;
} reducer_emit_arg_t;

/*
In questa sezione mi definisco e scrivo tutte le funzioni che mi servono per 
la gestione dei gruppi.
*/

/*
 Cerca un gruppo per token nell'array dei gruppi creati.
 Ritorna il puntatore al gruppo se lo trova, NULL altrimenti.
 */
static reducer_group_t *find_group(reducer_group_t *groups,//array dei gruppi
                                   size_t count,//lunghezza array gruppi
                                    const char *token//token da cercare
                                    )
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(groups[i].token, token) == 0)
            return &groups[i];
    return NULL;
}

/*
 Aggiunge un valore a un gruppo esistente copiando i byte grezzi 
 dalla memoria cosi non ci sono problemi ne di tipi ne di 
 interpretazione dei dati intermedi.
 Copia i byte del valore quindi il gruppo possiede la copia.
 Ritorna 0 in caso di successo, -1 in caso di errore.
 */
static int group_add_value(reducer_group_t *g,
                           const void *data, size_t size)
{
    //Espande l'array se necessario 
    if (g->values_count == g->values_cap) {
        size_t new_cap = g->values_cap == 0 ? 4 : g->values_cap * 2;
        mr_value_t *tmp = realloc(g->values,
                                  new_cap * sizeof(mr_value_t));
        if (tmp == NULL) return -1;
        g->values     = tmp;
        g->values_cap = new_cap;
    }

    //Copia il valore opaco 
    void *copy = NULL;
    if (size > 0) {
        copy = malloc(size);
        if (copy == NULL) return -1;
        memcpy(copy, data, size);
    }

    g->values[g->values_count].data = copy;
    g->values[g->values_count].size = size;
    g->values_count++;
    return 0;
}

/*
 Crea un nuovo gruppo per il token dato.
 Ritorna il puntatore al nuovo gruppo nell'array (dopo realloc),
 o NULL in caso di errore.
 Groups è un puntatore a puntatore perché realloc può spostare l'array
 ho bisogno di un doppio puntatore cosi che io possa modificare in caso 
 di realloc la posizione dell'array anche nel chiamate.
 */
static reducer_group_t *group_create(reducer_group_t **groups,
                                     size_t *count, size_t *cap,
                                     const char *token)
{
    //Espande l'array dei gruppi se necessario 
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 16 : *cap * 2;
        reducer_group_t *tmp = realloc(*groups,
                                       new_cap * sizeof(reducer_group_t));
        if (tmp == NULL) return NULL;
        *groups = tmp;
        *cap    = new_cap;
    }

    //Inizializza il nuovo gruppo 
    reducer_group_t *g = &(*groups)[*count];
    size_t tlen  = strlen(token);
    g->token     = malloc(tlen + 1);
    if (g->token == NULL) return NULL;
    memcpy(g->token, token, tlen + 1);
    g->values       = NULL;
    g->values_count = 0;
    g->values_cap   = 0;
    (*count)++;
    return g;
}

/*
Confronto per qsort: ordine lessicografico per token.
Garantisce output deterministico.
Si basa tutto su strcmp() che funziona solo se le stringhe 
finiscono con '\0' per questo ho aggiunto in fondo ad ogni 
token nelle funzioni di ricezione del file mr_pipe.c il carattere '\0'
 */
static int group_cmp(const void *a, const void *b)
{
    const reducer_group_t *ga = (const reducer_group_t *)a;
    const reducer_group_t *gb = (const reducer_group_t *)b;
    return strcmp(ga->token, gb->token);
}

/*
 * Libera tutta la memoria di un gruppo.
 */
static void group_free(reducer_group_t *g)
{
    for (size_t i = 0; i < g->values_count; i++)
        free((void *)g->values[i].data);//qui libero tutta la memoria puntata dai punatori data
    free(g->values);
    free(g->token);
}

/*
Qui definisco la funzione emit per i reducer che come funzionamento è identica
a quella dei mapper ma ho bisogno di 2 funzioni divere perche i mapper e i reducer
mandano 2 cosa diverse e quindi usano 2 funzioni diverese definite in mr_pipe.c
*/

static int reducer_emit(const char *token, const void *result,
                        size_t result_size, void *arg)
{
    reducer_emit_arg_t *ea = (reducer_emit_arg_t *)arg;

    size_t token_len = strlen(token);
//aquisico e il lock cosi da mandare i risultati evitando race condition
    mtx_lock(ea->write_mutex);
    int ret = mr_send_result(STDOUT_FILENO, token, token_len,
                             result, result_size);
    mtx_unlock(ea->write_mutex);

    return ret;
}


/*
Il thread lettore legge tutte le coppie da stdin e le raggruppa per token.
Solo dopo EOF inserisce i gruppi completi nella coda.

Qui si vede la differenza tra i mapper e i sorter perche oltre a quello che 
fanno a livello di valutazione dei dati si differenziano proprio perche 
mentre e mapper possono lavorare gia da subito i thread reducer
devono aspettare che la fase di raggruppamento dei riusltatu di uno
stesso token sia completata questa fase è chiamante di shuffle and sort
e questa fase nel mio codice è ricoperta dal thread lettore che prima legge 
tutto fa le sue operazioni e solo dopo inserisce nella struttura condivisa

Questo è il motivo per cui la fase di lettura è separata dalla
fase di elaborazione: non puoi chiamare il reducer su un token
finché non sai che hai ricevuto TUTTI i suoi valori.
 */
static int reducer_reader_main(void *arg)
{
    reducer_reader_arg_t *ra = (reducer_reader_arg_t *)arg;

    reducer_group_t *groups = NULL;
    size_t           count  = 0;
    size_t           cap    = 0;

    while(1) {
        char  *token   = NULL;
        void  *value   = NULL;
        size_t tok_len = 0;
        size_t val_len = 0;

        int ret = mr_recv_pair(STDIN_FILENO,
                               &token, &tok_len,
                               &value, &val_len);
        if (ret == 1) break;//pipe chiusa EOF
        if (ret < 0) {
            fprintf(stderr, "[reducer reader] errore lettura coppia\n");
            free(token);
            free(value);
            break;
        }

        reducer_group_t *g = find_group(groups, count, token);
        if (g == NULL)
            g = group_create(&groups, &count, &cap, token);

        if (g != NULL)
            group_add_value(g, value, val_len);

        free(token);
        free(value);
    }

    //Ordina lessicograficamente 
    if (count > 0)
        qsort(groups, count, sizeof(reducer_group_t), group_cmp);

    //Salva i risultati nella struttura condivisa 
    ra->groups      = groups;
    ra->group_count = count;

    //Inserisce i gruppi nella coda per i worker 
    for (size_t i = 0; i < count; i++)
        mr_queue_push(ra->queue, &groups[i]);

    mr_queue_close(ra->queue);
    return 0;
}

/*
Struttura del thread worker reducer
*/

static int reducer_worker_main(void *arg)
{
    reducer_worker_arg_t *wa = (reducer_worker_arg_t *)arg;

    reducer_emit_arg_t ea;
    ea.write_mutex = wa->write_mutex;

    while(1) {
        reducer_group_t *g = (reducer_group_t *)mr_queue_pop(wa->queue);
        if (g == NULL) break;   //coda vuota e chiusa

        // Invoca la callback reducer utente con il gruppo completo
        //producendo quinid un riusltato che verra inviato da emit tramite mr_send_result()
        wa->reducer_fn(g->token,
                       g->values,
                       g->values_count,
                       reducer_emit, &ea,
                       wa->user_arg);
    }

    return 0;
}

/*
questa funzione verra ciamata dal processo reducer dopo che sara stato creato
con la fork() e dopo che con le dup2 saranno stati reindirizzati stdin e stdout
alle pipe corrispondenti ovvero lettura di [Mapper->Reducer] e scrittura di [Reducer->Principale]
*/

void reducer_process_main(mr_reducer_t reducer_fn, void *user_arg,
                          size_t num_threads, size_t queue_size)
{
    mr_queue_t queue;
    if (mr_queue_init(&queue, queue_size) != 0) {
        fprintf(stderr, "[reducer] mr_queue_init fallita\n");
        return;
    }

    mtx_t write_mutex;
    if (mtx_init(&write_mutex, mtx_plain) != thrd_success) {
        fprintf(stderr, "[reducer] mtx_init fallita\n");
        mr_queue_destroy(&queue);
        return;
    }

    //Avvia il thread lettore 
    reducer_reader_arg_t ra;
    ra.queue       = &queue;
    ra.groups      = NULL;
    ra.group_count = 0;

    thrd_t reader;
    thrd_create(&reader, reducer_reader_main, &ra);

    //Avvia i thread worker 
    thrd_t              *workers = malloc(num_threads * sizeof(thrd_t));
    reducer_worker_arg_t *wargs  = malloc(num_threads * sizeof(reducer_worker_arg_t));

    for (size_t i = 0; i < num_threads; i++) {
        wargs[i].queue       = &queue;
        wargs[i].reducer_fn  = reducer_fn;
        wargs[i].user_arg    = user_arg;
        wargs[i].write_mutex = &write_mutex;
        thrd_create(&workers[i], reducer_worker_main, &wargs[i]);
    }

    // Attende il lettore 
    thrd_join(reader, NULL);

    //Attende tutti i worker 
    for (size_t i = 0; i < num_threads; i++)
        thrd_join(workers[i], NULL);

    //Libera i gruppi siccome i worker hanno già finito di usarli 
    if (ra.groups != NULL) {
        for (size_t i = 0; i < ra.group_count; i++)
            group_free(&ra.groups[i]);
        free(ra.groups);
    }

    //Pulizia 
    free(workers);
    free(wargs);
    mtx_destroy(&write_mutex);
    mr_queue_destroy(&queue);

    /*
    Chiude stdout solo dopo che tutti i worker hanno terminato.
    Questo è il segnale di EOF per il processo principale.
     */
    close(STDOUT_FILENO);
}