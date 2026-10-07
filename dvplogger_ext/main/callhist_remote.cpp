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
#include "decl.h"
#include "variables.h"
#include "settings.h"
#include "callhist_remote.h"
#include "dupechk.h"
#include "mux_transport.h"
#include <stdarg.h>
#ifdef DVPLOGGER_EXT
#include "packed_string.h"
#include "esp_heap_caps.h"

#define RCH_CHECKPOINT_SHIFT 4U
#define RCH_CHECKPOINT_STRIDE (1U << RCH_CHECKPOINT_SHIFT)
#define RCH_HEAP_RESERVE (12U * 1024U)

static uint8_t *rch_call_pool = NULL;
static uint8_t *rch_call_len5 = NULL;
static uint16_t *rch_call_checkpoint = NULL;
static uint8_t *rch_exch_pool = NULL;
static uint8_t *rch_exch_len = NULL;
static uint16_t *rch_exch_checkpoint = NULL;
static size_t rch_call_pool_capacity = 0, rch_call_pool_used = 0;
static size_t rch_exch_pool_capacity = 0, rch_exch_pool_used = 0;
static int rch_capacity = 0, rch_count = 0;
static int rch_last_seq = 0;
static size_t rch_bytes = 0;
static size_t rch_mem_bytes = 0;

static void free_remote_callhist(void) {
  free(rch_call_pool); rch_call_pool = NULL;
  free(rch_call_len5); rch_call_len5 = NULL;
  free(rch_call_checkpoint); rch_call_checkpoint = NULL;
  free(rch_exch_pool); rch_exch_pool = NULL;
  free(rch_exch_len); rch_exch_len = NULL;
  free(rch_exch_checkpoint); rch_exch_checkpoint = NULL;
  rch_call_pool_capacity = rch_call_pool_used = 0;
  rch_exch_pool_capacity = rch_exch_pool_used = 0;
  rch_capacity = rch_count = rch_last_seq = 0;
  rch_bytes = rch_mem_bytes = 0;
}

static size_t rch_call_offset(int index) {
  if (index < 0 || index >= rch_count || !rch_call_checkpoint || !rch_call_len5)
    return (size_t)-1;
  const int base = index & ~(int)(RCH_CHECKPOINT_STRIDE - 1U);
  size_t off = rch_call_checkpoint[(unsigned)base >> RCH_CHECKPOINT_SHIFT];
  for (int i = base; i < index; ++i)
    off += packed6_size(packed_len5_get(rch_call_len5, (uint16_t)i));
  return off;
}

static const uint8_t *rch_call_ptr(int index, uint8_t *len) {
  const size_t off = rch_call_offset(index);
  if (off == (size_t)-1 || off >= rch_call_pool_used) return NULL;
  const uint8_t l = packed_len5_get(rch_call_len5, (uint16_t)index);
  const size_t n = packed6_size(l);
  if (!l || off + n > rch_call_pool_used) return NULL;
  if (len) *len = l;
  return rch_call_pool + off;
}

static size_t rch_exch_offset(int index) {
  if (index < 0 || index >= rch_count || !rch_exch_checkpoint || !rch_exch_len)
    return (size_t)-1;
  const int base = index & ~(int)(RCH_CHECKPOINT_STRIDE - 1U);
  size_t off = rch_exch_checkpoint[(unsigned)base >> RCH_CHECKPOINT_SHIFT];
  for (int i = base; i < index; ++i) off += rch_exch_len[i];
  return off;
}

static bool rch_decode_call(int index, char *dst, size_t n) {
  uint8_t len = 0;
  const uint8_t *p = rch_call_ptr(index, &len);
  return p && packed6_decode(p, packed6_size(len), len, true, dst, n);
}

static bool rch_decode_exch(int index, char *dst, size_t n) {
  if (!dst || n == 0 || index < 0 || index >= rch_count) return false;
  const size_t off = rch_exch_offset(index);
  if (off == (size_t)-1 || off > rch_exch_pool_used) return false;
  const size_t len = rch_exch_len[index];
  if (off + len > rch_exch_pool_used) return false;
  const size_t copy = (len < n - 1) ? len : n - 1;
  if (copy) memcpy(dst, rch_exch_pool + off, copy);
  dst[copy] = '\0';
  return true;
}

static void send_callhist_diag(const char *fmt, ...) {
  char msg[160];
  memcpy(msg, "chdiag:", 7);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg + 7, sizeof(msg) - 7, fmt, ap);
  va_end(ap);
  msg[sizeof(msg) - 1] = '\0';
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)msg, strlen(msg));
}

static void send_callhist_ack(int seq, bool ok) {
  char b[64];
  snprintf(b, sizeof(b), "chack:%d|%d|%d", seq, rch_count, ok ? 1 : 0);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
}

void process_callhist_reset_subcpu(const char *s) {
  int n = 0;
  unsigned call_bytes = 0, exch_bytes = 0;
  if (s) {
    int parsed = sscanf(s, "%d|%u|%u", &n, &call_bytes, &exch_bytes);
    if (parsed < 2 && n > 0) call_bytes = (unsigned)n * PACKED6_CALL_MAX_BYTES;
    if (parsed < 3 && n > 0) exch_bytes = (unsigned)n * LEN_EXCH;
  }

  free_remote_callhist();
  if (n <= 0) {
    if (n < 0) send_callhist_diag("reset invalid entries=%d", n);
    return;
  }
  if (n > 5000 || call_bytes == 0 || exch_bytes > 65535U || call_bytes > 65535U) {
    send_callhist_diag("reset invalid entries=%d callpool=%u exchpool=%u",
                       n, call_bytes, exch_bytes);
    return;
  }

  const size_t len5_bytes = ((size_t)n * 5U + 7U) / 8U;
  const size_t checkpoints = ((size_t)n + RCH_CHECKPOINT_STRIDE - 1U) /
                             RCH_CHECKPOINT_STRIDE;
  const size_t need = (size_t)call_bytes + (size_t)exch_bytes + len5_bytes +
                      (size_t)n + checkpoints * sizeof(uint16_t) * 2U;
  const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (need + RCH_HEAP_RESERVE > free8) {
    printf("CALLHIST SUBCPU alloc refused entries=%d need=%u free=%u reserve=%u\r\n",
           n, (unsigned)need, (unsigned)free8, (unsigned)RCH_HEAP_RESERVE);
    send_callhist_diag("alloc refused entries=%d need=%u free=%u reserve=%u",
                       n, (unsigned)need, (unsigned)free8,
                       (unsigned)RCH_HEAP_RESERVE);
    return;
  }

  rch_call_pool = (uint8_t *)malloc(call_bytes ? call_bytes : 1U);
  rch_call_len5 = (uint8_t *)calloc(len5_bytes ? len5_bytes : 1U, 1U);
  rch_call_checkpoint = (uint16_t *)calloc(checkpoints ? checkpoints : 1U,
                                            sizeof(uint16_t));
  rch_exch_pool = (uint8_t *)malloc(exch_bytes ? exch_bytes : 1U);
  rch_exch_len = (uint8_t *)calloc((size_t)n, 1U);
  rch_exch_checkpoint = (uint16_t *)calloc(checkpoints ? checkpoints : 1U,
                                            sizeof(uint16_t));

  if (!rch_call_pool || !rch_call_len5 || !rch_call_checkpoint ||
      !rch_exch_pool || !rch_exch_len || !rch_exch_checkpoint) {
    printf("CALLHIST SUBCPU alloc failed entries=%d need=%u free=%u\r\n",
           n, (unsigned)need,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    send_callhist_diag("alloc failed entries=%d need=%u free=%u",
                       n, (unsigned)need,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    free_remote_callhist();
    return;
  }

  rch_capacity = n;
  rch_call_pool_capacity = call_bytes;
  rch_exch_pool_capacity = exch_bytes;
  rch_mem_bytes = need;
  printf("CALLHIST PACKED init entries=%d callpool=%u exchpool=%u meta=%u total=%u free=%u\r\n",
         n, call_bytes, exch_bytes,
         (unsigned)(need - call_bytes - exch_bytes), (unsigned)need,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  send_callhist_diag("reset OK entries=%d total=%u free=%u largest=%u",
                     n, (unsigned)need,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void process_callhist_entry_subcpu(char *s) {
  char *p = strchr(s, '|');
  if (!p) {
    send_callhist_diag("entry malformed missing seq separator");
    return;
  }
  *p++ = '\0';
  int seq = atoi(s);

  if (seq == rch_last_seq) {
    send_callhist_ack(seq, true);
    if (seq <= 3) send_callhist_diag("entry re-ACK seq=%d count=%d", seq, rch_count);
    return;
  }
  if (seq != rch_last_seq + 1) {
    send_callhist_ack(seq, false);
    send_callhist_diag("entry out-of-order seq=%d expected=%d count=%d",
                       seq, rch_last_seq + 1, rch_count);
    return;
  }

  char *exch = strchr(p, '|');
  if (!exch) {
    send_callhist_ack(seq, false);
    send_callhist_diag("entry malformed seq=%d missing exch", seq);
    return;
  }
  *exch++ = '\0';

  const size_t call_len = strlen(p);
  const size_t exch_len = strlen(exch);
  uint8_t packed[PACKED6_CALL_MAX_BYTES];
  const size_t packed_bytes = packed6_encode(p, true, packed, sizeof(packed));
  if (!rch_call_pool || !rch_exch_pool) {
    send_callhist_ack(seq, false);
    send_callhist_diag("entry reject seq=%d DB not initialized cap=%d count=%d",
                       seq, rch_capacity, rch_count);
    return;
  }
  if (rch_count >= rch_capacity || call_len == 0 || call_len > LEN_CALLSIGN ||
      exch_len > LEN_EXCH || packed_bytes == 0 ||
      rch_call_pool_used + packed_bytes > rch_call_pool_capacity ||
      rch_exch_pool_used + exch_len > rch_exch_pool_capacity) {
    send_callhist_ack(seq, false);
    send_callhist_diag("entry reject seq=%d count=%d/%d call=%u exch=%u packed=%u calluse=%u/%u exchuse=%u/%u",
                       seq, rch_count, rch_capacity, (unsigned)call_len,
                       (unsigned)exch_len, (unsigned)packed_bytes,
                       (unsigned)rch_call_pool_used, (unsigned)rch_call_pool_capacity,
                       (unsigned)rch_exch_pool_used, (unsigned)rch_exch_pool_capacity);
    return;
  }

  if ((rch_count & (int)(RCH_CHECKPOINT_STRIDE - 1U)) == 0) {
    const unsigned ci = (unsigned)rch_count >> RCH_CHECKPOINT_SHIFT;
    rch_call_checkpoint[ci] = (uint16_t)rch_call_pool_used;
    rch_exch_checkpoint[ci] = (uint16_t)rch_exch_pool_used;
  }
  memcpy(rch_call_pool + rch_call_pool_used, packed, packed_bytes);
  packed_len5_set(rch_call_len5, (uint16_t)rch_count, (uint8_t)call_len);
  rch_call_pool_used += packed_bytes;

  if (exch_len) memcpy(rch_exch_pool + rch_exch_pool_used, exch, exch_len);
  rch_exch_len[rch_count] = (uint8_t)exch_len;
  rch_exch_pool_used += exch_len;

  rch_bytes += call_len + exch_len + 2U;
  rch_count++;
  rch_last_seq = seq;
  send_callhist_ack(seq, true);
  if (seq <= 3)
    send_callhist_diag("entry OK seq=%d count=%d calluse=%u/%u exchuse=%u/%u",
                       seq, rch_count,
                       (unsigned)rch_call_pool_used, (unsigned)rch_call_pool_capacity,
                       (unsigned)rch_exch_pool_used, (unsigned)rch_exch_pool_capacity);
}

void process_callhist_end_subcpu() {
  char b[96];
  snprintf(b, sizeof(b), "chdone:%d|%u|%u", rch_count,
           (unsigned)rch_bytes, (unsigned)rch_mem_bytes);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
}

bool search_callhist_subcpu_local(const char *call, char *exch, size_t n) {
  if (!call || !*call) return false;
  uint8_t query[PACKED6_CALL_MAX_BYTES];
  const size_t qlen = strlen(call);
  const size_t qbytes = (qlen <= LEN_CALLSIGN)
                            ? packed6_encode(call, true, query, sizeof(query)) : 0;

  // Fast common case: exact raw callsign comparison stays entirely packed.
  if (qbytes) {
    for (int i = rch_count - 1; i >= 0; --i) {
      uint8_t len = 0;
      const uint8_t *p = rch_call_ptr(i, &len);
      if (p && packed6_equal(p, len, query, (uint8_t)qlen)) {
        return rch_decode_exch(i, exch, n);
      }
    }
  }

  // Portable-suffix normalization is uncommon; decode only this fallback pass.
  char decoded[LEN_CALLSIGN + 1];
  for (int i = rch_count - 1; i >= 0; --i) {
    if (!rch_decode_call(i, decoded, sizeof(decoded))) continue;
    if (dupe_callsign_equal(decoded, call)) return rch_decode_exch(i, exch, n);
  }
  return false;
}

static bool rch_partial_match(int index, const char *pattern, bool *exact_match) {
  if (exact_match) *exact_match = false;
  uint8_t len = 0;
  const uint8_t *stored = rch_call_ptr(index, &len);
  if (!stored || !pattern || !*pattern) return false;

  bool match = false;
  if (strchr(pattern, '-') != NULL) {
    packed6_masked_query q;
    match = packed6_make_masked_query(pattern, '-', &q) &&
            packed6_match_masked(stored, len, &q);
  } else {
    const size_t plen = strlen(pattern);
    uint8_t packed[PACKED6_CALL_MAX_BYTES];
    const size_t n = (plen <= LEN_CALLSIGN)
                         ? packed6_encode(pattern, true, packed, sizeof(packed)) : 0;
    if (n) match = packed6_contains(stored, len, packed, (uint8_t)plen);

    // Exact comparison also accepts the portable suffix normalization rules.
    if (!match) {
      char decoded[LEN_CALLSIGN + 1];
      if (rch_decode_call(index, decoded, sizeof(decoded)) &&
          dupe_callsign_equal(decoded, pattern)) {
        match = true;
        if (exact_match) *exact_match = true;
      }
    } else if (exact_match) {
      char decoded[LEN_CALLSIGN + 1];
      if (rch_decode_call(index, decoded, sizeof(decoded)))
        *exact_match = dupe_callsign_equal(decoded, pattern);
    }
  }
  return match;
}

bool get_callhist_subcpu_match(int index, const char *pattern, bool *exact_match,
                               char *call, size_t call_size,
                               char *exch, size_t exch_size) {
  if (index < 0 || index >= rch_count ||
      !rch_partial_match(index, pattern, exact_match)) return false;
  return rch_decode_call(index, call, call_size) &&
         rch_decode_exch(index, exch, exch_size);
}

int append_callhist_partial_subcpu(const char *call, struct check_entry_list *list,
                                   int maxe) {
  int added = 0;
  for (int i = 0; i < rch_count && list->nentry < maxe; ++i) {
    char c[LEN_CALLSIGN + 1], e[LEN_EXCH + 1];
    bool exact = false;
    if (!get_callhist_subcpu_match(i, call, &exact, c, sizeof(c), e, sizeof(e)))
      continue;
    bool dup = false;
    for (int j = 0; j < list->nentry; ++j)
      if (!strcmp(list->entryl[j].callsign, c)) { dup = true; break; }
    if (dup) continue;
    check_entry *ce = &list->entryl[list->nentry++];
    memset(ce, 0, sizeof(*ce));
    strncpy(ce->callsign, c, sizeof(ce->callsign) - 1);
    strncpy(ce->exch, e, sizeof(ce->exch) - 1);
    ce->flag = CHECK_ENTRY_FLAG_CALLHIST_LIST;
    if (exact) ce->flag |= CHECK_ENTRY_FLAG_EXACT_MATCH;
    added++;
  }
  return added;
}

int get_callhist_subcpu_count() { return rch_count; }
size_t get_callhist_subcpu_bytes() { return rch_mem_bytes; }
bool get_callhist_subcpu_entry(int i, const char **c, const char **e) {
  static char callbuf[LEN_CALLSIGN + 1];
  static char exchbuf[LEN_EXCH + 1];
  if (i < 0 || i >= rch_count ||
      !rch_decode_call(i, callbuf, sizeof(callbuf)) ||
      !rch_decode_exch(i, exchbuf, sizeof(exchbuf))) return false;
  *c = callbuf; *e = exchbuf;
  return true;
}
#else
#include "SD.h"
static volatile bool ch_done = false;
static int ch_count = 0;
static size_t ch_bytes = 0;
static volatile bool ch_ack_received = false;
static int ch_ack_seq = 0;
static int ch_ack_count = 0;
static int ch_ack_ok = 0;

void process_callhist_control_response_main(const char *b) {
  if (!strncmp(b, "chdone:", 7)) {
    unsigned n = 0, sz = 0;
    if (sscanf(b + 7, "%u|%u", &n, &sz) == 2) {
      ch_count = n;
      ch_bytes = sz;
      ch_done = true;
    }
    return;
  }

  if (!strncmp(b, "chack:", 6)) {
    int seq = 0, count = 0, ok = 0;
    if (sscanf(b + 6, "%d|%d|%d", &seq, &count, &ok) == 3) {
      ch_ack_seq = seq;
      ch_ack_count = count;
      ch_ack_ok = ok;
      ch_ack_received = true;
    }
  }
}

static bool send_callhist_entry_with_ack(int seq, const char *packet) {
  const int max_retries = 3;
  const uint32_t ack_timeout_ms = 250;

  for (int attempt = 0; attempt < max_retries; attempt++) {
    ch_ack_received = false;
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                           (unsigned char *)packet, strlen(packet));

    uint32_t deadline = millis() + ack_timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
      if (f_mux_transport) mux_transport.recv_pkt();
      if (ch_ack_received) {
        if (ch_ack_seq == seq) {
          return ch_ack_ok != 0 && ch_ack_count == seq;
        }
        /* Ignore a stale ACK and continue waiting for this sequence. */
        ch_ack_received = false;
      }
      delay(1);
    }
  }

  return false;
}

bool load_callhist_subcpu(const char *fn) {
  File f = SD.open(fn, FILE_READ);
  if (!f) {
    console->printf("callhist: cannot open %s\n", fn);
    return false;
  }

  char line[128];
  int count = 0;
  while (readline(&f, line, 0x0d0a, sizeof(line)) != 0)
    if (line[0]) count++;
  f.close();

  char b[160];
  snprintf(b, sizeof(b), "chreset%d", count);
  ch_done = false;
  ch_ack_received = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
  delay(10);

  f = SD.open(fn, FILE_READ);
  if (!f) return false;

  int sent = 0;
  while (readline(&f, line, 0x0d0a, sizeof(line)) != 0) {
    char *p = line;
    while (*p == ' ') p++;
    char *sp = strchr(p, ' ');
    if (!sp) continue;
    *sp++ = '\0';
    while (*sp == ' ') sp++;
    if (!*p || !*sp) continue;

    int seq = sent + 1;
    snprintf(b, sizeof(b), "che%d|%.*s|%.*s",
             seq, LEN_CALLSIGN, p, LEN_EXCH, sp);
    if (!send_callhist_entry_with_ack(seq, b)) {
      console->printf("callhist transfer failed at seq=%d sent=%d\n",
                      seq, sent);
      f.close();
      clear_callhist_subcpu_main();
      return false;
    }
    sent = seq;
  }
  f.close();

  ch_done = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"chend", 5);
  uint32_t deadline = millis() + 3000;
  while (!ch_done && (int32_t)(millis() - deadline) < 0) {
    if (f_mux_transport) mux_transport.recv_pkt();
    delay(1);
  }

  console->printf(
      "subcpu callhist: received=%d sent=%d bytes=%u done=%d\n",
      ch_count, sent, (unsigned)ch_bytes, ch_done ? 1 : 0);

  return ch_done && ch_count == sent;
}

void clear_callhist_subcpu_main(){const char*b="chreset0";mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char*)b,strlen(b));}
void process_callhist_reset_subcpu(const char*){} void process_callhist_entry_subcpu(char*){} void process_callhist_end_subcpu(){}
bool search_callhist_subcpu_local(const char*,char*,size_t){return false;} int append_callhist_partial_subcpu(const char*,check_entry_list*,int){return 0;}
int get_callhist_subcpu_count(){return ch_count;} size_t get_callhist_subcpu_bytes(){return ch_bytes;}
bool get_callhist_subcpu_entry(int,const char**,const char**){return false;}
#endif
