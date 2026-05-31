#ifndef MSH_HISTORY_H
#define MSH_HISTORY_H

void history_init(void);

void history_destroy(void);

void history_add(const char *command);

void history_print(void);

#endif