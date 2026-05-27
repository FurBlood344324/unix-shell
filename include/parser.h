#ifndef MSH_PARSER_H
#define MSH_PARSER_H

#define MAX_ARGS 64

int parse_line(char *line, char **argv);
int parse_pipe(char *line, char **left_argv, char **right_argv);

#endif
