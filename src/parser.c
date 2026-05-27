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
    char *left  = line;
    char *right = pipe_pos + 1;

    int left_argc = 0;
    char *tok = strtok(left, " \t\r\n");
    while (tok != NULL && left_argc < MAX_ARGS - 1) {
        left_argv[left_argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    left_argv[left_argc] = NULL;

    int right_argc = 0;
    tok = strtok(right, " \t\r\n");
    while (tok != NULL && right_argc < MAX_ARGS - 1) {
        right_argv[right_argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    right_argv[right_argc] = NULL;

    if (left_argc == 0 || right_argc == 0) {
        return -1;
    }

    return 1;
}
