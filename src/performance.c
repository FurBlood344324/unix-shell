#include "performance.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static long command_count = 0;
static double total_time_ms = 0.0;
static double min_time_ms = -1.0;
static double max_time_ms = 0.0;
static char perf_path[1024];

void performance_init(const char *startup_dir)
{
    snprintf(perf_path, sizeof(perf_path), "%s/performance.txt", startup_dir);
}

void record_command_time(struct timespec start, struct timespec end)
{
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                        (end.tv_nsec - start.tv_nsec) / 1000000.0;

    char log_buffer[128];
    snprintf(log_buffer, sizeof(log_buffer), "command execution time: %.2f ms", elapsed_ms);
    log_msg("PERF", log_buffer);

    command_count++;
    total_time_ms += elapsed_ms;
    if (min_time_ms < 0 || elapsed_ms < min_time_ms) {
        min_time_ms = elapsed_ms;
    }
    if (elapsed_ms > max_time_ms) {
        max_time_ms = elapsed_ms;
    }
}

void print_performance_summary(void)
{
    if (command_count == 0) return;

    FILE *fp = fopen(perf_path, "w");
    if (!fp) return;

    double avg_time_ms = total_time_ms / command_count;
    fprintf(fp, "--- Performans Ozeti ---\n");
    fprintf(fp, "Calistirilan komut sayisi: %ld\n", command_count);
    fprintf(fp, "Ortalama calisma suresi: %.2f ms\n", avg_time_ms);
    fprintf(fp, "Minimum calisma suresi: %.2f ms\n", min_time_ms);
    fprintf(fp, "Maksimum calisma suresi: %.2f ms\n", max_time_ms);
    fprintf(fp, "------------------------\n");
    fclose(fp);
}
