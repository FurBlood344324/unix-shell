#ifndef MSH_LOG_H
#define MSH_LOG_H

int  log_init(const char *path);
void log_close(void);
void log_msg(const char *level, const char *fmt, ...);

#endif
