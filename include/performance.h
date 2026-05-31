#ifndef MSH_PERFORMANCE_H
#define MSH_PERFORMANCE_H

#include <time.h>

void performance_init(const char *startup_dir);
void record_command_time(struct timespec start, struct timespec end);
void print_performance_summary(void);

#endif
