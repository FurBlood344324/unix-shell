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

static int builtin_cd(char **argv)
{
    const char *target = NULL;

    if (argv[1] == NULL || strcmp(argv[1], "~") == 0) {
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            log_msg("ERROR", "cd: HOME not set");
            return -1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        target = getenv("OLDPWD");
        if (target == NULL) {
            fprintf(stderr, "cd: OLDPWD not set\n");
            log_msg("ERROR", "cd: OLDPWD not set");
            return -1;
        }
        printf("%s\n", target);
    } else {
        target = argv[1];
    }

    char old_cwd[4096];
    if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
        perror("cd: getcwd");
        log_msg("ERROR", "cd: getcwd failed: %s", strerror(errno));
        return -1;
    }

    if (chdir(target) < 0) { // Change Directory
        perror("cd");
        log_msg("ERROR", "cd '%s' failed: %s", target, strerror(errno));
        return -1;
    }

    log_msg("INFO", "cd '%s'", target);

    if (setenv("OLDPWD", old_cwd, 1) < 0) {
        perror("cd: setenv OLDPWD");
        log_msg("ERROR", "cd: setenv OLDPWD failed: %s", strerror(errno));
        return -1;
    }

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
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) < 0) {
            perror("shell: dup2");
            _exit(1);
        }
        close(fd[1]);

        if (strcmp(left_argv[0], "cd") == 0) {
            builtin_cd(left_argv);
            _exit(0);
        }
        if (strcmp(left_argv[0], "exit") == 0) {
            int code = last_exit_code;
            if (left_argv[1] != NULL) {
                char *endptr;
                long val = strtol(left_argv[1], &endptr, 10);
                if (*endptr == '\0' && endptr != left_argv[1]) {
                    code = (int)val;
                }
            }
            _exit(code);
        }

        execvp(left_argv[0], left_argv);
        fprintf(stderr, "shell: %s: %s\n", left_argv[0], strerror(errno));
        _exit(127);
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
        close(fd[1]);
        if (dup2(fd[0], STDIN_FILENO) < 0) {
            perror("shell: dup2");
            _exit(1);
        }
        close(fd[0]);

        if (strcmp(right_argv[0], "cd") == 0) {
            builtin_cd(right_argv);
            _exit(0);
        }
        if (strcmp(right_argv[0], "exit") == 0) {
            int code = last_exit_code;
            if (right_argv[1] != NULL) {
                char *endptr;
                long val = strtol(right_argv[1], &endptr, 10);
                if (*endptr == '\0' && endptr != right_argv[1]) {
                    code = (int)val;
                }
            }
            _exit(code);
        }

        execvp(right_argv[0], right_argv);
        fprintf(stderr, "shell: %s: %s\n", right_argv[0], strerror(errno));
        _exit(127);
    }

    close(fd[0]);
    close(fd[1]);

    int left_status = 0;
    waitpid(left_pid, &left_status, 0);
    if (WIFEXITED(left_status)) {
        log_msg("INFO", "pipe-left cmd='%s' pid=%d exit=%d",
                left_argv[0], (int)left_pid, WEXITSTATUS(left_status));
    } else if (WIFSIGNALED(left_status)) {
        log_msg("WARN", "pipe-left cmd='%s' pid=%d signal=%d",
                left_argv[0], (int)left_pid, WTERMSIG(left_status));
    }

    int right_status = 0;
    waitpid(right_pid, &right_status, 0);
    if (WIFEXITED(right_status)) {
        last_exit_code = WEXITSTATUS(right_status);
        log_msg("INFO", "pipe-right cmd='%s' pid=%d exit=%d",
                right_argv[0], (int)right_pid, last_exit_code);
    } else if (WIFSIGNALED(right_status)) {
        last_exit_code = 128 + WTERMSIG(right_status);
        log_msg("WARN", "pipe-right cmd='%s' pid=%d signal=%d",
                right_argv[0], (int)right_pid, WTERMSIG(right_status));
    }

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

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("shell: waitpid");
        log_msg("ERROR", "waitpid(%d) basarisiz: %s", (int)pid, strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        last_exit_code = WEXITSTATUS(status);
        log_msg("INFO", "cmd='%s' child_pid=%d exit=%d",
                argv[0], (int)pid, last_exit_code);
    } else if (WIFSIGNALED(status)) {
        last_exit_code = 128 + WTERMSIG(status);
        log_msg("WARN", "cmd='%s' child_pid=%d sinyal=%d",
                argv[0], (int)pid, WTERMSIG(status));
    }

    return 0;
}
