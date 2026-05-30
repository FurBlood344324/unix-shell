#define _GNU_SOURCE
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "linenoise.h"
#include <strings.h>

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static const char *unsupported_term[] = {"dumb","cons25","emacs",NULL};

static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit. */
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void linenoiseAtExit(void);
int linenoiseHistoryAdd(const char *line);
static void refreshLine(int fd, const char *prompt, char *buf, size_t len, size_t pos, size_t cols);

/* ======================= Low level terminal handling ====================== */

static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (strcasecmp(term,unsupported_term[j]) == 0) return 1;
    return 0;
}

static int enableRawMode(int fd) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - echoing off, canonical mode off,
     * extended input processing off, signal chars off. */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    if (tcsetattr(fd,TCSADRAIN,&raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void disableRawMode(int fd) {
    if (rawmode && tcsetattr(fd,TCSADRAIN,&orig_termios) != -1)
        rawmode = 0;
}

static int getColumns(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return 80;
    return ws.ws_col;
}

void linenoiseClearScreen(void) {
    if (write(STDOUT_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning */
    }
}

static void linenoiseBeep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

static int completeLine(const char *prompt, char *buf, size_t buflen, size_t *len, size_t *pos, size_t cols, linenoiseCompletions *lc) {
    char c = 0;

    completionCallback(buf,lc);
    if (lc->len == 0) {
        linenoiseBeep();
    } else {
        size_t stop = 0, i = 0;

        while(!stop) {
            if (i < lc->len) {
                char *newbuf = lc->cvec[i];
                size_t newlen = strlen(newbuf);
                *len = *pos = newlen;
                strncpy(buf, newbuf, buflen-1);
                buf[buflen-1] = '\0';
                refreshLine(STDOUT_FILENO,prompt,buf,*len,*pos,cols);
            } else {
                refreshLine(STDOUT_FILENO,prompt,buf,*len,*pos,cols);
            }

            if (read(STDIN_FILENO, &c, 1) <= 0) {
                freeCompletions(lc);
                return -1;
            }

            switch(c) {
                case TAB: /* tab */
                    i = (i+1) % (lc->len+1);
                    if (i == lc->len) linenoiseBeep();
                    break;
                case ESC: /* escape */
                    if (i < lc->len) {
                        refreshLine(STDOUT_FILENO,prompt,buf,*len,*pos,cols);
                    }
                    stop = 1;
                    break;
                default:
                    stop = 1;
                    break;
            }
        }
    }

    freeCompletions(lc);
    return c; /* Return last read character */
}

void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len+1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    cvec = realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

struct abuf {
    char *b;
    int len;
};

static void abInit(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
}

static void refreshLine(int fd, const char *prompt, char *buf, size_t len, size_t pos, size_t cols)
{
    struct abuf ab;
    char seq[64];
    size_t plen = strlen(prompt);
    
    abInit(&ab);
    while((plen+pos) >= cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > cols) {
        len--;
    }

    snprintf(seq,64,"\r");
    abAppend(&ab,seq,strlen(seq));
    abAppend(&ab,prompt,strlen(prompt));
    abAppend(&ab,buf,len);
    snprintf(seq,64,"\x1b[K");
    abAppend(&ab,seq,strlen(seq));
    snprintf(seq,64,"\r\x1b[%dC", (int)(pos+plen));
    abAppend(&ab,seq,strlen(seq));
    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

static int linenoiseEditInsert(char *buf, size_t buflen, size_t *len, size_t *pos, char c) {
    if (*len < buflen) {
        if (*len == *pos) {
            buf[*pos] = c;
            (*pos)++;
            (*len)++;
            buf[*len] = '\0';
        } else {
            memmove(buf+*pos+1,buf+*pos,*len-*pos);
            buf[*pos] = c;
            (*len)++;
            (*pos)++;
            buf[*len] = '\0';
        }
    }
    return 0;
}

static void linenoiseEditMoveLeft(size_t *pos) {
    if (*pos > 0) (*pos)--;
}

static void linenoiseEditMoveRight(size_t len, size_t *pos) {
    if (*pos != len) (*pos)++;
}

static void linenoiseEditMoveHome(size_t *pos) {
    *pos = 0;
}

static void linenoiseEditMoveEnd(size_t len, size_t *pos) {
    *pos = len;
}

static void linenoiseEditHistoryNext(size_t buflen, char *buf, size_t *len, size_t *pos, int dir, int *history_index) {
    if (history_len > 1) {
        free(history[history_len - 1 - *history_index]);
        history[history_len - 1 - *history_index] = strdup(buf);
        *history_index += (dir == 1) ? 1 : -1;
        if (*history_index < 0) {
            *history_index = 0;
            return;
        } else if (*history_index >= history_len) {
            *history_index = history_len-1;
            return;
        }
        strncpy(buf,history[history_len - 1 - *history_index],buflen);
        buf[buflen-1] = '\0';
        *len = *pos = strlen(buf);
    }
}

static void linenoiseEditDelete(char *buf, size_t *len, size_t *pos) {
    if (*len > 0 && *pos < *len) {
        memmove(buf+*pos,buf+*pos+1,*len-*pos-1);
        (*len)--;
        buf[*len] = '\0';
    }
}

static void linenoiseEditBackspace(char *buf, size_t *len, size_t *pos) {
    if (*pos > 0 && *len > 0) {
        memmove(buf+*pos-1,buf+*pos,*len-*pos);
        (*pos)--;
        (*len)--;
        buf[*len] = '\0';
    }
}

static void linenoiseEditDeletePrevWord(char *buf, size_t *len, size_t *pos) {
    size_t old_pos = *pos;
    size_t diff;

    while (*pos > 0 && buf[*pos-1] == ' ')
        (*pos)--;
    while (*pos > 0 && buf[*pos-1] != ' ')
        (*pos)--;
    diff = old_pos - *pos;
    memmove(buf+*pos,buf+old_pos,*len-old_pos+1);
    *len -= diff;
}

static int linenoiseEdit(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt)
{
    size_t plen = strlen(prompt);
    size_t pos = 0;
    size_t len = 0;
    size_t cols = getColumns();
    int history_index = 0;

    buf[0] = '\0';
    buflen--; 

    linenoiseHistoryAdd("");

    if (write(stdout_fd,prompt,plen) == -1) return -1;
    while(1) {
        char c;
        int nread;
        char seq[3];

        nread = read(stdin_fd,&c,1);
        if (nread <= 0) return len;

        if (c == TAB && completionCallback != NULL) {
            linenoiseCompletions lc = { 0, NULL };
            c = completeLine(prompt,buf,buflen,&len,&pos,cols,&lc);
            if (c == -1) return len;
            if (c != 0) continue;
        }

        switch(c) {
        case ENTER:
            history_len--;
            free(history[history_len]);
            return (int)len;
        case CTRL_C:
            errno = EAGAIN;
            return -1;
        case BACKSPACE:
        case CTRL_H:
            linenoiseEditBackspace(buf,&len,&pos);
            break;
        case CTRL_D:
            if (len > 0) {
                linenoiseEditDelete(buf,&len,&pos);
            } else {
                history_len--;
                free(history[history_len]);
                return -1;
            }
            break;
        case CTRL_T:
            if (pos > 0 && pos < len) {
                char aux = buf[pos-1];
                buf[pos-1] = buf[pos];
                buf[pos] = aux;
                if (pos != len-1) pos++;
            }
            break;
        case CTRL_B:
            linenoiseEditMoveLeft(&pos);
            break;
        case CTRL_F:
            linenoiseEditMoveRight(len,&pos);
            break;
        case CTRL_P:
            linenoiseEditHistoryNext(buflen,buf,&len,&pos,1,&history_index);
            break;
        case CTRL_N:
            linenoiseEditHistoryNext(buflen,buf,&len,&pos,0,&history_index);
            break;
        case ESC:
            if (read(stdin_fd,seq,1) == -1) break;
            if (read(stdin_fd,seq+1,1) == -1) break;

            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    if (read(stdin_fd,seq+2,1) == -1) break;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': /* Delete key. */
                            linenoiseEditDelete(buf,&len,&pos);
                            break;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': /* Up */
                        linenoiseEditHistoryNext(buflen,buf,&len,&pos,1,&history_index);
                        break;
                    case 'B': /* Down */
                        linenoiseEditHistoryNext(buflen,buf,&len,&pos,0,&history_index);
                        break;
                    case 'C': /* Right */
                        linenoiseEditMoveRight(len,&pos);
                        break;
                    case 'D': /* Left */
                        linenoiseEditMoveLeft(&pos);
                        break;
                    case 'H': /* Home */
                        linenoiseEditMoveHome(&pos);
                        break;
                    case 'F': /* End*/
                        linenoiseEditMoveEnd(len,&pos);
                        break;
                    }
                }
            } else if (seq[0] == 'O') {
                switch(seq[1]) {
                case 'H': /* Home */
                    linenoiseEditMoveHome(&pos);
                    break;
                case 'F': /* End*/
                    linenoiseEditMoveEnd(len,&pos);
                    break;
                }
            }
            break;
        default:
            if (linenoiseEditInsert(buf,buflen,&len,&pos,c)) return -1;
            break;
        case CTRL_U:
            buf[0] = '\0';
            pos = len = 0;
            break;
        case CTRL_K:
            buf[pos] = '\0';
            len = pos;
            break;
        case CTRL_A:
            linenoiseEditMoveHome(&pos);
            break;
        case CTRL_E:
            linenoiseEditMoveEnd(len,&pos);
            break;
        case CTRL_L:
            linenoiseClearScreen();
            break;
        case CTRL_W:
            linenoiseEditDeletePrevWord(buf,&len,&pos);
            break;
        }
        refreshLine(stdout_fd,prompt,buf,len,pos,cols);
    }
    return len;
}

static int linenoiseRaw(char *buf, size_t buflen, const char *prompt) {
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(STDIN_FILENO)) {
        if (fgets(buf, buflen, stdin) == NULL) return -1;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        if (enableRawMode(STDIN_FILENO) == -1) return -1;
        count = linenoiseEdit(STDIN_FILENO, STDOUT_FILENO, buf, buflen, prompt);
        disableRawMode(STDIN_FILENO);
        printf("\n");
    }
    return count;
}

char *linenoise(const char *prompt) {
    char buf[LINENOISE_MAX_LINE];
    int count;

    if (isUnsupportedTerm()) {
        printf("%s",prompt);
        fflush(stdout);
        if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        count = linenoiseRaw(buf,LINENOISE_MAX_LINE,prompt);
        if (count == -1) return NULL;
    }
    return strdup(buf);
}

void linenoiseFree(void *ptr) {
    free(ptr);
}

/* ================================ History ================================= */

int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;
    if (history == NULL) {
        history = malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }
    if (history_len > 0 && !strcmp(history[history_len-1], line)) return 0;
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

int linenoiseHistorySetMaxLen(int len) {
    char **new;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;
        new = malloc(sizeof(char*)*len);
        if (new == NULL) return 0;
        if (len < tocopy) {
            int j;
            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new,0,sizeof(char*)*len);
        memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

int linenoiseHistorySave(const char *filename) {
    FILE *fp = fopen(filename,"w");
    int j;

    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

int linenoiseHistoryLoad(const char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;
        
        p = strchr(buf,'\r');
        if (p) *p = '\0';
        p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}

void linenoiseAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

void linenoisePrintKeyCodes(void) {
    char c;
    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see bytes sequences.\n"
            "Press Ctrl-C or Ctrl-D to quit.\n");
    if (enableRawMode(STDIN_FILENO) == -1) return;
    while(1) {
        if (read(STDIN_FILENO,&c,1) <= 0) continue;
        switch(c) {
        case CTRL_D:
        case CTRL_C:
            disableRawMode(STDIN_FILENO);
            return;
        }
        printf("%d\n", (int)c);
    }
}