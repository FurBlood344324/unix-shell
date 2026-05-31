#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_CAPACITY 10

static char *history_buffer[HISTORY_CAPACITY];
static int history_start = 0;
static int history_count = 0;

void history_init(void) {
    for (int i = 0; i < HISTORY_CAPACITY; ++i) {
        history_buffer[i] = NULL;
    }
    log_msg("INFO", "History buffer initialized");
}

void history_destroy(void) {
    for (int i = 0; i < HISTORY_CAPACITY; ++i) {
        free(history_buffer[i]);
    }
    log_msg("INFO", "History buffer flushed");
}

void history_add(const char *command) {
    if (command == NULL || strlen(command) == 0) {
        return;
    }

    int next_index = (history_start + history_count) % HISTORY_CAPACITY;

    if (history_count == HISTORY_CAPACITY) {
        free(history_buffer[history_start]);
        history_start = (history_start + 1) % HISTORY_CAPACITY;
        history_count--;
    }
    
    history_buffer[next_index] = strdup(command);
    history_count++;
}

void history_print(void) {
    for (int i = 0; i < history_count; ++i) {
        int index = (history_start + i) % HISTORY_CAPACITY;
        printf("%d: %s\n", i + 1, history_buffer[index]);
    }
}