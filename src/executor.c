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
        log_msg("INFO", "cmd='%s' child_pid=%d exit=%d",
                argv[0], (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_msg("WARN", "cmd='%s' child_pid=%d sinyal=%d",
                argv[0], (int)pid, WTERMSIG(status));
    }

    return 0;
}
