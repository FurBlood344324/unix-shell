#ifndef MSH_HISTORY_H
#define MSH_HISTORY_H

char *readline(const char *prompt);
void  history_add(const char *line);
void  history_print(void);
void  history_load(const char *filename);
void  history_save(const char *filename);
void  history_destroy(void);

#endif
