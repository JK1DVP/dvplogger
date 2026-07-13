#ifndef FILE_USER_CONTEST_MD_H
#define FILE_USER_CONTEST_MD_H

#include <stddef.h>

#define USER_MD_CONTEST_ID 44

bool is_user_md_contest_name(const char *contest_name);
bool start_user_md_contest(const char *contest_name);
void process_user_md_contest();
bool user_md_contest_loading();
int user_md_multi_check(const char *exchange, int bandid);
void release_user_md_contest();

#endif
