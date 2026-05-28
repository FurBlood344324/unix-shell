#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static int last_exit_code = 0;

static char *expand_cd_target(const char *arg)
{
    const char *home;
    size_t home_len;
    size_t suffix_len;
    char *expanded;

    if (arg == NULL || strcmp(arg, "~") == 0) {
        home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            log_msg("ERROR", "cd: HOME not set");
            return NULL;
        }
        return strdup(home);
    }

    if (arg[0] != '~' || arg[1] != '/') {
        return strdup(arg);
    }

    home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "cd: HOME not set\n");
        log_msg("ERROR", "cd: HOME not set");
        return NULL;
    }

    home_len = strlen(home);
    suffix_len = strlen(arg + 1);
    expanded = malloc(home_len + suffix_len + 1);
    if (expanded == NULL) {
        perror("cd: malloc");
        log_msg("ERROR", "cd: malloc failed: %s", strerror(errno));
        return NULL;
    }

    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, arg + 1, suffix_len + 1);
    return expanded;
}

static int builtin_cd(char **argv)
{
    char *resolved_target = NULL;
    const char *target = NULL;

    if (argv[1] == NULL) {
        resolved_target = expand_cd_target(NULL);
        if (resolved_target == NULL) {
            return -1;
        }
        target = resolved_target;
    } else if (strcmp(argv[1], "-") == 0) {
        target = getenv("OLDPWD");
        if (target == NULL) {
            fprintf(stderr, "cd: OLDPWD not set\n");
            log_msg("ERROR", "cd: OLDPWD not set");
            return -1;
        }
        printf("%s\n", target);
    } else {
        resolved_target = expand_cd_target(argv[1]);
        if (resolved_target == NULL) {
            return -1;
        }
        target = resolved_target;
    }

    char old_cwd[4096];
    if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
        perror("cd: getcwd");
        log_msg("ERROR", "cd: getcwd failed: %s", strerror(errno));
        free(resolved_target);
        return -1;
    }

    if (chdir(target) < 0) {
        perror("cd");
        log_msg("ERROR", "cd '%s' failed: %s", target, strerror(errno));
        free(resolved_target);
        return -1;
    }

    log_msg("INFO", "cd '%s'", target);

    if (setenv("OLDPWD", old_cwd, 1) < 0) {
        perror("cd: setenv OLDPWD");
        log_msg("ERROR", "cd: setenv OLDPWD failed: %s", strerror(errno));
        free(resolved_target);
        return -1;
    }

    free(resolved_target);
    return 0;
}

static int builtin_exit(char **argv)
{
    int code = last_exit_code;

    if (argv[1] != NULL) {
        char *endptr;
        long val = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || endptr == argv[1]) {
            fprintf(stderr, "exit: %s: numeric argument required\n", argv[1]);
            log_msg("ERROR", "exit: %s: numeric argument required", argv[1]);
            return -1;
        }
        code = (int)val;
    }

    log_msg("INFO", "shell exit (code=%d)", code);
    log_close();
    exit(code);
}

static void run_pipe_builtin(char **argv)
{
    if (strcmp(argv[0], "cd") == 0) {
        builtin_cd(argv);
        _exit(0);
    }
    if (strcmp(argv[0], "exit") == 0) {
        int code = last_exit_code;
        if (argv[1] != NULL) {
            char *endptr;
            long val = strtol(argv[1], &endptr, 10);
            if (*endptr == '\0' && endptr != argv[1]) {
                code = (int)val;
            }
        }
        _exit(code);
    }
}

static void pipe_child(int close_fd, int dup_fd, int target_fd, char **argv)
{
    close(close_fd);
    if (dup2(dup_fd, target_fd) < 0) {
        perror("shell: dup2");
        _exit(1);
    }
    close(dup_fd);

    run_pipe_builtin(argv);
    execvp(argv[0], argv);
    fprintf(stderr, "shell: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

static int log_process(pid_t pid, char **argv, const char *label, int set_exit)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("shell: waitpid");
        log_msg("ERROR", "waitpid(%d) basarisiz: %s", (int)pid, strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (set_exit) last_exit_code = code;
        log_msg("INFO", "%scmd='%s' pid=%d exit=%d",
                label, argv[0], (int)pid, code);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (set_exit) last_exit_code = 128 + sig;
        log_msg("WARN", "%scmd='%s' pid=%d signal=%d",
                label, argv[0], (int)pid, sig);
    }
    return 0;
}

int run_pipe(char **left_argv, char **right_argv)
{
    int fd[2];
    if (pipe(fd) < 0) {
        perror("shell: pipe");
        log_msg("ERROR", "pipe basarisiz: %s", strerror(errno));
        return -1;
    }

    pid_t left_pid = fork();
    if (left_pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        return -1;
    }

    if (left_pid == 0) {
        pipe_child(fd[0], fd[1], STDOUT_FILENO, left_argv);
    }

    pid_t right_pid = fork();
    if (right_pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        waitpid(left_pid, NULL, 0);
        return -1;
    }

    if (right_pid == 0) {
        pipe_child(fd[1], fd[0], STDIN_FILENO, right_argv);
    }

    close(fd[0]);
    close(fd[1]);

    log_process(left_pid, left_argv, "pipe-left: ", 0);
    log_process(right_pid, right_argv, "pipe-right: ", 1);

    return 0;
}

int run_builtin(char **argv)
{
    if (strcmp(argv[0], "cd") == 0) {
        builtin_cd(argv);
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0) {
        builtin_exit(argv);
        return 1;
    }
    return 0;
}

int run_external(char **argv)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "shell: %s: %s\n", argv[0], strerror(errno));
        log_msg("ERROR", "execvp '%s' basarisiz: %s", argv[0], strerror(errno));
        _exit(127);
    }

    if (log_process(pid, argv, "", 1) < 0)
        return -1;
    return 0;
}
