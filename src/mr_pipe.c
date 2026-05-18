#include "mr_internal.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>


ssize_t readn(int fd, void* buf, size_t n){
    size_t tot = 0;//numero di byte letti fino ad adesso 
    ssize_t r;
    while(tot<n){
        r = read(fd,(char*)buf + tot, n - tot);
        if(r == 0) return 0;//EOF
        if(r < 0) return -1;//errore nella lettura
        tot += (size_t)r;
    }
    return (ssize_t)tot;
}


ssize_t writen(int fd, const void* buf, size_t n){
    size_t tot = 0;//numero byte scritti fino ad adesso
    ssize_t w;
    while(tot<n){
        w = write(fd,(const char*)buf+tot,n-tot);
        if(w<0)return -1;//errore nella scrittura
        tot += (size_t)w;
    }
    return (ssize_t)tot;
}

//[Principale->Mapper]
int mr_send_line(int fd,
                 const char *file_name, size_t file_name_len,
                 unsigned long line_number,
                 const char *line, size_t line_len)
{

    mr_line_header_t hdr;
    hdr.file_name_len = (int)file_name_len;
    hdr.line_number = line_number;
    hdr.line_len = (int)line_len;

    if(writen(fd,&hdr,sizeof(hdr)) != (ssize_t)sizeof(hdr)) return -1;//errore nella scrittura
    if(file_name_len>0){
        if(writen(fd,file_name,file_name_len)!=(ssize_t)file_name_len) return -1;
    }
    if(line_len>0){
    if(writen(fd,line,line_len)!=(ssize_t)line_len) return -1;
    }
    return 0;
}


int mr_recv_line(int fd, mr_line_header_t *hdr,
                 char **file_name_out, char **line_out)
{
    ssize_t r = readn(fd,hdr,(size_t)sizeof(hdr));
    if(r == 0)return 1;//pipe chiusa EOF
    if(r<0) return -1;//errore nella readn

//valida gli attributi
    if(hdr->file_name_len<0 || hdr->file_name_len > MR_MAX_FNAME_LEN){
        errno = EINVAL;
        return -1;
        }
    if(hdr->line_len<0 || hdr->line_len>MR_MAX_LINE_LEN){
        errno = EINVAL;
        return -1;
        }

//legge il nome del file 
    char* file_name = malloc((hdr->file_name_len)+1);
    if(file_name == NULL) return -1;
    if(hdr->file_name_len>0){
        if(readn(fd,file_name,(size_t)hdr->file_name_len) <=0){
            free(file_name);
            return -1;
        }
    }
    file_name[hdr->file_name_len] = '\0';

//legge la riga come sequenza di byte
    char* line = malloc((hdr->line_len)+1);
    if(line == NULL){
        free(file_name);
        file_name = NULL;
        return -1;
        }
    if(hdr->line_len>0){
        if(readn(fd,line,(size_t)hdr->line_len) <=0){
            free(file_name);
            file_name = NULL;
            free(line);
            return -1;
        }
    }
    line[hdr->line_len] = '\0';

//scrive i risultati ottenuti nelle variabili punatate dai puntatori passati in input
    *file_name_out = file_name;
    *line_out = line;
    return 0;
}


//[Mapper->Reducer]
int mr_send_pair(int fd,
                 const char *token, size_t token_len,
                 const void *value, size_t value_len)
{
    mr_pair_header_t hdr;
    hdr.token_len = (int) token_len;
    hdr.value_len = (int) value_len;

    if(writen(fd,&hdr,sizeof(hdr))!=(ssize_t)sizeof(hdr)) return -1;
    if(token_len>0){
        if(writen(fd,token,token_len)!=(ssize_t)token_len)return -1;
    }
    if(value_len>0){
        if(writen(fd,value,value_len)!=(ssize_t)value_len) return -1;
    }

    return 0;

}

int mr_recv_pair(int fd,
                 char **token_out, size_t *token_len_out,
                 void **value_out, size_t *value_len_out)
{
    mr_pair_header_t hdr;
    ssize_t r = readn(fd,&hdr,sizeof(hdr));
    if(r == 0) return 1;//pipe chiusa EOF
    if(r<0)return -1;//errore nella read
//valida gli attributi
    if(hdr.token_len<=0 || hdr.token_len>MR_MAX_TOKEN_LEN){
        errno = EINVAL;
        return -1;
    } 

    if(hdr.value_len<0 || hdr.value_len>MR_MAX_VALUE_LEN){
        errno = EINVAL;
        return -1;
    }


//alloca e legge il token
    char* token = malloc((size_t)hdr.token_len + 1);
    if(token == NULL) return -1;
    if(readn(fd,&token,hdr.token_len) <=0 ) return -1;
    token[hdr.token_len] = '\0';

//legge il value come seq di byte opaca value puo essere NULL e value_len puo essere 0
    void* value = NULL;
    if(hdr.value_len>0){
        value = malloc((size_t)hdr.value_len);
        if(value == NULL){
            free(token);
            return -1;
        }
        if(readn(fd,value,hdr.value_len)<=0){
            free(token);
            free(value);
            return -1;
        }

    }
//scrive i risultati ottenuti nelle variabili punatate dai puntatori passati in input
    *token_out = token;
    *token_len_out = (size_t) hdr.token_len;
    *value_out = value;
    *value_len_out = (size_t)hdr.value_len;
    return 0;
}

//[Reducer->Principale]
int mr_send_result(int fd,
                   const char *token, size_t token_len,
                   const void *result, size_t result_len)
{
    mr_result_header_t hdr;
    hdr.token_len = (int) token_len;
    hdr.result_len = (int) result_len;

    if(writen(fd,&hdr,sizeof(hdr))!=(ssize_t)sizeof(hdr)) return -1;

    if(hdr.token_len>0)
        if(writen(fd,token,token_len)!=(ssize_t)token_len)return -1;

    if(hdr.result_len>0)
        if(writen(fd,result,result_len)!=(ssize_t)result_len)return -1;

    return 0;
}

int mr_recv_result(int fd,
                   char **token_out,  size_t *token_len_out,
                   void **result_out, size_t *result_len_out)
{
    mr_result_header_t hdr;
    ssize_t r = read(fd,&hdr,sizeof(hdr));
    if(r == 0)return 1;//pipe chiusa EOF
    if(r<0)return -1;//errore nella lettura

//valida gli attributi
    if(hdr.token_len<=0 || hdr.token_len>MR_MAX_TOKEN_LEN){
        errno = EINVAL;
        return -1;
    }
    if(hdr.result_len<0 || hdr.result_len>MR_MAX_VALUE_LEN){
        errno = EINVAL;
        return -1;
    }
//alloca e legge il token
    char* token = malloc((size_t)(hdr.token_len + 1));
    if(token == NULL) return -1;
    if(readn(fd,token,(size_t)hdr.token_len)<=0){
        free(token);
        return -1;
    }
    token[hdr.token_len] = '\0';

//legge il result come seq di byte opaca result puo essere NULL e result_len puo essere 0
    void* result = NULL;
    if(hdr.result_len>0){
        result = malloc((size_t)hdr.result_len);
        if(result == NULL){
            free(token);
            return -1;
        }
        if(readn(fd,result,(size_t)hdr.result_len)<=0){
            free(token);
            free(result);
            return -1;
        }

    }

//scrive i risultati ottenuti nelle variabili punatate dai puntatori passati in input
    *token_out = token;
    *token_len_out = (size_t)hdr.token_len;
    *result_out = result;
    *result_len_out = (size_t)hdr.result_len;
    return 0;

}


