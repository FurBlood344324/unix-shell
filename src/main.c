#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"
#include "parser.h"
#include "history.h"
#include "linenoise.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define MAX_LINE 1024
#define LOG_FILE "shell.log"
#define PROMPT   "msh> "

static long command_count = 0;
static double total_time_ms = 0.0;
static double min_time_ms = -1.0;
static double max_time_ms = 0.0;

void print_summary() {
    if (command_count > 0) {
        double avg_time_ms = total_time_ms / command_count;
        printf("\n--- Performans Ozeti ---\n");
        printf("Calistirilan komut sayisi: %ld\n", command_count);
        printf("Ortalama calisma suresi: %.2f ms\n", avg_time_ms);
        printf("Minimum calisma suresi: %.2f ms\n", min_time_ms);
        printf("Maksimum calisma suresi: %.2f ms\n", max_time_ms);
        printf("------------------------\n");
    }
}

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

    history_init();
    linenoiseHistorySetMaxLen(10);
    linenoiseHistoryLoad("history.txt"); 

    log_msg("INFO", "shell baslatildi");

    char *line;
    char *argv[MAX_ARGS];
    char *left_argv[MAX_ARGS];
    char *right_argv[MAX_ARGS];

    struct timespec start_time, end_time;

    while ((line = linenoise(PROMPT)) != NULL) {
        if (line[0] == '\0') {
            free(line);
            continue;
        }

        history_add(line);
        linenoiseHistoryAdd(line);
        linenoiseHistorySave("history.txt");

        int background = 0;
        int n_args = parse_line(line, argv, &background);

        if (n_args < 0) {
            fputs("shell: komut parse edilemedi\n", stderr);
            free(line);
            continue;
        }

        if (n_args == 0) {
            free(line);
            continue;
        }
        
        if (strcmp(argv[0], "exit") == 0) {
            free(line);
            break;
        }

        if (strcmp(argv[0], "history") == 0) {
            history_print();
            free(line);
            continue;
        }

        if (strcmp(argv[0], "cd") == 0) {
            if (n_args < 2) {
                fputs("cd: arguman eksik\n", stderr);
            } else if (chdir(argv[1]) < 0) {
                perror("cd");
            }
            free(line);
            continue;
        }

        int pipe_pos = -1;
        for (int i = 0; i < n_args; ++i) {
            if (strcmp(argv[i], "|") == 0) {
                pipe_pos = i;
                break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &start_time);

        if (pipe_pos < 0) {
            if (run_builtin(argv, background) < 0) {
                run_external(argv, background);
            }
        } else {
            char *left_argv[MAX_ARGS] = {0};
            char *right_argv[MAX_ARGS] = {0};
            
            for (int i = 0; i < pipe_pos; ++i) {
                left_argv[i] = argv[i];
            }
            for (int i = 0; i < n_args - pipe_pos - 1; ++i) {
                right_argv[i] = argv[pipe_pos + 1 + i];
            }
            run_pipe(left_argv, right_argv);
        }
        free(line);

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                            (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

        char log_buffer[128];
        snprintf(log_buffer, sizeof(log_buffer), "Komut calisma suresi: %.2f ms", elapsed_ms);
        log_msg("PERF", log_buffer);

        command_count++;
        total_time_ms += elapsed_ms;
        if (min_time_ms < 0 || elapsed_ms < min_time_ms) {
            min_time_ms = elapsed_ms;
        }
        if (elapsed_ms > max_time_ms) {
            max_time_ms = elapsed_ms;
        }
    }

    print_summary();
    history_destroy();
    log_msg("INFO", "shell sonlandirildi");
    log_close();

    return EXIT_SUCCESS;
}
