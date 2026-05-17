#define _POSIX_C_SOURCE 200809L

#include "log.h"
#include "parser.h"
#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 1024
#define LOG_FILE "shell.log"
#define PROMPT   "msh> "

int main(void)
{
    if (log_init(LOG_FILE) < 0) {
        perror("shell: log dosyasi acilamadi");
        return EXIT_FAILURE;
    }

    log_msg("INFO", "shell baslatildi");

    char  line[MAX_LINE];
    char *argv[MAX_ARGS];

    for (;;) {
        if (isatty(STDIN_FILENO)) {
            fputs(PROMPT, stdout);
            fflush(stdout);
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (isatty(STDIN_FILENO)) fputc('\n', stdout);
            break;
        }

        if (parse_line(line, argv) == 0) {
            continue;
        }

        if (run_builtin(argv)) {
            continue;
        }

        run_external(argv);
    }

    log_msg("INFO", "shell kapaniyor");
    log_close();
    return EXIT_SUCCESS;
}
