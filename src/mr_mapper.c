#include "mr_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <threads.h>

/*
L'idea generale di questo file e definire la funzione emit che verra usata 
per scrivere nella pipe che andra al processo dei reducer in modo atomico
cosi da evitare race condition ed un volta fatto questo sfruttare le fuznioni 
che gia mi sono definito in mr_pipe.c e mr_queue.c per ricevere ed inviare righe 
e sincronizzare i thread con un modello produttore consumatore

ho deciso di definire le strucr e i prototipi delle fuznioni realtive ai thread
mapper direttamente in questo file perche questi prototipi saranno utilizzati solo
in questo file qui quidni sarebbe inutile scriverli da altre parti
*/


/*
Argomenti del thread lettore.
Il lettore legge righe da stdin e le inserisce nella coda
questa è la coda condivisa gestita con le funzioni implementate in mr_queue.c
 */
typedef struct {
    mr_queue_t *queue;   /* coda condivisa con i worker */
} mapper_reader_arg_t;

/*
Questa è la struttuta di una riga logica che poi i thread worker prenderanno
come input, la struttura di questa riga viene ricostruita dal thread lettore
prendendo le righe ed estrapolando i dati tramite mr_recv_line()
e la struttura della riga logica viene messa nella coda condivisa
grazie alla mr_queue_push()

Una riga letta da stdin dal thread reader, viene allocata e costruita
da esso e liberata poi dal thread worker che la analizza
 */
typedef struct {
    char         *file_name;      //nome del file sorgente
    size_t        file_name_len;
    unsigned long line_number;
    char         *line;           //contenuto della riga 
    size_t        line_len;
} mapper_line_t;

/*
 Argomenti di ciascun thread worker.
 Ogni worker condivide la stessa coda e lo stesso mutex di scrittura.
 questo è necessario per la sincronizzazione interna dei thread e  per
 la sicnronizzazione in scrittura dato che non l'ho implementata nelle funzioni
 in mr_pipe.c
 */
typedef struct {
    mr_queue_t  *queue;        // coda da cui estrarre le righe 
    mr_mapper_t  mapper_fn;    // callback mapper utente 
    void        *user_arg;
    mtx_t       *write_mutex;  // mutex condiviso per scrivere su stdout
} mapper_worker_arg_t;

/*
Implementazione della funzione emit che poi i thread worker usareanno per scrivere
l'obbiettivo di questa funzione è far si che grazie al mutex condiviso passato grazie
ad emit_arg di quando si chiama la funzione mapper dell'utente allora tutti i 
thread worker mapper possano scrivere sicronizzandosi con la solita mutex e
mandando le copie grazie alla funzione mr_send_pair()
*/

/*
 Questa struttura viene passata come emit_arg alla funzione mapper utente.
 Contiene tutto il necessario per scrivere la coppia su stdout.
 */
typedef struct {
    mtx_t *write_mutex;
}mapper_emit_arg_t;

/*
Funzione emit passata alla callback mapper utente.
Serializza la coppia <token, valore> e la scrive su stdout (pipe → reducer).
La scrittura è protetta dal mutex per evitare interleaving tra thread.
 */
static int mapper_emit(const char *token, const void *value,
                       size_t value_size, void *arg)
{
    mapper_emit_arg_t *ea = (mapper_emit_arg_t *)arg;

    size_t token_len = strlen(token);

    //Acquisisce il mutex: scrive l'intera coppia evitando race condition
    mtx_lock(ea->write_mutex);
    int ret = mr_send_pair(STDOUT_FILENO, token, token_len,
                           value, value_size);
    mtx_unlock(ea->write_mutex);

    return ret;
}



/*
Il THREAD LETTORE MAPPER legge righe serializzate da stdin (pipe dal processo principale),
ricostruisce una mapper_line_t (linee logiche) per ciascuna e la inserisce nella coda.
Quando stdin è esaurito (EOF), chiude la coda e termina.
*/
static int mapper_reader_main(void *arg)
{
    mapper_reader_arg_t *ra = (mapper_reader_arg_t *)arg;

    while(1) {
        mr_line_header_t hdr;
        char *file_name = NULL;
        char *line      = NULL;

        int ret = mr_recv_line(STDIN_FILENO, &hdr, &file_name, &line);
        if (ret == 1) break;   //EOF: nessuna altra riga
        if (ret < 0) {
            fprintf(stderr, "[mapper reader] errore lettura riga\n");
            break;
        }

        //Alloca la struttura per la riga logica e la mette in coda
        mapper_line_t *ml = malloc(sizeof(mapper_line_t));
        if (ml == NULL) {
            free(file_name);
            free(line);
            break;
        }
        ml->file_name     = file_name;
        ml->file_name_len = (size_t)hdr.file_name_len;
        ml->line_number   = hdr.line_number;
        ml->line          = line;
        ml->line_len      = (size_t)hdr.line_len;

        mr_queue_push(ra->queue, ml);
    }

    //qui chiude la coda in modo da segnalare i worker che non arrivano piu messaggi
    mr_queue_close(ra->queue);
    return 0;
}

/*
 Il THREAD WORKER MAPPER estrae righe dalla coda, invoca la funzione mapper utente su ciascuna,
 e termina quando la coda è vuota e chiusa.
 */
static int mapper_worker_main(void *arg)
{
    mapper_worker_arg_t *wa = (mapper_worker_arg_t *)arg;

    mapper_emit_arg_t ea;
    ea.write_mutex = wa->write_mutex;

    while(1){
        mapper_line_t *ml = (mapper_line_t *)mr_queue_pop(wa->queue);
        if (ml == NULL) break;   //coda vuota e chiusa: fine lavoro 

        //Ricostruisce la struttura pubblica mr_file_line_t (riga logica)
        mr_file_line_t fl;
        fl.file_name     = ml->file_name;
        fl.file_name_len = ml->file_name_len;
        fl.line_number   = ml->line_number;
        fl.line          = ml->line;
        fl.line_len      = ml->line_len;

        //Invoca la callback mapper utente 
        wa->mapper_fn(&fl, mapper_emit, &ea, wa->user_arg);

        // Libera la riga: il worker ne è responsabile dopo il pop 
        free(ml->file_name);
        free(ml->line);
        free(ml);
    }

    return 0;
}



/*
 mapper_process_main() verra chiamata dal processo mapper
 dopo che è stato creato (in seguito alla fork) e dopo che
 stdin e stdout sono stati modificati tramite dup2() per 
 ridirezzionarli rispettivamente alla pipe[0] di [Principale->Mapper]
 e pipe[1] di [Mapper->Reducer].
 
 Sequenza:
 1. inizializza la coda e il mutex di scrittura
 2. avvia il thread lettore
 3. avvia i thread worker
 4. attende la terminazione di tutti i thread
 5. chiude stdout di conseguenza segnala EOF al reducer
 */
void mapper_process_main(mr_mapper_t mapper_fn, void *user_arg,
                         size_t num_threads, size_t queue_size)
{
    //Inizializza la coda produttore-consumatore 
    mr_queue_t queue;
    if (mr_queue_init(&queue, queue_size) != 0) {
        fprintf(stderr, "[mapper] mr_queue_init fallita\n");
        return;
    }

    //Inizializza il mutex condiviso per la scrittura su stdout 
    mtx_t write_mutex;
    if (mtx_init(&write_mutex, mtx_plain) != thrd_success) {
        fprintf(stderr, "[mapper] mtx_init fallita\n");
        mr_queue_destroy(&queue);
        return;
    }

    //Avvia il thread lettore 
    mapper_reader_arg_t ra;
    ra.queue = &queue;

    thrd_t reader;
    thrd_create(&reader, mapper_reader_main, &ra);

    //Avvia i thread worker 
    thrd_t             *workers  = malloc(num_threads * sizeof(thrd_t));
    mapper_worker_arg_t *wargs   = malloc(num_threads * sizeof(mapper_worker_arg_t));

    for (size_t i = 0; i < num_threads; i++) {
        wargs[i].queue       = &queue;
        wargs[i].mapper_fn   = mapper_fn;
        wargs[i].user_arg    = user_arg;
        wargs[i].write_mutex = &write_mutex;
        thrd_create(&workers[i], mapper_worker_main, &wargs[i]);
    }

    //Attende il lettore 
    thrd_join(reader, NULL);

    //Attende tutti i worker 
    for (size_t i = 0; i < num_threads; i++)
        thrd_join(workers[i], NULL);

    // /Pulizia liberando le risorse
    free(workers);
    free(wargs);
    mtx_destroy(&write_mutex);
    mr_queue_destroy(&queue);

    /*
    Solo ora chiude stdout.
    Questo è il segnale di EOF per il processo reducer:
    tutte le coppie sono state scritte, nessuna andrà perduta.
    Perche chiudendo stdout si chiude il lato di scrittura della pipe 
    [Mapper->Reducer] cosi facendo il processo Reducer sa che non 
    arriverranno piu coppie <token,val>
     */
    close(STDOUT_FILENO);
}