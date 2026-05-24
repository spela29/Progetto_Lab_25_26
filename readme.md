# libmr

Progetto per il corso di Laboratorio 2A, a.a. 2025-26.

Framework in C per l'analisi di file testuali secondo il modello
MapReduce su singolo calcolatore. L'idea è separare la logica
applicativa (le funzioni mapper e reducer che l'utente fornisce)
dall'infrastruttura concorrente (processi, pipe, thread).

## Come compilare

```bash
make          # compila la libreria e l'esempio
make test     # esegue i test
make clean    # pulizia