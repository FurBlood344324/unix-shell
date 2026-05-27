#include "parser.h"

#include <stdlib.h>
#include <string.h>

int parse_line(char *line, char **argv)
{
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;
    return argc;
}

int parse_pipe(char *line, char **left_argv, char **right_argv)
{
    char *pipe_pos = strchr(line, '|');
    if (pipe_pos == NULL) {
        return 0;
    }

    *pipe_pos = '\0';

    int left_argc  = parse_line(line, left_argv);
    int right_argc = parse_line(pipe_pos + 1, right_argv);

    if (left_argc == 0 || right_argc == 0) {
        return -1;
    }

    return 1;
}
