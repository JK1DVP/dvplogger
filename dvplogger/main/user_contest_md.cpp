/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "Arduino.h"
#include "SD.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "decl.h"
#include "variables.h"
#include "multi.h"
#include "multi_process.h"
#include "dupechk.h"
#include "display.h"
#include "contest.h"
#include "user_contest_md.h"
#include "cp932_utf8.h"

extern struct multi_list multi_list;

namespace {

enum UserMdState {
  USER_MD_IDLE = 0,
  USER_MD_OPEN,
  USER_MD_READ,
  USER_MD_PARSE,
  USER_MD_DONE,
  USER_MD_ERROR
};

enum UserMdWildcard : uint8_t {
  USER_MD_EXACT = 0,
  USER_MD_ANY_FIXED,
  USER_MD_ANY_VARIABLE,
  USER_MD_ALPHA_VARIABLE
};

struct UserMdContext {
  UserMdState state;
  File file;
  char filename[24];
  char contest_name[LEN_CONTEST_NAME + 1];
  char *buffer;
  char *name_pool;
  size_t name_pool_size;
  size_t name_pool_used;
  bool source_is_cp932;
  size_t file_size;
  size_t bytes_read;
  struct multi_item *table;
  uint8_t *wildcard_type;
  uint8_t *wildcard_length;
  int count;
  bool allow_other;
  bool score_format;
  int dupe_mask;
  char error[64];
  uint32_t next_progress_ms;
};

static UserMdContext ctx = {};
static struct multi_item *active_table = NULL;
static char *active_buffer = NULL;
static char *active_name_pool = NULL;
static uint8_t *active_wildcard_type = NULL;
static uint8_t *active_wildcard_length = NULL;
static int active_count = 0;
static bool active_allow_other = false;

static void *user_alloc(size_t size) {
  if (f_spiram) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) return p;
  }
  return malloc(size);
}

static void reset_loading_objects() {
  if (ctx.file) ctx.file.close();
  if (ctx.table != NULL) free(ctx.table);
  if (ctx.buffer != NULL) free(ctx.buffer);
  if (ctx.name_pool != NULL) free(ctx.name_pool);
  if (ctx.wildcard_type != NULL) free(ctx.wildcard_type);
  if (ctx.wildcard_length != NULL) free(ctx.wildcard_length);
  ctx.table = NULL;
  ctx.buffer = NULL;
  ctx.name_pool = NULL;
  ctx.wildcard_type = NULL;
  ctx.wildcard_length = NULL;
  ctx.file_size = 0;
  ctx.name_pool_size = 0;
  ctx.name_pool_used = 0;
  ctx.source_is_cp932 = false;
  ctx.bytes_read = 0;
  ctx.count = 0;
}

static void set_error(const char *message) {
  snprintf(ctx.error, sizeof(ctx.error), "%s", message != NULL ? message : "unknown error");
  ctx.state = USER_MD_ERROR;
}

static bool valid_basename(const char *name) {
  size_t len = strlen(name);
  if (len == 0 || len > 16) return false;
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

static bool token_is_integer(const char *s) {
  if (s == NULL || *s == '\0') return false;
  if (*s == '+' || *s == '-') ++s;
  if (*s == '\0') return false;
  while (*s != '\0') {
    if (!isdigit((unsigned char)*s)) return false;
    ++s;
  }
  return true;
}

static int tokenize(char *buffer, char **tokens, int max_tokens) {
  int n = 0;
  char *saveptr = NULL;
  char *tok = strtok_r(buffer, " \t\r\n", &saveptr);
  while (tok != NULL && n < max_tokens) {
    tokens[n++] = tok;
    tok = strtok_r(NULL, " \t\r\n", &saveptr);
  }
  return n;
}

static bool detect_score_format(char **tokens, int ntokens) {
  // CTESTWIN has two incompatible token layouts:
  //   code name
  //   code name points
  // Numeric multiplier codes make a local "third token is numeric" test
  // ambiguous.  Accept the score format only when the complete token stream
  // can be consumed consistently as three-token records.
  int normal_records = 0;

  for (int i = 0; i < ntokens;) {
    const char *t = tokens[i];

    // Score-format-only three-token records.
    if (strcmp(t, "$") == 0 || strcmp(t, "&") == 0) {
      if (i + 2 >= ntokens || !token_is_integer(tokens[i + 2])) return false;
      i += 3;
      continue;
    }

    // In the score format contest options are written as "% M 0".
    if (strcmp(t, "%") == 0) {
      if (i + 2 >= ntokens) return false;
      i += 3;
      continue;
    }

    // "%M 0", "%D 0", etc. are the two-token, no-score syntax.
    if (t[0] == '%' && t[1] != '\0') return false;

    // Explanatory records also carry a dummy score in score-format files.
    if (strcmp(t, "|") == 0) {
      if (i + 2 >= ntokens || !token_is_integer(tokens[i + 2])) return false;
      i += 3;
      continue;
    }

    // The fallback definition is "* * points" in score-format files.
    if (strcmp(t, "*") == 0 && i + 1 < ntokens && strcmp(tokens[i + 1], "*") == 0) {
      if (i + 2 >= ntokens || !token_is_integer(tokens[i + 2])) return false;
      i += 3;
      continue;
    }

    // Every ordinary score-format multiplier must have an integer third field.
    if (i + 2 >= ntokens || !token_is_integer(tokens[i + 2])) return false;
    ++normal_records;
    i += 3;
  }

  return normal_records > 0;
}

static char *store_multi_name(const char *name) {
  if (name == NULL) return NULL;
  size_t src_len = strlen(name);
  size_t need = ctx.source_is_cp932 ? src_len * 3 + 1 : src_len + 1;
  if (ctx.name_pool == NULL || ctx.name_pool_used + need > ctx.name_pool_size) {
    set_error("name pool exhausted");
    return NULL;
  }
  char *dst = ctx.name_pool + ctx.name_pool_used;
  size_t used;
  if (ctx.source_is_cp932) {
    used = cp932_to_utf8((const uint8_t *)name, src_len, dst,
                         ctx.name_pool_size - ctx.name_pool_used);
  } else {
    memcpy(dst, name, src_len + 1);
    used = src_len;
  }
  ctx.name_pool_used += used + 1;
  return dst;
}

static bool add_multi(char *pattern, char *name) {
  if (ctx.count >= N_MULTI - 1) {
    set_error("too many multipliers");
    return false;
  }
  if (strchr(pattern, '#') != NULL) {
    // Dynamic # multipliers are intentionally deferred to a later implementation.
    return true;
  }

  size_t base_len = strcspn(pattern, "?*%");
  UserMdWildcard type = USER_MD_EXACT;
  uint8_t wildcard_len = 0;
  char suffix = pattern[base_len];

  if (suffix == '?') {
    type = USER_MD_ANY_FIXED;
    size_t p = base_len;
    while (pattern[p] == '?' && wildcard_len < 255) {
      ++wildcard_len;
      ++p;
    }
    if (pattern[p] != '\0') return true; // unsupported mixed pattern: ignore safely
  } else if (suffix == '*') {
    type = USER_MD_ANY_VARIABLE;
    wildcard_len = 0;
    if (pattern[base_len + 1] != '\0') return true;
  } else if (suffix == '%') {
    type = USER_MD_ALPHA_VARIABLE;
    wildcard_len = 0;
    if (pattern[base_len + 1] != '\0') return true;
  }

  pattern[base_len] = '\0';
  if (*pattern == '\0' || *name == '\0') return true;

  char *stored_name = store_multi_name(name);
  if (stored_name == NULL) return false;

  ctx.table->mul[ctx.count] = pattern;
  ctx.table->name[ctx.count] = stored_name;
  ctx.wildcard_type[ctx.count] = (uint8_t)type;
  ctx.wildcard_length[ctx.count] = wildcard_len;
  if (ctx.count < 30) {
    plogw->ostream->printf(
        "MD multi[%d]: code=[%s] name=[%s] type=%d len=%u\n",
        ctx.count,
        pattern,
        name,
        (int)type,
        (unsigned int)wildcard_len);
  }  
  ++ctx.count;
  return true;
}

static bool parse_md_buffer() {
  // One token pointer per two bytes is a safe upper estimate for whitespace-delimited text.
  size_t max_tokens_sz = ctx.file_size / 2 + 8;
  if (max_tokens_sz > 32767) max_tokens_sz = 32767;
  char **tokens = (char **)user_alloc(max_tokens_sz * sizeof(char *));
  if (tokens == NULL) {
    set_error("token memory allocation failed");
    return false;
  }

  if (ctx.file_size >= 3 &&
      (uint8_t)ctx.buffer[0] == 0xEF &&
      (uint8_t)ctx.buffer[1] == 0xBB &&
      (uint8_t)ctx.buffer[2] == 0xBF) {
    memmove(ctx.buffer, ctx.buffer + 3, ctx.file_size - 2);
    ctx.file_size -= 3;
  }
  ctx.source_is_cp932 = !is_valid_utf8_bytes((const uint8_t *)ctx.buffer, ctx.file_size);
  int ntokens = tokenize(ctx.buffer, tokens, (int)max_tokens_sz);
  ctx.score_format = detect_score_format(tokens, ntokens);

  plogw->ostream->printf(
      "USER MD: file_size=%u tokens=%d score_format=%d\n",
      (unsigned int)ctx.file_size,
      ntokens,
      ctx.score_format ? 1 : 0);

  int debug_tokens = ntokens;
  if (debug_tokens > 40) debug_tokens = 40;

  for (int j = 0; j < debug_tokens; ++j) {
    plogw->ostream->printf("[%02d] ", j);
    plogw->ostream->println(tokens[j]);
  }
  
  ctx.dupe_mask = CW_PH_DUPE_NG;  
  ctx.allow_other = false;

  for (int i = 0; i < ntokens;) {
    char *t = tokens[i];

    if (strcmp(t, "$") == 0 || strcmp(t, "&") == 0) {
      i += (i + 2 < ntokens) ? 3 : 1;
      continue;
    }
    if (strcmp(t, "|") == 0) {
      i += ctx.score_format ? 3 : 2;
      continue;
    }
    if (strcmp(t, "%") == 0) {
      if (i + 2 >= ntokens) break;
      char *key = tokens[i + 1];
      char *value = tokens[i + 2];
      if (strcasecmp(key, "M") == 0) {
        ctx.dupe_mask = atoi(value) == 1 ? CW_PH_DUPE_OK : CW_PH_DUPE_NG;
      }
      i += 3;
      continue;
    }
    if (t[0] == '%' && t[1] != '\0') {
      if (i + 1 >= ntokens) break;
      if (strcasecmp(t, "%M") == 0) {
        ctx.dupe_mask = atoi(tokens[i + 1]) == 1 ? CW_PH_DUPE_OK : CW_PH_DUPE_NG;
      }
      // %D, %K and %P... are recognized and consumed, but not applied yet.
      i += 2;
      continue;
    }
    if (strcmp(t, "*") == 0 && i + 1 < ntokens && strcmp(tokens[i + 1], "*") == 0) {
      ctx.allow_other = true;
      i += ctx.score_format && i + 2 < ntokens ? 3 : 2;
      continue;
    }

    int fields = ctx.score_format ? 3 : 2;
    if (i + fields - 1 >= ntokens) break;
    if (!add_multi(tokens[i], tokens[i + 1])) {
      free(tokens);
      return false;
    }
    i += fields;
  }

  free(tokens);
  if (ctx.count == 0) {
    set_error("no supported multiplier found");
    return false;
  }
  return true;
}

static void show_progress() {
  uint32_t now = millis();
  if ((int32_t)(now - ctx.next_progress_ms) < 0) return;
  ctx.next_progress_ms = now + 500;
  char msg[96];
  if (ctx.state == USER_MD_READ) {
    snprintf(msg, sizeof(msg), "User contest\n%s\nReading %u/%u",
             ctx.filename,
             (unsigned)ctx.bytes_read,
             (unsigned)ctx.file_size);
  } else {
    snprintf(msg, sizeof(msg), "User contest\n%s\nLoading...", ctx.filename);
  }
  upd_display_info_flash(msg);
}

static void activate_loaded_table() {
  release_user_md_contest();
  active_table = ctx.table;
  active_buffer = ctx.buffer;
  active_name_pool = ctx.name_pool;
  active_wildcard_type = ctx.wildcard_type;
  active_wildcard_length = ctx.wildcard_length;
  active_count = ctx.count;
  active_allow_other = ctx.allow_other;

  ctx.table = NULL;
  ctx.buffer = NULL;
  ctx.name_pool = NULL;
  ctx.wildcard_type = NULL;
  ctx.wildcard_length = NULL;

  init_multi(NULL, 1, N_BAND - 1);
  init_multi(active_table, -1, -1);
  plogw->contest_id = USER_MD_CONTEST_ID;
  plogw->multi_type = MULTI_TYPE_USER_MD;
  plogw->mask = ctx.dupe_mask;
  plogw->cw_pts = 1;
  sync_dupechk_mask_subcpu(plogw->mask);

  char msg[128];
  snprintf(msg, sizeof(msg), "contest\n%s\nselected.\nD:%s\nMult:%d",
           plogw->contest_name + 2,
           plogw->mask == CW_PH_DUPE_OK ? "OK C/P" : "NG C/P",
           active_count);
  upd_display_info_flash(msg);
  plogw->ostream->printf("Loaded %s: %d multipliers, encoding=%s, CW/Phone %s\n",
                         ctx.filename, active_count,
                         ctx.source_is_cp932 ? "CP932" : "UTF-8",
                         plogw->mask == CW_PH_DUPE_OK ? "separate" : "combined");
}

} // namespace

bool is_user_md_contest_name(const char *contest_name) {
  return contest_name != NULL && strncasecmp(contest_name, "User", 4) == 0;
}

bool start_user_md_contest(const char *contest_name) {
  if (ctx.state != USER_MD_IDLE) {
    upd_display_info_flash("User contest\nload already active");
    return false;
  }
  if (!is_user_md_contest_name(contest_name)) return false;

  const char *base = contest_name + 4;
  if (!valid_basename(base)) {
    upd_display_info_flash("User contest\ninvalid filename\nA-Z 0-9 _ - only");
    return false;
  }

  reset_loading_objects();
  ctx.state = USER_MD_OPEN;
  ctx.dupe_mask = CW_PH_DUPE_NG;
  ctx.allow_other = false;
  ctx.score_format = false;
  ctx.error[0] = '\0';
  ctx.next_progress_ms = 0;
  snprintf(ctx.contest_name, sizeof(ctx.contest_name), "%s", contest_name);
  snprintf(ctx.filename, sizeof(ctx.filename), "/%s.MD", base);
  upd_display_info_flash("User contest\nopening MD file");
  return true;
}

bool user_md_contest_loading() {
  return ctx.state != USER_MD_IDLE;
}

void process_user_md_contest() {
  if (ctx.state == USER_MD_IDLE) return;
  show_progress();

  switch (ctx.state) {
    case USER_MD_OPEN:
      ctx.file = SD.open(ctx.filename, FILE_READ);
      if (!ctx.file || ctx.file.isDirectory()) {
        set_error("MD file not found");
        break;
      }
      ctx.file_size = ctx.file.size();
      if (ctx.file_size == 0 || ctx.file_size > 65535) {
        set_error("invalid MD file size");
        break;
      }
      ctx.buffer = (char *)user_alloc(ctx.file_size + 1);
      ctx.name_pool_size = ctx.file_size * 3 + 64;
      ctx.name_pool = (char *)user_alloc(ctx.name_pool_size);
      ctx.table = (struct multi_item *)user_alloc(sizeof(struct multi_item));
      ctx.wildcard_type = (uint8_t *)user_alloc(N_MULTI);
      ctx.wildcard_length = (uint8_t *)user_alloc(N_MULTI);
      if (ctx.buffer == NULL || ctx.name_pool == NULL || ctx.table == NULL ||
          ctx.wildcard_type == NULL || ctx.wildcard_length == NULL) {
        set_error("MD memory allocation failed");
        break;
      }
      memset(ctx.table, 0, sizeof(struct multi_item));
      memset(ctx.wildcard_type, 0, N_MULTI);
      memset(ctx.wildcard_length, 0, N_MULTI);
      for (int i = 0; i < N_MULTI; ++i) {
        ctx.table->mul[i] = "";
        ctx.table->name[i] = "";
      }
      ctx.state = USER_MD_READ;
      break;

    case USER_MD_READ: {
      uint8_t chunk[256];
      size_t remain = ctx.file_size - ctx.bytes_read;
      size_t want = remain < sizeof(chunk) ? remain : sizeof(chunk);
      int n = ctx.file.read(chunk, want);
      if (n < 0) {
        set_error("MD read failed");
        break;
      }
      if (n > 0) {
        memcpy(ctx.buffer + ctx.bytes_read, chunk, (size_t)n);
        ctx.bytes_read += (size_t)n;
      }
      if (ctx.bytes_read >= ctx.file_size || n == 0) {
        ctx.buffer[ctx.bytes_read] = '\0';
        ctx.file.close();
        ctx.state = USER_MD_PARSE;
      }
      break;
    }

    case USER_MD_PARSE:
      if (parse_md_buffer()) ctx.state = USER_MD_DONE;
      break;

    case USER_MD_DONE:
      activate_loaded_table();
      reset_loading_objects();
      ctx.state = USER_MD_IDLE;
      break;

    case USER_MD_ERROR: {
      char msg[112];
      snprintf(msg, sizeof(msg), "User contest\n%s\n%s", ctx.filename, ctx.error);
      upd_display_info_flash(msg);
      plogw->ostream->printf("Failed to load %s: %s\n", ctx.filename, ctx.error);
      reset_loading_objects();
      ctx.state = USER_MD_IDLE;
      break;
    }

    default:
      break;
  }
}

int user_md_multi_check(const char *exchange, int bandid) {
  if (exchange == NULL || *exchange == '\0') return -1;
  if (bandid <= 0 || bandid > N_BAND) return -1;
  if (active_table == NULL || multi_list.multi[bandid - 1] != active_table) return -1;

  size_t exchange_len = strlen(exchange);
  int best_index = -1;
  size_t best_base_len = 0;

  for (int i = 0; i < active_count; ++i) {
    const char *base = active_table->mul[i];
    size_t base_len = strlen(base);
    if (strncasecmp(exchange, base, base_len) != 0) continue;

    bool match = false;
    switch ((UserMdWildcard)active_wildcard_type[i]) {
      case USER_MD_EXACT:
        match = exchange_len == base_len;
        break;
      case USER_MD_ANY_FIXED:
        match = exchange_len == base_len + active_wildcard_length[i];
        break;
      case USER_MD_ANY_VARIABLE:
        match = exchange_len >= base_len;
        break;
      case USER_MD_ALPHA_VARIABLE:
        match = exchange_len > base_len;
        if (match) {
          for (size_t j = base_len; j < exchange_len; ++j) {
            if (!isalpha((unsigned char)exchange[j])) {
              match = false;
              break;
            }
          }
        }
        break;
    }
    if (match && base_len >= best_base_len) {
      best_index = i;
      best_base_len = base_len;
    }
  }

  if (best_index >= 0) return best_index;
  return active_allow_other ? -2 : -1;
}

void release_user_md_contest() {
  if (active_table != NULL) free(active_table);
  if (active_buffer != NULL) free(active_buffer);
  if (active_name_pool != NULL) free(active_name_pool);
  if (active_wildcard_type != NULL) free(active_wildcard_type);
  if (active_wildcard_length != NULL) free(active_wildcard_length);
  active_table = NULL;
  active_buffer = NULL;
  active_name_pool = NULL;
  active_wildcard_type = NULL;
  active_wildcard_length = NULL;
  active_count = 0;
  active_allow_other = false;
}
