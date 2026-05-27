#ifndef MSH_EXECUTOR_H
#define MSH_EXECUTOR_H

int run_builtin(char **argv);
int run_external(char **argv);
int run_pipe(char **left_argv, char **right_argv);

#endif
