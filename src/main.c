#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"
#include "parser.h"
#include "history.h"
#include "performance.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_LINE 1024
#define LOG_FILE "shell.log"
#define PROMPT   "msh> "

int main(void)
{
    if (log_init(LOG_FILE) < 0) {
        perror("shell: log dosyasi acilamadi");
        return EXIT_FAILURE;
    }

    if (install_sigchld_handler() < 0) {
        perror("shell: SIGCHLD handler kurulamadi");
        log_close();
        return EXIT_FAILURE;
    }

    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            performance_init(cwd);
        }
    }

    history_load("history.txt");

    log_msg("INFO", "shell baslatildi");

    char *line;
    char *argv[MAX_ARGS];
    char *left_argv[MAX_ARGS];
    char *right_argv[MAX_ARGS];

    struct timespec start_time, end_time;

    while ((line = readline(PROMPT)) != NULL) {
        flush_background_events();

        if (line[0] == '\0') {
            free(line);
            continue;
        }

        history_add(line);
        history_save("history.txt");

        int background = 0;
        int pipe_result = parse_pipe(line, left_argv, right_argv, &background);

        if (pipe_result < 0) {
            fputs("shell: komut parse edilemedi\n", stderr);
            free(line);
            continue;
        }

        int timed = 1;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        if (pipe_result == 1) {
            if (background) {
                fputs("msh: background pipe is not supported\n", stderr);
                timed = 0;
            } else {
                run_pipe(left_argv, right_argv);
            }
        } else {
            int n_args = parse_line(line, argv, &background);
            if (n_args == 0) {
                timed = 0;
            } else if (run_builtin(argv, background) < 0) {
                run_external(argv, background);
            }
        }

        if (timed) {
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            record_command_time(start_time, end_time);
        }

        if (get_should_exit()) {
            free(line);
            break;
        }

        free(line);
    }

    print_performance_summary();
    history_destroy();
    log_msg("INFO", "shell sonlandirildi");
    log_close();

    return get_should_exit() ? get_exit_code() : EXIT_SUCCESS;
}
