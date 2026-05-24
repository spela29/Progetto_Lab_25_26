/*
 * mr_log.c
 *
 * Modulo di logging del framework libmr.
 *
 * Ogni processo (main, mapper, reducer) chiama log_open() all'avvio
 * e log_close() alla chiusura. L'accesso concorrente al file è
 * sincronizzato tramite un semaforo POSIX named, il cui nome è
 * derivato dal percorso del file di log.
 *
 * Formato di ogni riga:
 *   [YYYY-MM-DD HH:MM:SS.mmm] [PID] [TID] [LEVEL] messaggio
 *
 * TID è il valore restituito da thrd_current() castato a unsigned long.
 * Nel processo principale, dove i thread C11 non sono necessari, TID
 * vale 0 per convenzione.
 */

#include "mr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>
#include <threads.h>
#include <stdint.h>

#define LOG_SEM_PREFIX "/libmr_log_"
#define LOG_SEM_NAMELEN 64

static FILE *g_log_fp   = NULL;
static sem_t *g_log_sem = SEM_FAILED;
static char   g_sem_name[LOG_SEM_NAMELEN];

/* Crea un nome di semaforo valido dal percorso del file di log */
static void make_sem_name(const char *path, char *out, size_t out_len)
{
    /* Sostituiamo '/' con '_' per costruire il nome POSIX */
    const char *p = path;
    size_t i = 0;
    size_t prefix_len = strlen(LOG_SEM_PREFIX);
    if (prefix_len >= out_len) { out[0] = '\0'; return; }
    memcpy(out, LOG_SEM_PREFIX, prefix_len);
    i = prefix_len;

    for (; *p && i < out_len - 1; p++) {
        char c = *p;
        if (c == '/') c = '_';
        out[i++] = c;
    }
    out[i] = '\0';
}

int log_open(const char *path)
{
    const char *actual = path ? path : "mr.log";

    /* Apriamo in append (crea se non esiste) */
    g_log_fp = fopen(actual, "a");
    if (!g_log_fp) return -1;

    /* Semaforo POSIX named condiviso tra tutti i processi */
    make_sem_name(actual, g_sem_name, sizeof(g_sem_name));
    g_log_sem = sem_open(g_sem_name, O_CREAT, 0644, 1);
    if (g_log_sem == SEM_FAILED) {
        fclose(g_log_fp);
        g_log_fp = NULL;
        return -1;
    }
    return 0;
}

void log_close(void)
{
    if (g_log_sem != SEM_FAILED) {
        sem_close(g_log_sem);
        g_log_sem = SEM_FAILED;
    }
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

void log_write(const char *level, const char *fmt, ...)
{
    if (!g_log_fp) return;

    /* Timestamp con millisecondi */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    pid_t pid = getpid();
    /* thrd_current() restituisce un identificatore del thread corrente */
    unsigned long tid = (unsigned long)(uintptr_t)thrd_current();

    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Sezione critica: accesso esclusivo al file */
    sem_wait(g_log_sem);
    fprintf(g_log_fp, "[%s.%03ld] [%d] [%lu] [%s] %s\n",
            ts, (long)(tv.tv_usec / 1000), (int)pid, tid, level, msg);
    fflush(g_log_fp);
    sem_post(g_log_sem);
}
