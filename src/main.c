#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"
#include "parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_LINE 1024
#define LOG_FILE "shell.log"
#define PROMPT   "msh> "

int main(void)
{
    int prompt_shown = 0;

    if (log_init(LOG_FILE) < 0) {
        perror("shell: log dosyasi acilamadi");
        return EXIT_FAILURE;
    }

    if (install_sigchld_handler() < 0) {
        perror("shell: SIGCHLD handler kurulamadi");
        log_close();
        return EXIT_FAILURE;
    }

    log_msg("INFO", "shell baslatildi");

    char  line[MAX_LINE];
    char *argv[MAX_ARGS];
    char *left_argv[MAX_ARGS];
    char *right_argv[MAX_ARGS];

    for (;;) {
        flush_background_events();

        if (isatty(STDIN_FILENO) && !prompt_shown) {
            fputs(PROMPT, stdout);
            fflush(stdout);
            prompt_shown = 1;
        }

        errno = 0;
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                flush_background_events();
                continue;
            }
            if (isatty(STDIN_FILENO)) fputc('\n', stdout);
            break;
        }
        prompt_shown = 0;

        int background = 0;
        int pres = parse_pipe(line, left_argv, right_argv, &background);
        if (pres == -1) {
            fprintf(stderr, "shell: invalid pipe command\n");
            log_msg("ERROR", "invalid pipe, missing left or right command");
            continue;
        }
        if (pres == 1) {
            if (background) {
                fprintf(stderr, "shell: background pipe is not supported yet\n");
                log_msg("WARN", "background pipe reddedildi");
                continue;
            }
            run_pipe(left_argv, right_argv);
            continue;
        }

        if (parse_line(line, argv, &background) == 0) {
            continue;
        }

        if (run_builtin(argv, background)) {
            continue;
        }

        run_external(argv, background);
    }

    flush_background_events();
    log_msg("INFO", "shell kapaniyor");
    log_close();
    return EXIT_SUCCESS;
}
