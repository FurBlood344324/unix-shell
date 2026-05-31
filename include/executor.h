#ifndef MSH_EXECUTOR_H
#define MSH_EXECUTOR_H

int install_sigchld_handler(void);
void flush_background_events(void);
int run_builtin(char **argv, int background);
void run_external(char **argv, int background);
void run_pipe(char **left_argv, char **right_argv);
int get_last_exit_code(void);
int get_should_exit(void);
int get_exit_code(void);

#endif
