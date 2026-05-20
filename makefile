CC      = gcc #compilatore
CFLAGS  = -std=c11 -Wall -Wextra  -g -I include -I src #flags usati
AR      = ar # Programma per creare librerie statiche
ARFLAGS = rcs # r = inserisci/aggiorna, c = crea se non esiste, s = aggiungi indice

# File sorgenti della libreria
LIB_SRCS = src/mr.c         \
           src/mr_queue.c   \
           src/mr_pipe.c    \
           src/mr_mapper.c  \
           src/mr_reducer.c \
           src/mr_log.c     


LIB_OBJS = $(LIB_SRCS:.c=.o) # Trasforma ogni .c in .o automaticamente

LIB = libmr.a # Nome della libreria statica da produrre

#questo fa si che i comandi sotto i target all test e clean vengono 
#eseguiti comunque anche se sono gia presenti file con quei nomi nella
#directory corrente
.PHONY: all test clean

#questo è il comando che viene eseguito quando scriviamo make e basta
#perche è quello di default siccome è il primo trovato e compila 
#la libreria con l'esempio wordcount 
all: $(LIB) examples/wordcount


# Costruisce la libreria statica impacchettando tutti i .o perche 
#dice che $(LIB) nel nostro caso libmr.a dipende da $(LIB_OBJS) che sono
#tutti i file oggetto risultati dalla compilazione si tutti i file sorgente 
#in LIB_SRCS
# $@ = libmr.a (target corrente)
# $^ = tutti i .o
$(LIB): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^


# Regola generica: compila un .c in un .o usando il compilatore 
#settato in $(CC) e i falg in $(CFLAGS)
# $< = il file .c
# $@ = il file .o da produrre
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


# Compila l'esempio wordcount linkandolo alla libreria $(LIB)
# -L. = cerca le librerie nella cartella corrente
# -lmr = linka libmr.a
examples/wordcount: examples/wordcount.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lmr -o $@




# Compila ed esegue i test
tests/test_main: tests/test_main.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lmr -o $@

test: tests/test_main
	./tests/test_main


# Rimuove tutti i file generati dalla compilazione
clean:
	rm -f $(LIB_OBJS) $(LIB) examples/wordcount tests/test_main 