#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"
#include "history.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static int last_exit_code = 0;
static int should_exit = 0;
static int exit_code = 0;
static sigset_t sigchld_mask;

#define BG_EVENT_CAPACITY 64

static volatile sig_atomic_t bg_head = 0;
static volatile sig_atomic_t bg_tail = 0;
static volatile sig_atomic_t bg_overflow = 0;
static volatile sig_atomic_t bg_pids[BG_EVENT_CAPACITY];
static volatile sig_atomic_t bg_statuses[BG_EVENT_CAPACITY];

static int status_to_exit_code(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static void enqueue_bg_event(pid_t pid, int status)
{
    sig_atomic_t next = (bg_head + 1) % BG_EVENT_CAPACITY;

    if (next == bg_tail) {
        bg_overflow = 1;
        return;
    }

    bg_pids[bg_head] = (sig_atomic_t)pid;
    bg_statuses[bg_head] = (sig_atomic_t)status;
    bg_head = next;
}

static void collect_exited_children(void)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        enqueue_bg_event(pid, status);
    }

    errno = saved_errno;
}

static void sigchld_handler(int signo)
{
    (void)signo;
    collect_exited_children();
}

static int block_sigchld(sigset_t *oldmask)
{
    return sigprocmask(SIG_BLOCK, &sigchld_mask, oldmask);
}

static void restore_sigmask(const sigset_t *oldmask)
{
    sigprocmask(SIG_SETMASK, oldmask, NULL);
}

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
        perror("malloc");
        log_msg("ERROR", "malloc failed for cd expansion");
        return NULL;
    }

    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, arg + 1, suffix_len + 1);
    return expanded;
}

static void pipe_child(int close_fd, int dup_fd, int target_fd, char **argv)
{
    sigset_t empty;

    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);

    close(close_fd);
    if (dup2(dup_fd, target_fd) < 0) {
        perror("shell");
        _exit(1);
    }
    close(dup_fd);

    if (run_builtin(argv, 0) < 0) {
        execvp(argv[0], argv);
        perror("shell");
        _exit(127);
    }
    _exit(last_exit_code);
}

static int wait_for_child(pid_t pid, int set_exit)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) {
        perror("shell");
        log_msg("ERROR", "waitpid failed");
        return -1;
    }
    if (set_exit)
        last_exit_code = status_to_exit_code(status);
    return 0;
}

int run_builtin(char **argv, int background)
{
    if (strcmp(argv[0], "exit") == 0) {
        if (background) {
            fprintf(stderr, "msh: exit: cannot be run in background\n");
            return 0;
        }
        if (argv[1] != NULL) {
            char *endptr;
            long val = strtol(argv[1], &endptr, 10);
            if (*endptr != '\0' || val < 0 || val > 255) {
                fprintf(stderr, "exit: numeric argument required\n");
                last_exit_code = 2;
                return 0;
            }
            exit_code = (int)val;
        } else {
            exit_code = last_exit_code;
        }
        should_exit = 1;
        return 0;
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (background) {
            fprintf(stderr, "msh: cd: cannot be run in background\n");
            last_exit_code = 1;
            return 0;
        }

        char old_cwd[4096];
        old_cwd[0] = '\0';
        getcwd(old_cwd, sizeof(old_cwd));

        char *target = NULL;

        if (argv[1] != NULL && strcmp(argv[1], "-") == 0) {
            target = getenv("OLDPWD");
            if (target == NULL) {
                fprintf(stderr, "cd: OLDPWD not set\n");
                log_msg("ERROR", "cd: OLDPWD not set");
                last_exit_code = 1;
                return 0;
            }
            printf("%s\n", target);
            target = strdup(target);
        } else {
            target = expand_cd_target(argv[1]);
        }

        if (target == NULL) {
            last_exit_code = 1;
            return 0;
        }

        if (chdir(target) < 0) {
            perror("cd");
            last_exit_code = 1;
        } else {
            last_exit_code = 0;
            if (old_cwd[0] != '\0') {
                setenv("OLDPWD", old_cwd, 1);
            }
        }
        free(target);
        return 0;
    }

    if (strcmp(argv[0], "history") == 0) {
        if (background) {
            fprintf(stderr, "msh: history: cannot be run in background\n");
            last_exit_code = 1;
            return 0;
        }
        history_print();
        last_exit_code = 0;
        return 0;
    }

    return -1;
}

int install_sigchld_handler(void)
{
    struct sigaction sa;

    sigemptyset(&sigchld_mask);
    sigaddset(&sigchld_mask, SIGCHLD);

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    return sigaction(SIGCHLD, &sa, NULL);
}

void run_external(char **argv, int background)
{
    sigset_t oldmask;
    pid_t pid;

    block_sigchld(&oldmask);
    pid = fork();

    if (pid < 0) {
        perror("shell");
        log_msg("ERROR", "fork failed");
        restore_sigmask(&oldmask);
        return;
    }

    if (pid == 0) {
        restore_sigmask(&oldmask);
        execvp(argv[0], argv);
        perror("shell");
        log_msg("ERROR", "execvp failed");
        _exit(127);
    }

    if (background) {
        char log_buf[64];
        snprintf(log_buf, sizeof(log_buf), "background process started: %d", pid);
        log_msg("INFO", log_buf);
        last_exit_code = 0;
    } else {
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("shell");
            log_msg("ERROR", "waitpid failed");
        } else {
            last_exit_code = status_to_exit_code(status);
        }
    }

    restore_sigmask(&oldmask);
}

void run_pipe(char **left_argv, char **right_argv)
{
    int pipefd[2];
    pid_t left_pid, right_pid;
    sigset_t oldmask;

    if (pipe(pipefd) < 0) {
        perror("shell");
        log_msg("ERROR", "pipe failed");
        return;
    }

    block_sigchld(&oldmask);
    left_pid = fork();

    if (left_pid < 0) {
        perror("shell");
        log_msg("ERROR", "fork failed for left pipe child");
        close(pipefd[0]);
        close(pipefd[1]);
        restore_sigmask(&oldmask);
        return;
    }

    if (left_pid == 0) {
        pipe_child(pipefd[0], pipefd[1], STDOUT_FILENO, left_argv);
    }

    right_pid = fork();

    if (right_pid < 0) {
        perror("shell");
        log_msg("ERROR", "fork failed for right pipe child");
        close(pipefd[0]);
        close(pipefd[1]);
        kill(left_pid, SIGKILL);
        waitpid(left_pid, NULL, 0);
        restore_sigmask(&oldmask);
        return;
    }

    if (right_pid == 0) {
        pipe_child(pipefd[1], pipefd[0], STDIN_FILENO, right_argv);
    }

    close(pipefd[0]);
    close(pipefd[1]);

    wait_for_child(left_pid, 0);
    wait_for_child(right_pid, 1);

    restore_sigmask(&oldmask);
}

void flush_background_events(void)
{
    sigset_t oldmask;
    block_sigchld(&oldmask);

    if (bg_overflow) {
        log_msg("WARN", "background event queue overflowed");
        bg_overflow = 0;
    }

    while (bg_tail != bg_head) {
        pid_t pid = (pid_t)bg_pids[bg_tail];
        int status = (int)bg_statuses[bg_tail];
        bg_tail = (bg_tail + 1) % BG_EVENT_CAPACITY;

        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf),
                 "background process %d exited with code %d",
                 pid, status_to_exit_code(status));
        log_msg("INFO", log_buf);
    }

    restore_sigmask(&oldmask);
}

int get_last_exit_code(void)
{
    return last_exit_code;
}

int get_should_exit(void)
{
    return should_exit;
}

int get_exit_code(void)
{
    return exit_code;
}
