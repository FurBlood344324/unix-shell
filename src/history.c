#define _GNU_SOURCE
#include "history.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define HISTORY_MAX 10
#define LINE_MAX    4096

static struct termios orig_termios;
static int raw_mode;
static int atexit_registered;

static char *history[HISTORY_MAX];
static int hist_count;
static int hist_start;

static void disable_raw_mode(void)
{
    if (raw_mode) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
        raw_mode = 0;
    }
}

static int enable_raw_mode(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return -1;

    if (!atexit_registered) {
        atexit(disable_raw_mode);
        atexit_registered = 1;
    }

    raw = orig_termios;
    raw.c_lflag &= ~((tcflag_t)(ECHO | ICANON | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == -1) return -1;

    raw_mode = 1;
    return 0;
}

static int get_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return 80;
    return ws.ws_col;
}

static void refresh_line(const char *prompt, const char *buf, size_t len, size_t pos)
{
    char out[LINE_MAX + 128];
    size_t plen = strlen(prompt);
    size_t cols = (size_t)get_columns();

    while (plen + pos >= cols) { buf++; len--; pos--; }
    while (plen + len > cols)  { len--; }

    int n = snprintf(out, sizeof(out), "\r%s%.*s\x1b[K\r\x1b[%zuC",
                     prompt, (int)len, buf, plen + pos);
    if (n > 0) write(STDOUT_FILENO, out, (size_t)n);
}

void history_add(const char *line)
{
    if (!line || !*line) return;

    if (hist_count > 0) {
        int last = (hist_start + hist_count - 1) % HISTORY_MAX;
        if (strcmp(history[last], line) == 0) return;
    }

    if (hist_count == HISTORY_MAX) {
        free(history[hist_start]);
        hist_start = (hist_start + 1) % HISTORY_MAX;
        hist_count--;
    }

    int idx = (hist_start + hist_count) % HISTORY_MAX;
    history[idx] = strdup(line);
    hist_count++;
}

void history_print(void)
{
    for (int i = 0; i < hist_count; i++) {
        int idx = (hist_start + i) % HISTORY_MAX;
        printf("%d: %s\n", i + 1, history[idx]);
    }
}

void history_load(const char *filename)
{
    for (int i = 0; i < HISTORY_MAX; i++) history[i] = NULL;

    FILE *fp = fopen(filename, "r");
    if (!fp) return;

    char buf[LINE_MAX];
    while (fgets(buf, sizeof(buf), fp)) {
        char *p = strchr(buf, '\n'); if (p) *p = '\0';
        p = strchr(buf, '\r');       if (p) *p = '\0';
        history_add(buf);
    }
    fclose(fp);
}

void history_save(const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) return;
    for (int i = 0; i < hist_count; i++) {
        int idx = (hist_start + i) % HISTORY_MAX;
        fprintf(fp, "%s\n", history[idx]);
    }
    fclose(fp);
}

void history_destroy(void)
{
    for (int i = 0; i < HISTORY_MAX; i++) free(history[i]);
}

static int edit_line(char *buf, size_t limit, const char *prompt)
{
    size_t pos = 0, len = 0;
    int hidx = -1;
    char saved[LINE_MAX] = {0};
    char seq[3];

    buf[0] = '\0';
    limit--;

    if (write(STDOUT_FILENO, prompt, strlen(prompt)) == -1) return -1;

    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n == 0 || errno != EINTR) return -1;
            continue;
        }

        switch (c) {

        case 1:   pos = 0; break;                                   /* Ctrl-A */
        case 2:   if (pos > 0) pos--; break;                        /* Ctrl-B */
        case 3:   return -2;                                         /* Ctrl-C */
        case 4:                                                     /* Ctrl-D */
            if (len == 0) return -1;
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos - 1); len--; }
            break;
        case 5:   pos = len; break;                                  /* Ctrl-E */
        case 12:                                                     /* Ctrl-L */
            write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7);
            break;
        case 21:  buf[0] = '\0'; pos = len = 0; break;              /* Ctrl-U */
        case 23: {                                                   /* Ctrl-W */
            size_t old = pos;
            while (pos > 0 && buf[pos - 1] == ' ') pos--;
            while (pos > 0 && buf[pos - 1] != ' ') pos--;
            memmove(buf + pos, buf + old, len - old);
            len -= old - pos;
            break;
        }

        case '\r': case '\n':
            return (int)len;

        case 127: case 8:                                            /* Backspace */
            if (pos > 0) { memmove(buf + pos - 1, buf + pos, len - pos); pos--; len--; }
            break;

        case 27:                                                     /* ESC */
            if (read(STDIN_FILENO, seq, 2) < 2) break;
            if (seq[0] != '[') break;

            switch (seq[1]) {
            case 'A':                                                /* Up */
                if (hist_count == 0) break;
                if (hidx == -1) { strcpy(saved, buf); hidx = 0; }
                else if (hidx < hist_count - 1) { hidx++; }
                {
                    int i = (hist_start + hist_count - 1 - hidx) % HISTORY_MAX;
                    strncpy(buf, history[i], limit);
                    buf[limit] = '\0';
                    len = pos = strlen(buf);
                }
                break;
            case 'B':                                                /* Down */
                if (hidx == -1) break;
                if (hidx == 0) {
                    hidx = -1;
                    strncpy(buf, saved, limit);
                    buf[limit] = '\0';
                    len = pos = strlen(buf);
                } else {
                    hidx--;
                    int i = (hist_start + hist_count - 1 - hidx) % HISTORY_MAX;
                    strncpy(buf, history[i], limit);
                    buf[limit] = '\0';
                    len = pos = strlen(buf);
                }
                break;
            case 'C':  if (pos < len) pos++; break;                  /* Right */
            case 'D':  if (pos > 0) pos--; break;                    /* Left */
            case 'H':  pos = 0; break;                               /* Home */
            case 'F':  pos = len; break;                             /* End */
            case '3':                                                /* Delete */
                if (read(STDIN_FILENO, seq, 1) == 1 && seq[0] == '~')
                    if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos - 1); len--; }
                break;
            }
            break;

        default:
            if (c >= 32 && c <= 126 && len < limit) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos++] = c;
                len++;
            }
            break;
        }

        buf[len] = '\0';
        refresh_line(prompt, buf, len, pos);
    }
}

char *readline(const char *prompt)
{
    char buf[LINE_MAX];

    if (!isatty(STDIN_FILENO)) {
        if (!fgets(buf, sizeof(buf), stdin)) return NULL;
        size_t n = strlen(buf);
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        return strdup(buf);
    }

    if (enable_raw_mode() == -1) return NULL;

    int n = edit_line(buf, sizeof(buf), prompt);

    disable_raw_mode();
    printf("\n");

    if (n == -1) return NULL;
    if (n == -2) return strdup("");
    return strdup(buf);
}
