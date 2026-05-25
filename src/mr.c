/*
 * mr.c
 *
 * Modulo principale del framework libmr.
 *
 * Implementa:
 *   - mr_attr_init / mr_attr_destroy / mr_attr_set_*
 *   - mr_create / mr_start / mr_destroy
 *
 
 */

#include "mr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Valori di default                                                   */
/* ------------------------------------------------------------------ */
#define DEFAULT_MAPPER_THREADS  2
#define DEFAULT_REDUCER_THREADS 2
#define DEFAULT_QUEUE_SIZE      64
#define DEFAULT_LOG_FILE        "mr.log"

/* ------------------------------------------------------------------ */
/* mr_attr_*                                                           */
/* ------------------------------------------------------------------ */

int mr_attr_init(mr_attr_t *attr)
{
    if (!attr) { errno = EINVAL; return -1; }
    attr->mapper_threads  = DEFAULT_MAPPER_THREADS;
    attr->reducer_threads = DEFAULT_REDUCER_THREADS;
    attr->queue_size      = DEFAULT_QUEUE_SIZE;
    attr->log_file        = NULL;
    return 0;
}

int mr_attr_destroy(mr_attr_t *attr)
{
    /* log_file punta a memoria dell'utente, non la liberiamo */

    (void)attr;
    return 0;
}

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n)
{
    if (!attr || n == 0) { errno = EINVAL; return -1; }
    attr->mapper_threads = n;
    return 0;
}

int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n)
{
    if (!attr || n == 0) { errno = EINVAL; return -1; }
    attr->reducer_threads = n;
    return 0;
}

int mr_attr_set_queue_size(mr_attr_t *attr, size_t n)
{
    if (!attr || n == 0) { errno = EINVAL; return -1; }
    attr->queue_size = n;
    return 0;
}

int mr_attr_set_log_file(mr_attr_t *attr, const char *path)
{
    if (!attr) { errno = EINVAL; return -1; }
    attr->log_file = path;   /* puntatore dell'utente, non copiato qui */
    return 0;
}

/* ------------------------------------------------------------------ */
/* mr_create                                                           */
/* ------------------------------------------------------------------ */

int mr_create(mr_t *mr_out, const mr_attr_t *attr,
              mr_mapper_t mapper, mr_reducer_t reducer, void *user_arg)
{
    if (!mr_out || !attr || !mapper || !reducer) {
        errno = EINVAL; return -1;
    }

    struct mr *mr = malloc(sizeof(struct mr));
    if (!mr) return -1;

    mr->mapper_threads  = attr->mapper_threads;
    mr->reducer_threads = attr->reducer_threads;
    mr->queue_size      = attr->queue_size;
    mr->mapper          = mapper;
    mr->reducer         = reducer;
    mr->user_arg        = user_arg;

    /* Copia la stringa del file di log */
    if (attr->log_file) {
        mr->log_file = strdup(attr->log_file);
        if (!mr->log_file) { free(mr); return -1; }
    } else {
        mr->log_file = strdup(DEFAULT_LOG_FILE);
        if (!mr->log_file) { free(mr); return -1; }
    }

    *mr_out = mr;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mr_destroy                                                          */
/* ------------------------------------------------------------------ */

int mr_destroy(mr_t mr)
{
    if (!mr) { errno = EINVAL; return -1; }
    free(mr->log_file);
    free(mr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scansione input                                                     */
/* ------------------------------------------------------------------ */

/* Confronto per qsort di stringhe */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/*
 * Raccoglie i percorsi dei file da elaborare in ordine lessicografico.
 * Se input_path è un file regolare, restituisce solo quello.
 * Se è una directory, restituisce i file regolari in ordine lessicografico.
 *
 * *paths_out viene allocato (array di strdup). Il chiamante deve
 * liberarlo con free_paths().
 * Restituisce il numero di file, o -1 in caso di errore.
 */
static int collect_input_files(const char *input_path,
                               char ***paths_out)
{
    struct stat st;
    if (stat(input_path, &st) < 0) return -1;

    if (S_ISREG(st.st_mode)) {
        /* Singolo file */
        char **arr = malloc(sizeof(char *));
        if (!arr) return -1;
        arr[0] = strdup(input_path);
        if (!arr[0]) { free(arr); return -1; }
        *paths_out = arr;
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(input_path);
        if (!d) return -1;

        char **arr = NULL;
        size_t count = 0, cap = 0;
        struct dirent *de;

        while ((de = readdir(d)) != NULL) {
            /* Salta . e .. */
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) continue;

            /* Costruisce il percorso completo */
            size_t dir_len  = strlen(input_path);
            size_t name_len = strlen(de->d_name);
            char *full = malloc(dir_len + 1 + name_len + 1);
            if (!full) { closedir(d); goto oom; }
            memcpy(full, input_path, dir_len);
            full[dir_len] = '/';
            memcpy(full + dir_len + 1, de->d_name, name_len + 1);

            /* Controlla con stat se è file regolare */
            struct stat fs;
            if (stat(full, &fs) < 0) { free(full); continue; }
            if (!S_ISREG(fs.st_mode)) { free(full); continue; }

            /* Rialloca se necessario */
            if (count == cap) {
                size_t new_cap = cap ? cap * 2 : 16;
                char **tmp = realloc(arr, new_cap * sizeof(char *));
                if (!tmp) { free(full); closedir(d); goto oom; }
                arr = tmp; cap = new_cap;
            }
            arr[count++] = full;
        }
        closedir(d);

        /* Ordine lessicografico per nome file (non percorso completo,
         * ma il percorso completo è identico a meno del prefisso) */
        if (count > 1)
            qsort(arr, count, sizeof(char *), cmp_str);

        *paths_out = arr;
        return (int)count;

    oom:
        for (size_t i = 0; i < count; i++) free(arr[i]);
        free(arr);
        return -1;
    }

    errno = EINVAL;
    return -1;
}

static void free_paths(char **paths, int n)
{
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

/* ------------------------------------------------------------------ */
/* Invio righe al mapper                                               */
/* ------------------------------------------------------------------ */

static int send_file_to_mapper(int fd, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        LOG_ERROR("impossibile aprire '%s': %s", filepath, strerror(errno));
        return -1;
    }
    LOG_INFO("apertura file input '%s'", filepath);

    size_t  fname_len = strlen(filepath);
    char   *line_buf  = NULL;
    size_t  line_cap  = 0;
    ssize_t line_len;
    unsigned long line_num = 0;

    while ((line_len = getline(&line_buf, &line_cap, f)) != -1) {
        line_num++;

        /* Rimuove il '\n' finale, se presente */
        if (line_len > 0 && line_buf[line_len - 1] == '\n') {
            line_len--;
        }

        mr_file_line_t fl = {
            .file_name     = filepath,
            .file_name_len = fname_len,
            .line_number   = line_num,
            .line          = line_buf,
            .line_len      = (size_t)line_len,
        };

        if (proto_write_line(fd, &fl) < 0) {
            LOG_ERROR("proto_write_line fallita per '%s' riga %lu: %s",
                      filepath, line_num, strerror(errno));
            free(line_buf);
            fclose(f);
            return -1;
        }
    }

    free(line_buf);
    LOG_INFO("chiusura file input '%s' (%lu righe)", filepath, line_num);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Raccolta risultati e scrittura output                               */
/* ------------------------------------------------------------------ */

/*
 * Struttura per un singolo risultato raccolto dalla pipe.
 * Il processo principale accumula tutti i risultati in un array,
 * li ordina lessicograficamente per token con qsort, poi li scrive
 * nel file di output in ordine deterministico.
 * Questo approccio è più robusto rispetto all'ordinamento nel reducer
 * perché non dipende dall'ordine di scrittura dei thread worker.
 */
typedef struct {
    char *token;
    int   token_len;
    void *result;
    int   result_len;
} output_record_t;

static int cmp_output_records(const void *a, const void *b)
{
    const output_record_t *ra = (const output_record_t *)a;
    const output_record_t *rb = (const output_record_t *)b;
    return strcmp(ra->token, rb->token);
}

static int collect_output(int fd, const char *output_path)
{
    /* Fase 1: raccoglie tutti i risultati dalla pipe in un array */
    output_record_t *records = NULL;
    size_t count = 0, cap = 0;

    while(1) {
        char *token  = NULL;
        void *result = NULL;
        int   token_len, result_len;

        int r = proto_read_result(fd, &token, &token_len,
                                  &result, &result_len);
        if (r == 0) break;   /* EOF */
        if (r < 0) {
            LOG_ERROR("proto_read_result fallita: %s", strerror(errno));
            /* Libera i record già raccolti */
            for (size_t i = 0; i < count; i++) {
                free(records[i].token);
                free(records[i].result);
            }
            free(records);
            return -1;
        }

        /* Rialloca l'array se necessario */
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 64;
            output_record_t *tmp = realloc(records,
                                           new_cap * sizeof(output_record_t));
            if (!tmp) {
                free(token); free(result);
                for (size_t i = 0; i < count; i++) {
                    free(records[i].token);
                    free(records[i].result);
                }
                free(records);
                return -1;
            }
            records = tmp;
            cap = new_cap;
        }

        records[count].token      = token;
        records[count].token_len  = token_len;
        records[count].result     = result;
        records[count].result_len = result_len;
        count++;
    }

    LOG_INFO("raccolti %zu risultati dalla pipe, avvio ordinamento", count);

    /* Fase 2: ordina i record lessicograficamente per token */
    if (count > 1)
        qsort(records, count, sizeof(output_record_t), cmp_output_records);

    /* Fase 3: scrive i record ordinati nel file di output */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        LOG_ERROR("impossibile aprire output '%s': %s",
                  output_path, strerror(errno));
        for (size_t i = 0; i < count; i++) {
            free(records[i].token);
            free(records[i].result);
        }
        free(records);
        return -1;
    }
    LOG_INFO("apertura file output '%s'", output_path);

    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        /*
         * Scrittura del record di output:
         *   [int token_len][token][int result_len][result]
         */
        int tl = records[i].token_len;
        int rl = records[i].result_len;
        if (fwrite(&tl, sizeof(tl), 1, out) != 1 ||
            fwrite(records[i].token, (size_t)tl, 1, out) != 1 ||
            fwrite(&rl, sizeof(rl), 1, out) != 1 ||
            (rl > 0 &&
             fwrite(records[i].result, (size_t)rl, 1, out) != 1)) {
            LOG_ERROR("scrittura output fallita al record %zu", i);
            ret = -1;
            break;
        }
    }

    fclose(out);
    LOG_INFO("file output '%s' chiuso (%zu risultati scritti)",
             output_path, count);

    /* Libera tutti i record */
    for (size_t i = 0; i < count; i++) {
        free(records[i].token);
        free(records[i].result);
    }
    free(records);

    return ret;
}

/* ------------------------------------------------------------------ */
/* mr_start                                                            */
/* ------------------------------------------------------------------ */

int mr_start(mr_t mr, const char *input_path, const char *output_path)
{
    if (!mr || !input_path || !output_path) {
        errno = EINVAL; return -1;
    }

    /* Apriamo il log nel processo principale */
    if (log_open(mr->log_file) < 0) {
        /* Non fatale: continuiamo senza log */
        fprintf(stderr, "libmr: impossibile aprire log '%s': %s\n",
                mr->log_file, strerror(errno));
    }

    LOG_INFO("mr_start avviato: input='%s' output='%s'",
             input_path, output_path);
    LOG_INFO("configurazione: mapper_threads=%zu reducer_threads=%zu "
             "queue_size=%zu",
             mr->mapper_threads, mr->reducer_threads, mr->queue_size);

    /* Raccolta file di input */
    char **input_files = NULL;
    int n_files = collect_input_files(input_path, &input_files);
    if (n_files < 0) {
        LOG_ERROR("collect_input_files fallita per '%s': %s", input_path, strerror(errno));
        log_close();
        return -1;
    }
    LOG_INFO("file da elaborare: %d", n_files);

    /*---Creazione pipe ------------------ */
    int main_to_mapper[2];
    int mapper_to_reducer[2];
    int reducer_to_main[2];

    if (pipe(main_to_mapper) < 0 ||
        pipe(mapper_to_reducer) < 0 ||
        pipe(reducer_to_main) < 0) {
        LOG_ERROR("pipe() fallita: %s", strerror(errno));
        free_paths(input_files, n_files);
        log_close();
        return -1;
    }
    LOG_INFO("pipe create: main→mapper(%d,%d) mapper→reducer(%d,%d) "
             "reducer→main(%d,%d)",
             main_to_mapper[0], main_to_mapper[1],
             mapper_to_reducer[0], mapper_to_reducer[1],
             reducer_to_main[0], reducer_to_main[1]);

    /*----  fork() processo mapper------------ */
    pid_t mapper_pid = fork();
    if (mapper_pid < 0) {
        LOG_ERROR("fork mapper fallita: %s", strerror(errno));
        close(main_to_mapper[0]);   close(main_to_mapper[1]);
        close(mapper_to_reducer[0]); close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);   close(reducer_to_main[1]);
        free_paths(input_files, n_files);
        log_close();
        return -1;
    }

    if (mapper_pid == 0) {
        /* ------- Processo mapper -------------- */

        /* Collega stdin alla pipe proveniente dal principale */
        if (dup2(main_to_mapper[0], STDIN_FILENO) < 0) _exit(1);
        /* Collega stdout alla pipe verso il reducer */
        if (dup2(mapper_to_reducer[1], STDOUT_FILENO) < 0) _exit(1);

        /* Chiude tutti i descrittori non necessari */
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_reducer[0]);
        close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);
        close(reducer_to_main[1]);

        /* Apre il log per questo processo */
        log_open(mr->log_file);
        LOG_INFO("processo mapper: pipe e stdio configurati");

        mapper_process_main(mr);
        log_close();
        _exit(0);
    }
    LOG_INFO("processo mapper creato (PID %d)", (int)mapper_pid);

    /*----fork() processo reducer ----*/
    pid_t reducer_pid = fork();
    if (reducer_pid < 0) {
        LOG_ERROR("fork reducer fallita: %s", strerror(errno));
        close(main_to_mapper[0]);   close(main_to_mapper[1]);
        close(mapper_to_reducer[0]); close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);   close(reducer_to_main[1]);
        free_paths(input_files, n_files);
        /* Aspettiamo il mapper prima di uscire */
        waitpid(mapper_pid, NULL, 0);
        log_close();
        return -1;
    }

    if (reducer_pid == 0) {
        /* -- Processo reducer ----*/

        /* Collega stdin alla pipe proveniente dal mapper */
        if (dup2(mapper_to_reducer[0], STDIN_FILENO) < 0) _exit(1);
        /* Collega stdout alla pipe verso il processo principale */
        if (dup2(reducer_to_main[1], STDOUT_FILENO) < 0) _exit(1);

        /* Chiude tutti i descrittori non necessari */
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_reducer[0]);
        close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);
        close(reducer_to_main[1]);

        log_open(mr->log_file);
        LOG_INFO("processo reducer: pipe e stdio configurati");

        reducer_process_main(mr, output_path);
        log_close();
        _exit(0);
    }
    LOG_INFO("processo reducer creato (PID %d)", (int)reducer_pid);

    /* -- Processo principale: chiude i lati non necessari ---- */
    close(main_to_mapper[0]);    /* Non leggiamo da questa pipe      */
    close(mapper_to_reducer[0]); /* Non leggiamo qui (è del mapper)  */
    close(mapper_to_reducer[1]); /* Non scriviamo qui (è del mapper) */
    close(reducer_to_main[1]);   /* Non scriviamo qui (è del reducer)*/

    /* Ora il processo principale ha aperto solo:
     *   main_to_mapper[1]  (scrittura verso mapper)
     *   reducer_to_main[0] (lettura dal reducer)
     */

    /* -- Invio righe al mapper --- */
    int send_ok = 0;
    for (int i = 0; i < n_files; i++) {
        if (send_file_to_mapper(main_to_mapper[1], input_files[i]) < 0) {
            LOG_ERROR("errore nell'invio del file '%s'", input_files[i]);
            send_ok = -1;
            /* Continuiamo per non lasciare il mapper appeso */
        }
    }

    /* Chiude la pipe verso il mapper: segnala EOF */
    close(main_to_mapper[1]);
    LOG_INFO("invio righe completato; pipe verso mapper chiusa");

    /* -- Raccolta risultati dal reducer --*/
    int collect_ok = collect_output(reducer_to_main[0], output_path);
    close(reducer_to_main[0]);
    LOG_INFO("raccolta risultati completata");

    /* -- Attesa terminazione processi figli--*/
    int mapper_status = 0, reducer_status = 0;
    if (waitpid(mapper_pid, &mapper_status, 0) < 0) {
        LOG_ERROR("waitpid mapper fallita: %s", strerror(errno));
    } else {
        LOG_INFO("processo mapper terminato (status %d)",
                 WEXITSTATUS(mapper_status));
    }
    if (waitpid(reducer_pid, &reducer_status, 0) < 0) {
        LOG_ERROR("waitpid reducer fallita: %s", strerror(errno));
    } else {
        LOG_INFO("processo reducer terminato (status %d)",
                 WEXITSTATUS(reducer_status));
    }

    free_paths(input_files, n_files);

    int ret = (send_ok < 0 || collect_ok < 0 ||
               (WIFEXITED(mapper_status)  && WEXITSTATUS(mapper_status)  != 0) ||
               (WIFEXITED(reducer_status) && WEXITSTATUS(reducer_status) != 0))
              ? -1 : 0;

    if (ret == 0)
        LOG_INFO("mr_start completato con successo");
    else
        LOG_ERROR("mr_start terminato con errori");

    log_close();
    return ret;
}
