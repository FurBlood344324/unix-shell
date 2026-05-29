#include "parser.h"

#include <stdlib.h>
#include <string.h>

int parse_line(char *line, char **argv, int *background)
{
    int argc = 0;

    if (background != NULL) {
        *background = 0;
    }

    char *tok = strtok(line, " \t\r\n");
    while (tok != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        argc--;
        if (background != NULL) {
            *background = 1;
        }
    }

    argv[argc] = NULL;
    return argc;
}

int parse_pipe(char *line, char **left_argv, char **right_argv, int *background)
{
    int left_background = 0;
    int right_background = 0;
    char *pipe_pos = strchr(line, '|');

    if (background != NULL) {
        *background = 0;
    }

    if (pipe_pos == NULL) {
        return 0;
    }

    *pipe_pos = '\0';

    int left_argc = parse_line(line, left_argv, &left_background);
    int right_argc = parse_line(pipe_pos + 1, right_argv, &right_background);

    if (left_argc == 0 || right_argc == 0) {
        return -1;
    }

    if (background != NULL) {
        *background = left_background || right_background;
    }

    return 1;
}
