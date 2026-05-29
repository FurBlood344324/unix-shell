#define _POSIX_C_SOURCE 200809L

#include "executor.h"
#include "log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static int last_exit_code = 0;
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

    flush_background_events();
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

static void pipe_child(int close_fd, int dup_fd, int target_fd, char **argv,
                       const sigset_t *oldmask)
{
    restore_sigmask(oldmask);
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

static int wait_for_child(pid_t pid, char **argv, const char *label, int set_exit)
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

int install_sigchld_handler(void)
{
    struct sigaction sa;

    sigemptyset(&sigchld_mask);
    sigaddset(&sigchld_mask, SIGCHLD);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;

    return sigaction(SIGCHLD, &sa, NULL);
}

void flush_background_events(void)
{
    sigset_t oldmask;

    if (block_sigchld(&oldmask) < 0) {
        log_msg("ERROR", "sigprocmask basarisiz: %s", strerror(errno));
        return;
    }

    collect_exited_children();

    while (bg_tail != bg_head) {
        pid_t pid = (pid_t)bg_pids[bg_tail];
        int status = (int)bg_statuses[bg_tail];
        bg_tail = (bg_tail + 1) % BG_EVENT_CAPACITY;

        log_msg("INFO", "bg cmd bitti pid=%d exit=%d",
                (int)pid, status_to_exit_code(status));
    }

    if (bg_overflow) {
        log_msg("WARN", "bg event queue doldu, bazi cikislar loglanamamis olabilir");
        bg_overflow = 0;
    }

    restore_sigmask(&oldmask);
}

int run_pipe(char **left_argv, char **right_argv)
{
    int fd[2];
    sigset_t oldmask;

    if (block_sigchld(&oldmask) < 0) {
        perror("shell: sigprocmask");
        log_msg("ERROR", "sigprocmask basarisiz: %s", strerror(errno));
        return -1;
    }

    if (pipe(fd) < 0) {
        perror("shell: pipe");
        log_msg("ERROR", "pipe basarisiz: %s", strerror(errno));
        restore_sigmask(&oldmask);
        return -1;
    }

    pid_t left_pid = fork();
    if (left_pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        restore_sigmask(&oldmask);
        return -1;
    }

    if (left_pid == 0) {
        pipe_child(fd[0], fd[1], STDOUT_FILENO, left_argv, &oldmask);
    }

    pid_t right_pid = fork();
    if (right_pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        waitpid(left_pid, NULL, 0);
        restore_sigmask(&oldmask);
        return -1;
    }

    if (right_pid == 0) {
        pipe_child(fd[1], fd[0], STDIN_FILENO, right_argv, &oldmask);
    }

    close(fd[0]);
    close(fd[1]);

    wait_for_child(left_pid, left_argv, "pipe-left: ", 0);
    wait_for_child(right_pid, right_argv, "pipe-right: ", 1);
    restore_sigmask(&oldmask);

    return 0;
}

int run_builtin(char **argv, int background)
{
    if (strcmp(argv[0], "cd") == 0) {
        if (background) {
            fprintf(stderr, "shell: built-in commands cannot run in background\n");
            log_msg("WARN", "background built-in reddedildi: %s", argv[0]);
            last_exit_code = 1;
            return 1;
        }
        builtin_cd(argv);
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0) {
        if (background) {
            fprintf(stderr, "shell: built-in commands cannot run in background\n");
            log_msg("WARN", "background built-in reddedildi: %s", argv[0]);
            last_exit_code = 1;
            return 1;
        }
        builtin_exit(argv);
        return 1;
    }
    return 0;
}

int run_external(char **argv, int background)
{
    sigset_t oldmask;

    if (block_sigchld(&oldmask) < 0) {
        perror("shell: sigprocmask");
        log_msg("ERROR", "sigprocmask basarisiz: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("shell: fork");
        log_msg("ERROR", "fork basarisiz: %s", strerror(errno));
        restore_sigmask(&oldmask);
        return -1;
    }

    if (pid == 0) {
        restore_sigmask(&oldmask);
        execvp(argv[0], argv);
        fprintf(stderr, "shell: %s: %s\n", argv[0], strerror(errno));
        log_msg("ERROR", "execvp '%s' basarisiz: %s", argv[0], strerror(errno));
        _exit(127);
    }

    if (background) {
        printf("[%d]\n", (int)pid);
        fflush(stdout);
        log_msg("INFO", "bg cmd baslatildi pid=%d cmd='%s'", (int)pid, argv[0]);
        last_exit_code = 0;
        restore_sigmask(&oldmask);
        return 0;
    }

    int rc = wait_for_child(pid, argv, "", 1);
    restore_sigmask(&oldmask);
    return rc;
}
