#ifndef FILE_CALLHIST_REMOTE_H
#define FILE_CALLHIST_REMOTE_H
#include "Arduino.h"
#include "decl.h"
extern int callhist_at; // 0: main CPU, 1: sub CPU
bool load_callhist_subcpu(const char *fn);
void clear_callhist_subcpu_main();
void process_callhist_control_response_main(const char *buf);
void process_callhist_reset_subcpu(const char *s);
void process_callhist_entry_subcpu(char *s);
void process_callhist_end_subcpu();
bool search_callhist_subcpu_local(const char *call, char *exch, size_t exch_size);
int append_callhist_partial_subcpu(const char *call, struct check_entry_list *entry_list,
                                   int max_entries);
int get_callhist_subcpu_count();
size_t get_callhist_subcpu_bytes();
bool get_callhist_subcpu_entry(int index, const char **call, const char **exch);
#endif
