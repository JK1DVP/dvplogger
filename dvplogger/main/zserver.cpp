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
// Copyright (c) 2021-2024 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

// zlink  24/6/28 E.Araki
#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "zserver.h"
#include "misc.h"
#include "display.h"
#include "network.h"
#include "AsyncTCP.h"
#include "so2r.h"
#include "qso.h"
#include "cw_keying.h"
#include "timekeep.h"
#include "cp932_utf8.h"
#include "esp_heap_caps.h"
#include "SD.h"


char zserver_server[40] = "";
int zserver_port = 23;
char zserver_buf[NCHR_ZSERVER_RINGBUF];
//WiFiClient zserver_client;

AsyncClient *zserver_client = new AsyncClient;

int f_show_zserver=1;
int zserver_auto_enable=1;
static bool zserver_connected_state=false;
static bool zserver_suppress_disconnect_notice=false;
struct zserver zserver;
// idx                       0      1      2    3     4     5    6      7     8     9     10    11      12      13      14     15
const char *zserver_freqcodes[]={"1.9","3.5","7","10","14","18","21","24","28","50","144 ","430 ","1200","2400","5600","10G","10","18","24" };
int zserver_bandid_freqcodes_map[]={0,0,1,2,4,6,8,9,10,11,12,13,14,15,0,0,0};

//コード	モードa
const char *zserver_modecodes[]={"CW","SSB","FM","AM","RTTY","FT4","FT8","OTHER"};
//電力コード	
//コード	電力

const char *zserver_powcodes[]={"1W","2W","5W","10W","20W","25W","50W","100W","200W","500W","1000W" };

const char *zserver_client_commands[]={ "FREQ","QSOIDS","ENDQSOIDS","PROMPTUPDATE","NEWPX","PUTMESSAGE","!","POSTWANTED",
			"DELWANTED","SPOT","SPOT2","SPOT3","BSDATA","SENDSPOT","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			"CONNECTCLUSTER","PUTQSO","DELQSO","EXDELQSO","INSQSOAT","LOCKQSO","UNLOCKQSO","EDITQSOTO",
			"INSQSO","PUTLOGEX","PUTLOG","RENEW","SENDLOG" };




// 256 slots => 255 usable entries in the ring buffer.
// Z-Server currently sends about 20 QSO IDs per QSOIDS line, so this can
// absorb roughly 5,000 QSO IDs even if the MAIN loop is temporarily unable
// to drain the queue.  This queue is allocated explicitly from PSRAM.
#define ZMERGE_LINE_QUEUE 256
#define ZMERGE_LINE_SIZE 1024
#define ZMERGE_SEND_INTERVAL_MS 25
#define ZMERGE_REPLY_TIMEOUT_MS 15000

// No-PSRAM merge: keep QSOIDs on microSD instead of in RAM.
#define ZMERGE_SD_RAW "/ZMSRV.ID"
#define ZMERGE_SD_BUCKETS 16
struct __attribute__((packed)) zmerge_sd_id { uint32_t id; uint8_t seen; };
static File zmerge_sd_raw;

// Old zmerge and zmergenew are mutually exclusive.  Share their callback
// receive buffers so zmergenew can use the RAM that was otherwise reserved
// for the old zmerge QSOID/PUTLOGEX handoff.  The total static RAM footprint
// is no larger than before, while the NEW receive queue grows 32 -> 45 slots.
#define ZMERGE_SD_ID_QUEUE 1024  // 4096 bytes; one slot is kept empty
#define ZMERGENEW_MAX_BATCH 10
#define ZMERGENEW_LINE_SIZE 384
#define ZMERGENEW_LINE_QUEUE 45  // 44 usable; shared with old-zmerge buffers
union zmerge_lowmem_shared_arena {
  struct {
    uint32_t idq[ZMERGE_SD_ID_QUEUE];
    char putlogex[NCHR_ZSERVER_CMD + 1];
  } old_merge;
  char new_lines[ZMERGENEW_LINE_QUEUE][ZMERGENEW_LINE_SIZE + 1];
};
static union zmerge_lowmem_shared_arena zmerge_lowmem_arena;
#define zmerge_sd_idq       (zmerge_lowmem_arena.old_merge.idq)
#define zmerge_sd_putlogex  (zmerge_lowmem_arena.old_merge.putlogex)
#define zmergenew_lines     (zmerge_lowmem_arena.new_lines)
static_assert(sizeof(zmerge_lowmem_arena.new_lines) >=
              sizeof(zmerge_lowmem_arena.old_merge),
              "shared zmerge arena too small for old merge buffers");

// AsyncTCP callback -> MAIN handoff for no-PSRAM systems.  Keep the callback
// free of SD/FAT calls: it only parses QSOIDs into this small fixed ring.
#define ZMERGE_SD_EV_BEGIN_OK  0x01
#define ZMERGE_SD_EV_BEGIN_NG  0x02
#define ZMERGE_SD_EV_END_IDS   0x04
#define ZMERGE_SD_EV_PUTLOGEX  0x08
static volatile uint16_t zmerge_sd_idq_r = 0;
static volatile uint16_t zmerge_sd_idq_w = 0;
static volatile uint16_t zmerge_sd_idq_highwater = 0;
static volatile uint8_t zmerge_sd_events = 0;
static volatile bool zmerge_sd_putlogex_ready = false;
static volatile bool zmerge_sd_idq_overflow = false;
static portMUX_TYPE zmerge_sd_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t zmerge_sd_fetch_bucket = 0;
static uint32_t zmerge_sd_fetch_pos = 0;

static void zmerge_sd_bucket_name(uint8_t b, char *dst, size_t n)
{ snprintf(dst, n, "/ZM%X.ID", b & 15); }

static void zmerge_sd_cleanup()
{
  if (zmerge_sd_raw) zmerge_sd_raw.close();
  SD.remove(ZMERGE_SD_RAW);
  char fn[12];
  for (uint8_t b=0; b<ZMERGE_SD_BUCKETS; b++) { zmerge_sd_bucket_name(b, fn, sizeof(fn)); SD.remove(fn); }
  portENTER_CRITICAL(&zmerge_sd_mux);
  zmerge_sd_idq_r = zmerge_sd_idq_w = 0;
  zmerge_sd_idq_highwater = 0;
  zmerge_sd_events = 0;
  zmerge_sd_putlogex[0] = '\0';
  zmerge_sd_putlogex_ready = false;
  zmerge_sd_idq_overflow = false;
  portEXIT_CRITICAL(&zmerge_sd_mux);
  zmerge_sd_fetch_bucket = 0; zmerge_sd_fetch_pos = 0;
}

struct zmerge_context {
  uint8_t phase; // 0 idle, 1 wait lock, 2 receive IDs, 3 scan/upload, 4 fetch server-only
  bool locked;
  bool abort_requested;
  bool dry_run;
  bool repair_mode;
  uint32_t deadline;
  uint32_t next_send;
  uint32_t last_lcd_update;
  uint32_t *server_ids;
  uint8_t *server_seen;
  size_t server_count;
  size_t server_capacity;
  unsigned long total_records;
  unsigned long sent_records;
  unsigned long common_records;
  unsigned long local_only_records;
  unsigned long skipped_no_qsoid;
  unsigned long skipped_legacy;
  unsigned long skipped_deleted;
  unsigned long received_records;
  size_t fetch_index;
  uint32_t requested_qsoid;
  union qso_union_tag pending_record;
  bool pending_valid;
  struct qso_repair_stats repair_stats;
};

static struct zmerge_context zmerge;
static char (*zmerge_lines)[ZMERGE_LINE_SIZE] = nullptr;
static volatile uint8_t zmerge_line_rptr = 0;
static volatile uint8_t zmerge_line_wptr = 0;
static volatile bool zmerge_line_overflow = false;
static portMUX_TYPE zmerge_line_mux = portMUX_INITIALIZER_UNLOCKED;

// zserver_process() runs in a task with limited stack.  Keep the large
// merge work buffers out of the task stack.
static char *zmerge_rx_line = nullptr;
static union qso_union_tag *zmerge_record = nullptr;
static char *zmerge_tx_line = nullptr;


// NEW merge protocol (Z-Server v3.0.1.0, DVPlogger low-memory path).
// Only one CHECKQSOIDS batch is outstanding, so RAM use is bounded by the
// selected batch size (1..10) rather than the complete server QSOID list.
// Receive queue storage is the shared low-memory arena declared above.

struct zmergenew_context {
  uint8_t phase; // 0 idle, 1 scan/send CHECK, 2 wait CHECK reply, 3 wait server-only/NEWENDMERGE
  uint8_t batch_size;
  uint8_t batch_count;
  bool abort_requested;
  uint32_t deadline;
  uint32_t started_ms;
  unsigned long total_records;
  unsigned long valid_records;
  unsigned long common_records;
  unsigned long sent_records;
  unsigned long received_records;
  unsigned long skipped_no_qsoid;
  unsigned long skipped_legacy;
  unsigned long skipped_deleted;
  unsigned long check_rounds;
  uint32_t batch_ids[ZMERGENEW_MAX_BATCH];
  union qso_union_tag batch_records[ZMERGENEW_MAX_BATCH];
};
static struct zmergenew_context zmergenew;
static File zmergenew_scanf;
static volatile uint8_t zmergenew_line_rptr=0, zmergenew_line_wptr=0;
static volatile uint8_t zmergenew_line_highwater=0;
static volatile bool zmergenew_line_overflow=false;
static volatile bool zmergenew_line_too_long=false;
static portMUX_TYPE zmergenew_line_mux = portMUX_INITIALIZER_UNLOCKED;

// Detailed per-line ZMERGENEW wire trace is intentionally separate from
// normal merge progress.  Use verbose bit 4096 only for deep protocol debug;
// ordinary nonzero verbose gives throttled (~100 QSO) progress without
// flooding the AsyncTCP receive path.
#define ZMERGENEW_TRACE_ENABLED() ((verbose & 4096) != 0)

static void zmergenew_queue_line(const char *line)
{
  size_t line_len = strlen(line);
  if (zmergenew.phase != 0 && ZMERGENEW_TRACE_ENABLED())
    plogw->ostream->printf("ZMERGENEW TRACE RXRAW phase=%u len=%u: <%s>\n",
                           (unsigned)zmergenew.phase, (unsigned)line_len, line);
  if (line_len > ZMERGENEW_LINE_SIZE) {
    portENTER_CRITICAL(&zmergenew_line_mux);
    zmergenew_line_too_long = true;
    portEXIT_CRITICAL(&zmergenew_line_mux);
    return;
  }
  portENTER_CRITICAL(&zmergenew_line_mux);
  uint8_t next=(uint8_t)((zmergenew_line_wptr+1)%ZMERGENEW_LINE_QUEUE);
  if (next==zmergenew_line_rptr) zmergenew_line_overflow=true;
  else {
    strlcpy(zmergenew_lines[zmergenew_line_wptr], line,
            sizeof(zmergenew_lines[zmergenew_line_wptr]));
    zmergenew_line_wptr=next;
    uint8_t used = (zmergenew_line_wptr >= zmergenew_line_rptr)
                     ? (uint8_t)(zmergenew_line_wptr - zmergenew_line_rptr)
                     : (uint8_t)(ZMERGENEW_LINE_QUEUE - zmergenew_line_rptr + zmergenew_line_wptr);
    if (used > zmergenew_line_highwater) zmergenew_line_highwater = used;
  }
  portEXIT_CRITICAL(&zmergenew_line_mux);
}

static bool zmergenew_dequeue_line(char *line, size_t n)
{
  bool have=false;
  portENTER_CRITICAL(&zmergenew_line_mux);
  if (zmergenew_line_rptr!=zmergenew_line_wptr) {
    strlcpy(line,zmergenew_lines[zmergenew_line_rptr],n);
    zmergenew_line_rptr=(uint8_t)((zmergenew_line_rptr+1)%ZMERGENEW_LINE_QUEUE);
    have=true;
  }
  portEXIT_CRITICAL(&zmergenew_line_mux);
  return have;
}
// Local QSO scan uses a temporary read-only handle so the persistent
// append handle (qsologf) and its file position are never disturbed.
static File zmerge_scanf;

static void zmerge_reset(bool restore_log_pos);
static const char *zmerge_phase_name(uint8_t phase);
static void zmerge_recover_stale_state(const char *where);
static void zmerge_finish(bool success, const char *reason);
static void zmerge_process_line(const char *line);
static bool zmerge_build_putlog(const union qso_union_tag *rec, uint32_t qsoid,
                                char *buf, size_t buf_size);

bool zserver_send_qso_record(const union qso_union_tag *record, uint32_t qsoid)
{
  if (record == NULL || qsoid == 0) return false;
  char line[768];
  if (!zmerge_build_putlog(record, qsoid, line, sizeof(line))) return false;
  zserver_send(line);
  return true;
}


static bool zmerge_parse_putlogex(const char *line, union qso_union_tag *rec,
                                  uint32_t expected_qsoid);
static bool zmerge_append_pending_record();
static bool zmerge_alloc_work_buffers();
static void zmerge_free_work_buffers();
static void zmergenew_finish(bool success, const char *reason);
static void zmergenew_process_line(char *line);
static void zmergenew_show_progress(const char *stage);

static void zmerge_lcd(const char *line1, const char *line2, uint32_t timeout_ms=2000)
{
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%s\n%s", line1 ? line1 : "", line2 ? line2 : "");
  upd_display_info_flash(dp->lcdbuf);
  info_disp.timer = timeout_ms;
}

static void zmerge_show_progress(bool force=false)
{
  const uint32_t now = millis();
  if (!force && (uint32_t)(now - zmerge.last_lcd_update) < 500) return;
  zmerge.last_lcd_update = now;

  switch (zmerge.phase) {
    case 1:
      zmerge_lcd("ZMERGE", "Waiting for server");
      break;
    case 2:
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ZMERGE IDs\nServer: %u", (unsigned)zmerge.server_count);
      upd_display_info_flash(dp->lcdbuf);
      info_disp.timer = 1500;
      break;
    case 3:
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ZMERGE local: %lu\nUpload: %lu",
               zmerge.total_records, zmerge.sent_records);
      upd_display_info_flash(dp->lcdbuf);
      info_disp.timer = 1500;
      break;
    case 4:
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ZMERGE server\nDownload: %lu/%u",
               zmerge.received_records, (unsigned)zmerge.server_count);
      upd_display_info_flash(dp->lcdbuf);
      info_disp.timer = 1500;
      break;
  }
}

static void zmergenew_show_progress(const char *stage)
{
  const uint32_t elapsed_s = (millis() - zmergenew.started_ms) / 1000UL;
  if (stage && !strcmp(stage, "DOWNLOAD")) {
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "ZMERGENEW DOWNLOAD\nDown:%lu\n%lus",
             zmergenew.received_records, (unsigned long)elapsed_s);
  } else {
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "ZMERGENEW CHECK\nChecked:%lu Common:%lu\nUp:%lu %lus",
             zmergenew.valid_records, zmergenew.common_records,
             zmergenew.sent_records, (unsigned long)elapsed_s);
  }
  upd_display_info_flash(dp->lcdbuf);
  info_disp.timer = 1500;
}

static bool zmerge_alloc_work_buffers()
{
  if (zmerge_lines && zmerge_rx_line && zmerge_record && zmerge_tx_line)
    return true;

  zmerge_lines = (char (*)[ZMERGE_LINE_SIZE])heap_caps_calloc(
      ZMERGE_LINE_QUEUE, ZMERGE_LINE_SIZE,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  zmerge_rx_line = (char *)heap_caps_malloc(
      NCHR_ZSERVER_CMD + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  zmerge_record = (union qso_union_tag *)heap_caps_malloc(
      sizeof(union qso_union_tag), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  zmerge_tx_line = (char *)heap_caps_malloc(
      NCHR_ZSERVER_CMD + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!zmerge_lines || !zmerge_rx_line || !zmerge_record || !zmerge_tx_line) {
    zmerge_free_work_buffers();
    return false;
  }

  memset(zmerge_rx_line, 0, NCHR_ZSERVER_CMD + 1);
  memset(zmerge_record, 0, sizeof(*zmerge_record));
  memset(zmerge_tx_line, 0, NCHR_ZSERVER_CMD + 1);
  return true;
}

static void zmerge_free_work_buffers()
{
  char (*lines)[ZMERGE_LINE_SIZE];
  portENTER_CRITICAL(&zmerge_line_mux);
  lines = zmerge_lines;
  zmerge_lines = nullptr;
  zmerge_line_rptr = zmerge_line_wptr = 0;
  portEXIT_CRITICAL(&zmerge_line_mux);

  if (lines) free(lines);
  if (zmerge_rx_line) free(zmerge_rx_line);
  if (zmerge_record) free(zmerge_record);
  if (zmerge_tx_line) free(zmerge_tx_line);
  zmerge_rx_line = nullptr;
  zmerge_record = nullptr;
  zmerge_tx_line = nullptr;
}

static void zmerge_queue_line(const char *line)
{
  portENTER_CRITICAL(&zmerge_line_mux);
  if (!zmerge_lines) {
    zmerge_line_overflow = true;
    portEXIT_CRITICAL(&zmerge_line_mux);
    return;
  }
  uint8_t next = (uint8_t)((zmerge_line_wptr + 1) % ZMERGE_LINE_QUEUE);
  if (next == zmerge_line_rptr) {
    zmerge_line_overflow = true;
    portEXIT_CRITICAL(&zmerge_line_mux);
    return;
  }
  strlcpy(zmerge_lines[zmerge_line_wptr], line, sizeof(zmerge_lines[0]));
  zmerge_line_wptr = next;
  portEXIT_CRITICAL(&zmerge_line_mux);
}

static bool zmerge_dequeue_line(char *line, size_t line_size)
{
  if (!line) return false;
  portENTER_CRITICAL(&zmerge_line_mux);
  if (!zmerge_lines || zmerge_line_rptr == zmerge_line_wptr) {
    portEXIT_CRITICAL(&zmerge_line_mux);
    return false;
  }
  strlcpy(line, zmerge_lines[zmerge_line_rptr], line_size);
  zmerge_line_rptr = (uint8_t)((zmerge_line_rptr + 1) % ZMERGE_LINE_QUEUE);
  portEXIT_CRITICAL(&zmerge_line_mux);
  return true;
}

static void fixed_field_to_cstr(char *dst, size_t dst_size,
                                const char *src, size_t src_size)
{
  size_t n = src_size;
  while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\r' ||
                   src[n - 1] == '\n' || src[n - 1] == '\0')) n--;
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}


// ZQID:<decimal> is an authoritative server ID. LQID:<decimal> is an ID
// issued by the persistent local allocator. Both are safe for automatic
// CHECK/PUTLOG. Legacy records without either marker are not.
static bool zmerge_qsoid_tag_from_remarks(const char *remarks, const char *tag,
                                           uint32_t *qsoid)
{
  if (remarks == NULL || tag == NULL || qsoid == NULL) return false;
  const char *p = strstr(remarks, tag);
  if (p == NULL) return false;
  p += 5;
  if (*p < '0' || *p > '9') return false;

  char *endp = NULL;
  unsigned long id = strtoul(p, &endp, 10);
  if (endp == p || id == 0 || id > 0xffffffffUL) return false;
  *qsoid = (uint32_t)id;
  return true;
}

static const char *zmerge_operator_memo_from_remarks(char *remarks)
{
  if (remarks == NULL) return "";

  char *memo = remarks;
  if ((!strncmp(remarks, "CQ ", 3) || !strncmp(remarks, "SP ", 3)) &&
      strlen(remarks) >= 7) {
    memo = remarks + 7;
  }

  // Downloaded records have "ZQID:<id> " immediately after the existing
  // local CQ/SP + tx/rnd metadata.  Keep it local; do not send it as memo.
  if (!strncmp(memo, "ZQID:", 5) || !strncmp(memo, "LQID:", 5)) {
    char *space = strchr(memo, ' ');
    memo = space ? space + 1 : memo + strlen(memo);
  }
  return memo;
}

static bool zmerge_qsoid_from_record(const union qso_union_tag *rec,
                                     uint32_t *qsoid, bool *safe_upload)
{
  char remarks[sizeof(rec->entry.remarks) + 1];
  fixed_field_to_cstr(remarks, sizeof(remarks), rec->entry.remarks,
                      sizeof(rec->entry.remarks));
  if (safe_upload) *safe_upload = false;

  if (zmerge_qsoid_tag_from_remarks(remarks, "ZQID:", qsoid) ||
      zmerge_qsoid_tag_from_remarks(remarks, "LQID:", qsoid)) {
    if (safe_upload) *safe_upload = true;
    return true;
  }

  // Legacy fallback is retained only for diagnostics/repair compatibility.
  // Its origin is uncertain because the log seqnr need not equal QSOID ss;
  // callers must never automatically PUTLOG an ID obtained this way.
  char seqbuf[sizeof(rec->entry.seqnr) + 1];
  char run[3] = {0};
  unsigned int tx = 0, rnd = 0;
  fixed_field_to_cstr(seqbuf, sizeof(seqbuf), rec->entry.seqnr,
                      sizeof(rec->entry.seqnr));
  unsigned long seq = strtoul(seqbuf, NULL, 10);
  if (seq == 0) return false;
  if (sscanf(remarks, "%2s %1u%2u", run, &tx, &rnd) != 3) return false;
  if ((strcmp(run, "CQ") != 0 && strcmp(run, "SP") != 0) || tx > 9 || rnd > 99)
    return false;
  *qsoid = (uint32_t)(tx * 100000000UL + seq * 10000UL + rnd * 100UL);
  return true;
}

static int compare_u32(const void *a, const void *b)
{
  uint32_t aa = *(const uint32_t *)a;
  uint32_t bb = *(const uint32_t *)b;
  return (aa > bb) - (aa < bb);
}

static int zmerge_find_server_id(uint32_t id)
{
  if (!f_spiram) {
    char fn[12]; zmerge_sd_bucket_name((uint8_t)(id >> 28), fn, sizeof(fn));
    File f = SD.open(fn, "r+"); // read/write; bucket already exists
    if (!f) return -1;
    struct zmerge_sd_id r; uint32_t pos = 0;
    while (f.read((uint8_t *)&r, sizeof(r)) == (int)sizeof(r)) {
      if (r.id == id) {
        if (!r.seen) { r.seen = 1; f.seek(pos); f.write((uint8_t *)&r, sizeof(r)); f.flush(); }
        f.close(); return 0;
      }
      pos += sizeof(r);
    }
    f.close(); return -1;
  }
  if (zmerge.server_count == 0 || zmerge.server_ids == NULL) return -1;
  uint32_t *p = (uint32_t *)bsearch(&id, zmerge.server_ids, zmerge.server_count,
                                    sizeof(uint32_t), compare_u32);
  return p ? (int)(p - zmerge.server_ids) : -1;
}

static bool zmerge_append_server_id(uint32_t id)
{
  if (id == 0) return true;
  if (zmerge.server_count >= zmerge.server_capacity) {
    size_t new_capacity = zmerge.server_capacity ? zmerge.server_capacity * 2 : 256;
    uint32_t *new_ids = (uint32_t *)heap_caps_malloc(new_capacity * sizeof(uint32_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *new_seen = (uint8_t *)heap_caps_malloc(new_capacity,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_ids) new_ids = (uint32_t *)malloc(new_capacity * sizeof(uint32_t));
    if (!new_seen) new_seen = (uint8_t *)malloc(new_capacity);
    if (!new_ids || !new_seen) {
      if (new_ids) free(new_ids);
      if (new_seen) free(new_seen);
      return false;
    }
    memset(new_seen, 0, new_capacity);
    if (zmerge.server_ids) {
      memcpy(new_ids, zmerge.server_ids, zmerge.server_count * sizeof(uint32_t));
      memcpy(new_seen, zmerge.server_seen, zmerge.server_count);
      free(zmerge.server_ids);
      free(zmerge.server_seen);
    }
    zmerge.server_ids = new_ids;
    zmerge.server_seen = new_seen;
    zmerge.server_capacity = new_capacity;
  }
  zmerge.server_ids[zmerge.server_count] = id;
  zmerge.server_seen[zmerge.server_count] = 0;
  zmerge.server_count++;
  return true;
}

static bool zmerge_sd_partition_server_ids()
{
  if (zmerge_sd_raw) { zmerge_sd_raw.flush(); zmerge_sd_raw.close(); }
  char fn[12];
  for (uint8_t b=0; b<ZMERGE_SD_BUCKETS; b++) {
    zmerge_sd_bucket_name(b, fn, sizeof(fn)); SD.remove(fn);
    File in = SD.open(ZMERGE_SD_RAW, FILE_READ);
    File out = SD.open(fn, "w");
    if (!in || !out) { if(in)in.close(); if(out)out.close(); return false; }
    uint32_t id;
    while (in.read((uint8_t *)&id, sizeof(id)) == (int)sizeof(id)) {
      if ((uint8_t)(id >> 28) == b) {
        struct zmerge_sd_id r = { id, 0 };
        if (out.write((uint8_t *)&r, sizeof(r)) != sizeof(r)) { in.close(); out.close(); return false; }
      }
    }
    out.flush(); out.close(); in.close(); delay(1);
  }
  SD.remove(ZMERGE_SD_RAW);
  return true;
}

static bool zmerge_sd_next_unseen(uint32_t *id)
{
  char fn[12]; struct zmerge_sd_id r;
  while (zmerge_sd_fetch_bucket < ZMERGE_SD_BUCKETS) {
    zmerge_sd_bucket_name(zmerge_sd_fetch_bucket, fn, sizeof(fn));
    File f = SD.open(fn, FILE_READ);
    if (!f) { zmerge_sd_fetch_bucket++; zmerge_sd_fetch_pos=0; continue; }
    if (!f.seek(zmerge_sd_fetch_pos)) { f.close(); return false; }
    while (f.read((uint8_t *)&r, sizeof(r)) == (int)sizeof(r)) {
      zmerge_sd_fetch_pos += sizeof(r);
      if (!r.seen) { *id=r.id; f.close(); return true; }
    }
    f.close(); zmerge_sd_fetch_bucket++; zmerge_sd_fetch_pos=0;
  }
  return false;
}

// Called from the AsyncTCP callback in no-PSRAM mode.  Never touch SD here.
// QSOIDs are parsed into a 512-byte fixed ring and drained by zserver_process().
static void zmerge_sd_callback_line(const char *line)
{
  uint8_t event = 0;
  if (!strcmp(line, "#ZLOG# BEGINMERGE-OK")) event = ZMERGE_SD_EV_BEGIN_OK;
  else if (!strcmp(line, "#ZLOG# BEGINMERGE-NG")) event = ZMERGE_SD_EV_BEGIN_NG;
  else if (!strcmp(line, "#ZLOG# ENDQSOIDS")) event = ZMERGE_SD_EV_END_IDS;
  else if (!strncmp(line, "#ZLOG# PUTLOGEX", 15)) {
    // Preserve the complete reply before zserver.cmdbuf is reused by the next
    // AsyncTCP line.  The previous event-only handoff later parsed cmdbuf,
    // which could already contain a different line and caused false
    // "invalid PUTLOGEX data" failures on HW1.
    portENTER_CRITICAL(&zmerge_sd_mux);
    if (!zmerge_sd_putlogex_ready) {
      strlcpy(zmerge_sd_putlogex, line, sizeof(zmerge_sd_putlogex));
      zmerge_sd_putlogex_ready = true;
      zmerge_sd_events |= ZMERGE_SD_EV_PUTLOGEX;
    }
    portEXIT_CRITICAL(&zmerge_sd_mux);
    return;
  }
  if (event) {
    portENTER_CRITICAL(&zmerge_sd_mux);
    zmerge_sd_events |= event;
    portEXIT_CRITICAL(&zmerge_sd_mux);
    return;
  }

  if (strncmp(line, "#ZLOG# QSOIDS", 13) || zmerge.phase != 2) return;
  const char *p = line + 13;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char *e;
    unsigned long v = strtoul(p, &e, 10);
    if (e == p) break;
    if (v) {
      portENTER_CRITICAL(&zmerge_sd_mux);
      uint16_t next = (uint16_t)((zmerge_sd_idq_w + 1) % ZMERGE_SD_ID_QUEUE);
      if (next == zmerge_sd_idq_r) zmerge_sd_idq_overflow = true;
      else {
        zmerge_sd_idq[zmerge_sd_idq_w] = (uint32_t)v;
        zmerge_sd_idq_w = next;
        uint16_t used = (zmerge_sd_idq_w >= zmerge_sd_idq_r)
                          ? (uint16_t)(zmerge_sd_idq_w - zmerge_sd_idq_r)
                          : (uint16_t)(ZMERGE_SD_ID_QUEUE - zmerge_sd_idq_r + zmerge_sd_idq_w);
        if (used > zmerge_sd_idq_highwater) zmerge_sd_idq_highwater = used;
      }
      portEXIT_CRITICAL(&zmerge_sd_mux);
    }
    p = e;
  }
}

// MAIN task only: drain callback QSOIDs to the SD spool.
static bool zmerge_sd_drain_id_queue()
{
  uint32_t id;
  for (;;) {
    bool have = false;
    portENTER_CRITICAL(&zmerge_sd_mux);
    if (zmerge_sd_idq_r != zmerge_sd_idq_w) {
      id = zmerge_sd_idq[zmerge_sd_idq_r];
      zmerge_sd_idq_r = (uint16_t)((zmerge_sd_idq_r + 1) % ZMERGE_SD_ID_QUEUE);
      have = true;
    }
    portEXIT_CRITICAL(&zmerge_sd_mux);
    if (!have) break;
    if (!zmerge_sd_raw || zmerge_sd_raw.write((uint8_t *)&id, sizeof(id)) != sizeof(id))
      return false;
    zmerge.server_count++;
    if (verbose && (zmerge.server_count % 100UL) == 0)
      plogw->ostream->printf("zmerge: received %u server QSOIDs (queue hwm=%u/%u)\n",
                             (unsigned)zmerge.server_count,
                             (unsigned)zmerge_sd_idq_highwater,
                             (unsigned)(ZMERGE_SD_ID_QUEUE - 1));
    zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
  }
  return true;
}

static double zmerge_tdatetime(const char *tm)
{
  int yy, mo, dd, hh, mm, ss;
  if (sscanf(tm, "%d/%d/%d-%d:%d:%d", &yy, &mo, &dd, &hh, &mm, &ss) != 6)
    return 0.0;
  int year = (yy < 70) ? 2000 + yy : 1900 + yy;
  myDateTime dt(year, mo, dd, hh, mm, ss);
  unsigned long st = dt.unixtime();
  return (double)(st / 86400UL + (45384 - 19815)) +
         (double)(st % 86400UL) / 86400.0;
}

static void append_field(char *buf, size_t buf_size, const char *value, bool tilde=true)
{
  strlcat(buf, value ? value : "", buf_size);
  if (tilde) strlcat(buf, "~", buf_size);
}

static bool zmerge_build_putlog(const union qso_union_tag *rec, uint32_t qsoid,
                                char *buf, size_t buf_size)
{
  char seq[16], tm[32], freq[24], band[12], opmode[16];
  char call[LEN_CALLSIGN + 2], sentrst[8], sentexch[LEN_EXCH + 2];
  char rcvrst[8], rcvexch[LEN_EXCH + 2], remarks[LEN_REMARKS + 2];
  char tmp[64], run[3] = {0};
  int tx = (int)(qsoid / 100000000UL);
  int cq = 0;

  fixed_field_to_cstr(seq, sizeof(seq), rec->entry.seqnr, sizeof(rec->entry.seqnr));
  fixed_field_to_cstr(tm, sizeof(tm), rec->entry.tm, sizeof(rec->entry.tm));
  fixed_field_to_cstr(freq, sizeof(freq), rec->entry.freq, sizeof(rec->entry.freq));
  fixed_field_to_cstr(band, sizeof(band), rec->entry.band, sizeof(rec->entry.band));
  fixed_field_to_cstr(opmode, sizeof(opmode), rec->entry.opmode, sizeof(rec->entry.opmode));
  fixed_field_to_cstr(call, sizeof(call), rec->entry.hiscall, sizeof(rec->entry.hiscall));
  fixed_field_to_cstr(sentrst, sizeof(sentrst), rec->entry.sentrst, sizeof(rec->entry.sentrst));
  fixed_field_to_cstr(sentexch, sizeof(sentexch), rec->entry.sentexch, sizeof(rec->entry.sentexch));
  fixed_field_to_cstr(rcvrst, sizeof(rcvrst), rec->entry.rcvrst, sizeof(rec->entry.rcvrst));
  fixed_field_to_cstr(rcvexch, sizeof(rcvexch), rec->entry.rcvexch, sizeof(rec->entry.rcvexch));
  fixed_field_to_cstr(remarks, sizeof(remarks), rec->entry.remarks, sizeof(rec->entry.remarks));
  if (sscanf(remarks, "%2s", run) == 1 && strcmp(run, "CQ") == 0) cq = 1;
  // The leading "CQ/SP trr" token and optional ZQID:<id> token are
  // DVPlogger-local metadata, not operator memo text sent to zLog.
  const char *memo = zmerge_operator_memo_from_remarks(remarks);

  int bandid = atoi(band);
  int zband = (bandid >= 0 && bandid < (int)(sizeof(zserver_bandid_freqcodes_map)/sizeof(zserver_bandid_freqcodes_map[0])))
                ? zserver_bandid_freqcodes_map[bandid] : 0;
  int zmode = opmode2zLogmode(opmode);
  int pcode = 6, pwr = 50;
  char *pc = power_code(bandid);
  if (pc) {
    if (!strcmp(pc, "P")) { pcode = 2; pwr = 5; }
    else if (!strcmp(pc, "L")) { pcode = 3; pwr = 10; }
    else if (!strcmp(pc, "M")) { pcode = 6; pwr = 50; }
    else if (!strcmp(pc, "H")) { pcode = 10; pwr = 1000; }
  }

  buf[0] = '\0';
  strlcpy(buf, "#ZLOG# PUTLOG ZLOGQSODATA:~", buf_size);
  snprintf(tmp, sizeof(tmp), "%.8f", zmerge_tdatetime(tm)); append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, call);
  append_field(buf, buf_size, sentexch);
  append_field(buf, buf_size, rcvexch);
  append_field(buf, buf_size, sentrst);
  append_field(buf, buf_size, rcvrst);
  append_field(buf, buf_size, seq);
  snprintf(tmp, sizeof(tmp), "%d", zmode); append_field(buf, buf_size, tmp);
  snprintf(tmp, sizeof(tmp), "%d", zband); append_field(buf, buf_size, tmp);
  snprintf(tmp, sizeof(tmp), "%d", pcode); append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, ""); // multi1
  append_field(buf, buf_size, ""); // multi2
  append_field(buf, buf_size, "0");
  append_field(buf, buf_size, "0");
  char qmode[8];
  fixed_field_to_cstr(qmode, sizeof(qmode), rec->entry.mode, sizeof(rec->entry.mode));
  snprintf(tmp, sizeof(tmp), "%d", !strcmp(qmode, "CW") ? plogw->cw_pts : 1);
  append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, plogw->my_name + 2);
  append_field(buf, buf_size, memo);
  snprintf(tmp, sizeof(tmp), "%d", cq); append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, "0"); // dupe is recalculated by zLog
  append_field(buf, buf_size, "0");
  snprintf(tmp, sizeof(tmp), "%d", tx); append_field(buf, buf_size, tmp);
  snprintf(tmp, sizeof(tmp), "%d", pwr); append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, "0");
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)qsoid); append_field(buf, buf_size, tmp);
  append_field(buf, buf_size, freq);
  append_field(buf, buf_size, "0");
  append_field(buf, buf_size, plogw->hostname + 2);
  append_field(buf, buf_size, "0");
  append_field(buf, buf_size, "0");
  append_field(buf, buf_size, "0", false);
  return strlen(buf) < buf_size - 1;
}

static int zmerge_local_bandid(int zband)
{
  for (int i = 1; i < (int)(sizeof(zserver_bandid_freqcodes_map) /
                             sizeof(zserver_bandid_freqcodes_map[0])); i++)
    if (zserver_bandid_freqcodes_map[i] == zband) return i;
  return 1;
}

static void zmerge_set_field(char *dst, size_t dst_size, const char *src)
{
  if (!src) return;
  size_t n = strlen(src);
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static bool zmerge_datetime_to_tm(const char *src, char *dst, size_t dst_size)
{
  double td = atof(src);
  if (td <= 0.0) return false;
  double unix_days = td - (45384.0 - 19815.0);
  if (unix_days < 0.0) return false;
  uint32_t unix_time = (uint32_t)(unix_days * 86400.0 + 0.5);
  DateTime dt(unix_time);
  snprintf(dst, dst_size, "%02d/%02d/%02d-%02d:%02d:%02d",
           dt.year() % 100, dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  return true;
}

static bool zmerge_parse_putlogex(const char *line, union qso_union_tag *rec,
                                  uint32_t expected_qsoid)
{
  const char *p = strstr(line, "ZLOGQSODATA:");
  if (!p) return false;
  p += strlen("ZLOGQSODATA:");
  if (*p == '~') p++;

  char data[NCHR_ZSERVER_CMD + 1];
  strlcpy(data, p, sizeof(data));
  char *field[30] = {0};
  int nf = 0;
  char *q = data;
  while (nf < 30) {
    field[nf++] = q;
    char *sep = strchr(q, '~');
    if (!sep) break;
    *sep = '\0';
    q = sep + 1;
  }
  if (nf < 30) return false;

  uint32_t qsoid = strtoul(field[23], NULL, 10);
  if (qsoid == 0 || (expected_qsoid && qsoid != expected_qsoid)) return false;
  if (atoi(field[29]) != 0) return false;

  // Diagnostic only: zmerge reconstructs the local QSOID later from
  // tx + seqnr + random suffix.  If the seqnr carried in PUTLOGEX field[6]
  // differs from the seqnr encoded in the authoritative QSOID, the same
  // server QSO will look "missing" again on the next zmerge.
  const unsigned long qsoid_seq =
      (unsigned long)((qsoid % 100000000UL) / 10000UL);
  const unsigned long server_seq = strtoul(field[6], NULL, 10);
  if (server_seq != qsoid_seq && (verbose & 4096)) {
    plogw->ostream->printf(
      "ZMERGE DIAG QSOID/SEQ mismatch: qsoid=%lu expected=%lu "
      "server_seq=%lu qsoid_seq=%lu call=%s\n",
      (unsigned long)qsoid,
      (unsigned long)expected_qsoid,
      server_seq, qsoid_seq,
      field[1] ? field[1] : "");
  }

  memset(rec->all, ' ', sizeof(rec->all));
  rec->entry.type[0] = 'Q';
  rec->entry.type[1] = '\0';
  char tm[20];
  if (!zmerge_datetime_to_tm(field[0], tm, sizeof(tm))) return false;
  zmerge_set_field(rec->entry.tm, sizeof(rec->entry.tm), tm);
  zmerge_set_field(rec->entry.hiscall, sizeof(rec->entry.hiscall), field[1]);
  zmerge_set_field(rec->entry.sentexch, sizeof(rec->entry.sentexch), field[2]);
  zmerge_set_field(rec->entry.rcvexch, sizeof(rec->entry.rcvexch), field[3]);
  zmerge_set_field(rec->entry.sentrst, sizeof(rec->entry.sentrst), field[4]);
  zmerge_set_field(rec->entry.rcvrst, sizeof(rec->entry.rcvrst), field[5]);
  zmerge_set_field(rec->entry.seqnr, sizeof(rec->entry.seqnr), field[6]);
  zmerge_set_field(rec->entry.mycall, sizeof(rec->entry.mycall), plogw->my_callsign + 2);

  int zmode = atoi(field[7]);
  int zband = atoi(field[8]);
  int bandid = zmerge_local_bandid(zband);

  // PUTLOGEX normally carries the frequency in Hz in field 25
  // (field[24] after removing the ZLOGQSODATA prefix).  Some zLog records,
  // especially records entered without rig control, leave this field empty.
  // Preserve an explicit frequency when present; otherwise use the nominal
  // frequency represented by the Z-Server band code.  The latter cannot
  // recover the exact operating frequency, but avoids storing an empty or
  // zero frequency in the local QSO record.
  if (field[24][0] && strtoull(field[24], NULL, 10) != 0) {
    zmerge_set_field(rec->entry.freq, sizeof(rec->entry.freq), field[24]);
  } else {
    char freqtmp[20];
    unsigned long long nominal_hz = 0;
    if (zband >= 0 &&
        zband < (int)(sizeof(zserver_freqcodes) / sizeof(zserver_freqcodes[0]))) {
      double mhz = atof(zserver_freqcodes[zband]);
      if (mhz > 0.0) nominal_hz = (unsigned long long)(mhz * 1000000.0 + 0.5);
    }
    if (nominal_hz == 0 && bandid > 0) {
      // Last-resort values for local bands whose Z-Server code is unknown.
      static const unsigned long long local_nominal_hz[] = {
        0ULL, 1900000ULL, 3500000ULL, 7000000ULL, 14000000ULL,
        21000000ULL, 28000000ULL, 50000000ULL, 144000000ULL,
        430000000ULL, 1200000000ULL, 2400000000ULL, 5600000000ULL,
        10000000000ULL, 10000000ULL, 18000000ULL, 24000000ULL
      };
      if (bandid < (int)(sizeof(local_nominal_hz) /
                         sizeof(local_nominal_hz[0])))
        nominal_hz = local_nominal_hz[bandid];
    }
    snprintf(freqtmp, sizeof(freqtmp), "%llu", nominal_hz);
    zmerge_set_field(rec->entry.freq, sizeof(rec->entry.freq), freqtmp);
    if (verbose & 4096)
      plogw->ostream->printf(
          "zmerge: PUTLOGEX QSOID %lu has no frequency; using nominal %llu Hz\n",
          (unsigned long)qsoid, nominal_hz);
  }
  char tmp[20];
  snprintf(tmp, sizeof(tmp), "%d", bandid);
  zmerge_set_field(rec->entry.band, sizeof(rec->entry.band), tmp);
  switch (zmode) {
  case 0:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "CW");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode), "CW");
    break;
  case 1:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "PH");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode),
                     bandid <= 3 ? "LSB" : "USB");
    break;
  case 2:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "PH");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode), "FM");
    break;
  case 3:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "PH");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode), "AM");
    break;
  case 4:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "DG");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode), "RTTY");
    break;
  default:
    zmerge_set_field(rec->entry.mode, sizeof(rec->entry.mode), "DG");
    zmerge_set_field(rec->entry.opmode, sizeof(rec->entry.opmode), "OTHER");
    break;
  }

  unsigned tx = (unsigned)((qsoid / 100000000UL) % 100UL);
  unsigned rnd = (unsigned)((qsoid / 100UL) % 100UL);
  char remarks_meta[48];
  snprintf(remarks_meta, sizeof(remarks_meta), "%s %1u%02u ZQID:%lu ",
           atoi(field[17]) ? "CQ" : "SP",
           tx % 10, rnd, (unsigned long)qsoid);
  zmerge_set_field(rec->entry.remarks, sizeof(rec->entry.remarks), remarks_meta);
  if (field[16][0])
    strlcat(rec->entry.remarks, field[16], sizeof(rec->entry.remarks));
  return true;
}

static bool zmerge_append_pending_record()
{
  if (!zmerge.pending_valid) return false;

  // The QSO module owns the only append handle for QSO.TXT.  Do not open a
  // second File object here even for size verification.
  const size_t record_size = sizeof(zmerge.pending_record.all);
  const size_t nw = qso_log_append_record(&zmerge.pending_record,
                                           QSO_LOG_APPEND_MERGE);

  if (nw != record_size) {
    plogw->ostream->printf("zmerge: append failed: written=%u expected=%u\n",
                           (unsigned)nw, (unsigned)record_size);
    return false;
  }
  zmerge.pending_valid = false;
  zmerge.received_records++;
  return true;
}


static void zmergenew_reset()
{
  close_qso_log_readonly(&zmergenew_scanf);
  memset(&zmergenew,0,sizeof(zmergenew));
  portENTER_CRITICAL(&zmergenew_line_mux);
  zmergenew_line_rptr=zmergenew_line_wptr=0;
  zmergenew_line_highwater=0;
  zmergenew_line_overflow=false;
  zmergenew_line_too_long=false;
  portEXIT_CRITICAL(&zmergenew_line_mux);
}

static void zmergenew_finish(bool success, const char *reason)
{
  // Commit any final merge records before changing merge state or connection.
  qso_log_flush();
  // NEWENDMERGE can already be queued when an overflow happens.  Do not report
  // a clean completion if any receive line was lost/truncated on the way.
  if (success && zmergenew_line_overflow) {
    success = false;
    reason = "receive queue overflow during completion";
  } else if (success && zmergenew_line_too_long) {
    success = false;
    reason = "receive line too long during completion";
  }
  const uint32_t elapsed=millis()-zmergenew.started_ms;
  if (success) {
    plogw->ostream->printf(
      "zmergenew complete: batch=%u elapsed=%lu ms checks=%lu local=%lu valid=%lu common=%lu uploaded=%lu downloaded=%lu noQSOID=%lu legacy=%lu deleted=%lu\n",
      (unsigned)zmergenew.batch_size,(unsigned long)elapsed,zmergenew.check_rounds,
      zmergenew.total_records,zmergenew.valid_records,zmergenew.common_records,
      zmergenew.sent_records,zmergenew.received_records,
      zmergenew.skipped_no_qsoid,zmergenew.skipped_legacy,zmergenew.skipped_deleted);
    snprintf(dp->lcdbuf,sizeof(dp->lcdbuf),
             "ZMERGENEW done\nLocal:%lu Common:%lu\nUp:%lu Down:%lu %lus",
             zmergenew.valid_records, zmergenew.common_records,
             zmergenew.sent_records, zmergenew.received_records,
             (unsigned long)(elapsed/1000));
  } else {
    plogw->ostream->printf("zmergenew failed: %s (batch=%u elapsed=%lu ms checks=%lu)\n",
                           reason?reason:"unknown",(unsigned)zmergenew.batch_size,
                           (unsigned long)elapsed,zmergenew.check_rounds);
    snprintf(dp->lcdbuf,sizeof(dp->lcdbuf),"ZMERGENEW failed\\n%s",reason?reason:"Unknown");
  }
  plogw->ostream->printf("zmergenew queue: capacity=%u usable=%u highwater=%u overflow=%u\n",
                         (unsigned)ZMERGENEW_LINE_QUEUE,
                         (unsigned)(ZMERGENEW_LINE_QUEUE-1),
                         (unsigned)zmergenew_line_highwater,
                         zmergenew_line_overflow ? 1U : 0U);
  upd_display_info_flash(dp->lcdbuf); info_disp.timer=4000;

  // A failed NEW merge can leave the server still streaming PUTLOGEX records.
  // Do not reuse that TCP session for the next NEWBEGINMERGE: close it and let
  // the normal state machine reconnect shortly.  This prevents stale stream
  // data from being mistaken for a CHECKQSOIDS reply in the next run.
  if (!success && zserver_client->connected()) {
    plogw->ostream->println("zmergenew: resetting Z-server connection after failure");
    zserver_suppress_disconnect_notice = true;
    zserver_connected_state = false;
    zserver_client->stop();
    zserver.stat = zserver_auto_enable ? 10 : 11;
    zserver.timeout = zserver_auto_enable ? millis() + 1000UL : 0;
  } else {
    zserver.stat=zserver_client->connected()?4:0;
  }
  zmergenew_reset();

  // NEW merge may have uploaded/downloaded records, so the live DUPE DB and
  // contest statistics must be rebuilt from the synchronized QSO.TXT.  Queue
  // the rebuild after closing the read handle; MAIN runs it incrementally.
  if (success) {
    plogw->ostream->println("zmergenew: MAKEDUPE rebuild queued");
    request_makedupe_rebuild();
  }
}

static int zmergenew_batch_index(uint32_t id)
{
  for (uint8_t i=0;i<zmergenew.batch_count;i++)
    if (zmergenew.batch_ids[i]==id) return i;
  return -1;
}

static void zmergenew_process_line(char *line)
{
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE PARSE phase=%u: <%s>\n",
                         (unsigned)zmergenew.phase, line);
  if (!strncmp(line,"#ZLOG# CHECKQSOIDS",18)) {
    if (zmergenew.phase!=2) {
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE IGNORE CHECKQSOIDS phase=%u expected=2\n", (unsigned)zmergenew.phase);
      return;
    }
    bool missing[ZMERGENEW_MAX_BATCH]={false};
    const char *p=line+18;
    while (*p) {
      while (*p==' ') p++;
      if (!*p) break;
      char *e; unsigned long v=strtoul(p,&e,10);
      if (e==p) break;
      int idx=zmergenew_batch_index((uint32_t)v);
      if (idx<0) { zmergenew_finish(false,"unexpected CHECKQSOIDS reply"); return; }
      missing[idx]=true; p=e;
    }
    for (uint8_t i=0;i<zmergenew.batch_count;i++) {
      if (missing[i]) {
        char tx[NCHR_ZSERVER_CMD+1];
        if (!zmerge_build_putlog(&zmergenew.batch_records[i],zmergenew.batch_ids[i],tx,sizeof(tx))) {
          zmergenew_finish(false,"PUTLOG command too long"); return;
        }
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE TX PUTLOG qsoid=%lu len=%u: <%s>\n",
                               (unsigned long)zmergenew.batch_ids[i], (unsigned)strlen(tx), tx);
        println_tcpserver(zserver_client,tx); zmergenew.sent_records++;
      } else zmergenew.common_records++;
    }
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE CHECK reply done batch=%u uploaded_total=%lu common_total=%lu -> phase=1\n",
                           (unsigned)zmergenew.batch_count, zmergenew.sent_records, zmergenew.common_records);
    if (zmergenew.valid_records &&
        (zmergenew.valid_records % 100UL) < zmergenew.batch_size) {
      plogw->ostream->printf("zmergenew: checked=%lu common=%lu uploaded=%lu (queue hwm=%u/%u)\n",
                             zmergenew.valid_records, zmergenew.common_records,
                             zmergenew.sent_records, (unsigned)zmergenew_line_highwater,
                             (unsigned)(ZMERGENEW_LINE_QUEUE-1));
      zmergenew_show_progress("CHECK");
    }
    zmergenew.batch_count=0;
    zmergenew.phase=1;
    zmergenew.deadline=millis()+120000UL;
    return;
  }
  if (!strncmp(line,"#ZLOG# PUTLOGEX",15)) {
    if (zmergenew.phase!=3) {
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE IGNORE PUTLOGEX phase=%u expected=3\n", (unsigned)zmergenew.phase);
      return;
    }
    union qso_union_tag rec;
    if (!zmerge_parse_putlogex(line,&rec,0)) {
      zmergenew_finish(false,"invalid PUTLOGEX data"); return;
    }
    size_t nw=qso_log_append_record(&rec,QSO_LOG_APPEND_MERGE);
    if (nw!=sizeof(rec.all)) {
      zmergenew_finish(false,"cannot append PUTLOGEX"); return;
    }
    zmergenew.received_records++;
    if ((zmergenew.received_records % 100UL) == 0) {
      plogw->ostream->printf("zmergenew: downloaded=%lu (queue hwm=%u/%u)\n",
                             zmergenew.received_records,
                             (unsigned)zmergenew_line_highwater,
                             (unsigned)(ZMERGENEW_LINE_QUEUE-1));
      zmergenew_show_progress("DOWNLOAD");
    }
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE PUTLOGEX appended downloaded=%lu\n", zmergenew.received_records);
    zmergenew.deadline=millis()+ZMERGE_REPLY_TIMEOUT_MS;
    return;
  }
  if (!strcmp(line,"#ZLOG# NEWENDMERGE")) {
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE RX NEWENDMERGE phase=%u\n", (unsigned)zmergenew.phase);
    if (zmergenew.phase==3) zmergenew_finish(true,NULL);
    return;
  }
}

bool zserver_start_merge_new(uint8_t batch_size)
{
  zmerge_recover_stale_state("zmergenew start");
  if (zmerge.phase!=0 || zmergenew.phase!=0) {
    plogw->ostream->printf(
        "zmergenew: another merge is already running "
        "stat=%d connected=%d zmerge.phase=%u(%s) zmergenew.phase=%u\n",
        zserver.stat, zserver_client->connected() ? 1 : 0,
        (unsigned)zmerge.phase, zmerge_phase_name(zmerge.phase),
        (unsigned)zmergenew.phase);
    return false;
  }
  if (batch_size<1 || batch_size>ZMERGENEW_MAX_BATCH) batch_size=10;
  if (zserver.stat!=4 || !zserver_client->connected()) {
    zmerge_lcd("ZMERGENEW unavailable","Z-Server not linked",4000); return false;
  }
  if (!qso_log_is_open()) {
    zmerge_lcd("ZMERGENEW unavailable","QSO log not open",4000); return false;
  }
  zmergenew_reset();
  if (!open_qso_log_readonly(&zmergenew_scanf)) {
    zmerge_lcd("ZMERGENEW failed","Cannot read QSO log",4000); return false;
  }
  zmergenew.phase=1; zmergenew.batch_size=batch_size;
  zmergenew.started_ms=millis(); zmergenew.deadline=millis()+120000UL;
  zserver.stat=6;
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE START batch=%u stat=%d connected=%d\n",
                         (unsigned)batch_size, zserver.stat, zserver_client->connected()?1:0);
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->println("ZMERGENEW TRACE TX: <#ZLOG# NEWBEGINMERGE>");
  println_tcpserver(zserver_client,"#ZLOG# NEWBEGINMERGE");
  plogw->ostream->printf("zmergenew: started batch=%u\n",(unsigned)batch_size);
  zmerge_lcd("ZMERGENEW", "Starting merge", 1500);
  return true;
}

static void zmerge_reset(bool restore_log_pos)
{
  (void)restore_log_pos;
  close_qso_log_readonly(&zmerge_scanf);
  if (zmerge.server_ids) free(zmerge.server_ids);
  if (zmerge.server_seen) free(zmerge.server_seen);
  zmerge_free_work_buffers();
  if (!f_spiram) zmerge_sd_cleanup();
  memset(&zmerge, 0, sizeof(zmerge));
  zmerge_line_rptr = zmerge_line_wptr = 0;
  zmerge_line_overflow = false;
}

static const char *zmerge_phase_name(uint8_t phase)
{
  switch (phase) {
    case 1: return "wait-begin";
    case 2: return "receive-server-ids";
    case 3: return "scan-local";
    case 4: return "download-server-only";
    default: return "idle";
  }
}

// A live merge always owns zserver.stat 5 (legacy zmerge) or 6 (zmergenew).
// If a disconnect/reset/reconnect leaves only the phase variable behind, the
// wire session is already gone and that phase can only block future merges.
static void zmerge_recover_stale_state(const char *where)
{
  if ((zmerge.phase == 0 && zmergenew.phase == 0) ||
      zserver.stat == 5 || zserver.stat == 6)
    return;

  plogw->ostream->printf(
      "ZMERGE state recover: where=%s stat=%d connected=%d "
      "zmerge.phase=%u(%s) zmergenew.phase=%u -> clearing stale merge state\n",
      where ? where : "?", zserver.stat,
      zserver_client->connected() ? 1 : 0,
      (unsigned)zmerge.phase, zmerge_phase_name(zmerge.phase),
      (unsigned)zmergenew.phase);

  zmerge_reset(false);
  zmergenew_reset();
}

static void zmerge_finish(bool success, const char *reason)
{
  // Commit any final merge records before changing merge state or connection.
  qso_log_flush();
  if (zmerge.locked && zserver_client->connected())
    println_tcpserver(zserver_client, "#ZLOG# ENDMERGE");
  if (!success) {
    plogw->ostream->print("zmerge failed: ");
    plogw->ostream->println(reason ? reason : "unknown error");
  } else {
    unsigned long server_only = (zmerge.server_count > zmerge.common_records)
                                ? zmerge.server_count - zmerge.common_records : 0;
    if (zmerge.repair_mode) {
      plogw->ostream->printf("zmerge repair complete: total=%lu, kept=%lu, removed=%lu, duplicate groups=%lu, groups awaiting restore=%lu, ambiguous server groups=%lu\n",
                             zmerge.repair_stats.total_records,
                             zmerge.repair_stats.kept_records,
                             zmerge.repair_stats.removed_records,
                             zmerge.repair_stats.duplicate_groups,
                             zmerge.repair_stats.groups_restored_by_zmerge,
                             zmerge.repair_stats.ambiguous_server_groups);
      if (zmerge.repair_stats.groups_restored_by_zmerge != 0)
        plogw->ostream->println("zmerge repair: run normal zmerge to restore authoritative server QSOs");
      if (zmerge.repair_stats.ambiguous_server_groups != 0)
        plogw->ostream->println("zmerge repair: ambiguous server duplicates kept unchanged");
    } else if (zmerge.dry_run) {
      plogw->ostream->printf("zmerge dry complete: server IDs=%u, local=%lu, common=%lu, local only=%lu, server only=%lu, no QSOID=%lu, deleted=%lu\n",
                             (unsigned)zmerge.server_count, zmerge.total_records,
                             zmerge.common_records, zmerge.local_only_records,
                             server_only, zmerge.skipped_no_qsoid,
                             zmerge.skipped_deleted);
    } else {
      plogw->ostream->printf("zmerge complete: server IDs=%u, local=%lu, common=%lu, uploaded=%lu, downloaded=%lu, no QSOID=%lu, deleted=%lu\n",
                             (unsigned)zmerge.server_count, zmerge.total_records,
                             zmerge.common_records, zmerge.sent_records,
                             zmerge.received_records, zmerge.skipped_no_qsoid,
                             zmerge.skipped_deleted);
    }
  }
  if (!f_spiram)
    plogw->ostream->printf("zmerge QSOID queue: capacity=%u usable=%u highwater=%u overflow=%u\n",
                           (unsigned)ZMERGE_SD_ID_QUEUE,
                           (unsigned)(ZMERGE_SD_ID_QUEUE-1),
                           (unsigned)zmerge_sd_idq_highwater,
                           zmerge_sd_idq_overflow ? 1U : 0U);
  if (!success) {
    zmerge_lcd("ZMERGE failed", reason ? reason : "Unknown error", 4000);
  } else if (zmerge.repair_mode) {
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "ZMERGE repair done\nRemoved: %lu", zmerge.repair_stats.removed_records);
    upd_display_info_flash(dp->lcdbuf);
    info_disp.timer = 4000;
  } else if (zmerge.dry_run) {
    unsigned long server_only = (zmerge.server_count > zmerge.common_records)
                                ? zmerge.server_count - zmerge.common_records : 0;
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "ZMERGE dry done\nLocal:%lu Server:%lu",
             zmerge.local_only_records, server_only);
    upd_display_info_flash(dp->lcdbuf);
    info_disp.timer = 4000;
  } else {
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "ZMERGE complete\nUp:%lu Down:%lu",
             zmerge.sent_records, zmerge.received_records);
    upd_display_info_flash(dp->lcdbuf);
    info_disp.timer = 4000;
  }

  // Stop the AsyncTCP callback from queueing merge lines before freeing buffers.
  zserver.stat = zserver_client->connected() ? 4 : 0;
  zmerge_reset(true);

  // zmerge can add/remove QSO records.  Rebuild DUPE/statistics only after
  // every merge-owned file handle/buffer has been released.  The rebuild is
  // asynchronous/incremental: request_makedupe_rebuild() only sets a pending
  // flag and the normal MAIN loop performs the actual work safely.
  if (success) {
    plogw->ostream->println("zmerge: MAKEDUPE rebuild queued");
    request_makedupe_rebuild();
  }
}

static void zmerge_process_line(const char *line)
{
  if (!strcmp(line, "#ZLOG# BEGINMERGE-OK")) {
    if (zmerge.phase != 1) return;
    zmerge.locked = true;
    zmerge.phase = 2;
    zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
    println_tcpserver(zserver_client, "#ZLOG# GETQSOIDS");
    plogw->ostream->println("zmerge: lock acquired; requesting QSOIDs");
    return;
  }
  if (!strcmp(line, "#ZLOG# BEGINMERGE-NG")) {
    zmerge_finish(false, "Z-Server is already being merged");
    return;
  }
  if (!strncmp(line, "#ZLOG# QSOIDS", 13)) {
    if (zmerge.phase != 2) return;
    const char *p = line + 13;
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;
      char *endp;
      unsigned long id = strtoul(p, &endp, 10);
      if (endp == p) break;
      if (!zmerge_append_server_id((uint32_t)id)) {
        zmerge_finish(false, "not enough memory for QSOID list");
        return;
      }
      p = endp;
    }
    zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
    return;
  }
  if (!strcmp(line, "#ZLOG# ENDQSOIDS")) {
    if (zmerge.phase != 2) return;
    if (zmerge.server_count > 1) {
      qsort(zmerge.server_ids, zmerge.server_count, sizeof(uint32_t), compare_u32);
      // QSOIDS should be unique, but compact duplicates so summary counts stay sane.
      size_t out = 1;
      for (size_t i = 1; i < zmerge.server_count; i++) {
        if (zmerge.server_ids[i] != zmerge.server_ids[out - 1])
          zmerge.server_ids[out++] = zmerge.server_ids[i];
      }
      zmerge.server_count = out;
    }
    if (zmerge.repair_mode) {
      plogw->ostream->println("zmerge repair: rebuilding QSO log");
      if (!repair_qso_log(zmerge.server_ids, zmerge.server_count,
                          &zmerge.repair_stats)) {
        zmerge_finish(false, "QSO log repair failed");
        return;
      }
      zmerge_finish(true, NULL);
      return;
    }
    close_qso_log_readonly(&zmerge_scanf);
    if (!open_qso_log_readonly(&zmerge_scanf)) {
      zmerge_finish(false, "cannot open QSO log for read");
      return;
    }
    zmerge.phase = 3;
    zmerge.next_send = millis();
    zmerge.deadline = millis() + 120000UL;
    plogw->ostream->printf("zmerge: received %u server QSOIDs; scanning local log\n",
                           (unsigned)zmerge.server_count);
    return;
  }
  if (!strncmp(line, "#ZLOG# PUTLOGEX", strlen("#ZLOG# PUTLOGEX"))) {
    if (zmerge.phase != 4 || zmerge.pending_valid || zmerge.requested_qsoid == 0)
      return;
    if (!zmerge_parse_putlogex(line, &zmerge.pending_record,
                               zmerge.requested_qsoid)) {
      zmerge_finish(false, "invalid PUTLOGEX data");
      return;
    }
    zmerge.pending_valid = true;
    zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
    return;
  }
}

bool zserver_merge_active()
{
  return zmerge.phase != 0 || zmergenew.phase != 0;
}

bool zserver_start_merge(bool dry_run)
{
  zmerge_recover_stale_state("zmerge start");
  if (zmerge.phase != 0 || zmergenew.phase != 0) {
    plogw->ostream->printf(
        "zmerge: another merge is already running "
        "stat=%d connected=%d zmerge.phase=%u(%s) zmergenew.phase=%u\n",
        zserver.stat, zserver_client->connected() ? 1 : 0,
        (unsigned)zmerge.phase, zmerge_phase_name(zmerge.phase),
        (unsigned)zmergenew.phase);
    zmerge_lcd("ZMERGE", "Already running", 3000);
    return false;
  }
  if (zserver.stat != 4 || !zserver_client->connected()) {
    plogw->ostream->println("zmerge: Z-Server is not connected and initialized");
    zmerge_lcd("ZMERGE unavailable", "Z-Server not linked", 4000);
    return false;
  }
  if (!qso_log_is_open()) {
    plogw->ostream->println("zmerge: QSO log is not open");
    zmerge_lcd("ZMERGE unavailable", "QSO log not open", 4000);
    return false;
  }
  zmerge_reset(false);
  if (f_spiram) {
    if (!zmerge_alloc_work_buffers()) {
      plogw->ostream->println("zmerge: cannot allocate work buffers in PSRAM");
      zmerge_lcd("ZMERGE failed", "No work memory", 4000);
      return false;
    }
    plogw->ostream->printf("zmerge: PSRAM work buffers allocated (%u bytes)\n",
                           (unsigned)(ZMERGE_LINE_QUEUE * ZMERGE_LINE_SIZE +
                                      2 * (NCHR_ZSERVER_CMD + 1) +
                                      sizeof(union qso_union_tag)));
  } else {
    zmerge_sd_cleanup();
    zmerge_sd_raw = SD.open(ZMERGE_SD_RAW, FILE_WRITE);
    if (!zmerge_sd_raw) {
      plogw->ostream->println("zmerge: cannot create SD QSOID spool");
      zmerge_lcd("ZMERGE failed", "SD spool error", 4000);
      return false;
    }
    plogw->ostream->println("zmerge: no PSRAM; using low-memory SD-backed merge");
  }
  zmerge.phase = 1;
  zmerge.dry_run = dry_run;
  zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
  zmerge.last_lcd_update = 0;
  zmerge_show_progress(true);
  zserver.stat = 5;
  println_tcpserver(zserver_client, "#ZLOG# BEGINMERGE");
  plogw->ostream->println(dry_run ? "zmerge dry: BEGINMERGE sent" : "zmerge: BEGINMERGE sent");
  return true;
}

bool zserver_start_repair()
{
  if (!f_spiram) {
    plogw->ostream->println("zmerge repair: PSRAM is still required for duplicate repair");
    return false;
  }
  if (!zserver_start_merge(false)) return false;
  zmerge.repair_mode = true;
  plogw->ostream->println("zmerge repair: backup will be QSOBAK.TXT");
  return true;
}
const char *zserver_server_commands[]={"BEGINMERGE","ENDMERGE","GETQSOIDS","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			       "BSDATA","POSTWANTED","DELWANTED","CONNECTCLUSTER",
			       "GETLOGQSOID","SENDRENEW","DELQSO","EXDELQSO","RENEW",
			       "LOCKQSO","UNLOCKQSO","BAND","OPERATOR","FREQ","SPOT",
			       "SENDSPOT","PUTQSO","PUTLOG","EDITQSOTO","INSQSO",
			       "EDITQSOTO","SENDLOG" };

int opmode2zLogmode(char *opmode)
{
  if ((strcmp(opmode,"CW")==0)||(strcmp(opmode,"CW-R")==0)) return 0;
  if ((strcmp(opmode,"LSB")==0)||(strcmp(opmode,"USB")==0)) return 1;
  if ((strcmp(opmode,"FM")==0)) return 2;
  if ((strcmp(opmode,"AM")==0)) return 3;      
  if ((strcmp(opmode,"RTTY")==0)||(strcmp(opmode,"RTTY-R")==0)) return 4;
  // FT4:5 and FT8:6 not supported
  return 7;
}

static bool zserver_is_configured() {
  return plogw != nullptr && plogw->zserver_name[2] != '\0' &&
         zserver_server[0] != '\0';
}

int connect_zserver() {
  // Resolve host[:port] again immediately before connect().  This is kept
  // here as a final guard even when reconnect_zserver() has already parsed
  // the setting, because older/cached zserver_server values may still contain
  // the ":port" suffix.
  char connect_host[sizeof(zserver_server)];
  strncpy(connect_host, zserver_server, sizeof(connect_host) - 1);
  connect_host[sizeof(connect_host) - 1] = '\0';
  int connect_port = zserver_port;

  char *colon = strrchr(connect_host, ':');
  if (colon != NULL && colon[1] != '\0') {
    char *endp = NULL;
    long port = strtol(colon + 1, &endp, 10);
    if (endp != colon + 1 && *endp == '\0' &&
        port >= 1 && port <= 65535) {
      *colon = '\0';
      connect_port = (int)port;
    }
  }

  // An empty Z-server setting explicitly disables the connection.  Check this
  // here as the final guard because connect_zserver() may be called from more
  // than one state-machine path.
  if (!zserver_auto_enable || !zserver_is_configured() || zserver.stat == 11) {
    if (zserver_client->connected()) {
      zserver_suppress_disconnect_notice = true;
      zserver_client->stop();
    }
    zserver.stat = 11;
    return 0;
  }

  if (wifi_status == 1) {
    if (!zserver_client->connected()) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("connecting to zserver ");
	plogw->ostream->print(connect_host);
	plogw->ostream->print(" port:");
	plogw->ostream->println(connect_port);
      }
      memtrace_event("before zserver connect");
      zserver_client->connect(connect_host, connect_port);
      memtrace_event("after zserver connect call");
      return 1;
    } else {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("connect_zserver() : zserver already connected -> ");
	zserver_client->stop();
	plogw->ostream->println("disconnected from zserver.");
      }
      return 0;
    }
  }
  return 0;
}

// received data  from zserver

static size_t zmsg_utf8_decode(const char *s, uint32_t *cp)
{
  const uint8_t c0 = (uint8_t)s[0];
  if (c0 < 0x80) {
    *cp = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0 &&
      ((uint8_t)s[1] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(c0 & 0x1F) << 6) |
          ((uint8_t)s[1] & 0x3F);
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0 &&
      ((uint8_t)s[1] & 0xC0) == 0x80 &&
      ((uint8_t)s[2] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(c0 & 0x0F) << 12) |
          ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) |
          ((uint8_t)s[2] & 0x3F);
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0 &&
      ((uint8_t)s[1] & 0xC0) == 0x80 &&
      ((uint8_t)s[2] & 0xC0) == 0x80 &&
      ((uint8_t)s[3] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(c0 & 0x07) << 18) |
          ((uint32_t)((uint8_t)s[1] & 0x3F) << 12) |
          ((uint32_t)((uint8_t)s[2] & 0x3F) << 6) |
          ((uint8_t)s[3] & 0x3F);
    return 4;
  }

  *cp = c0;
  return 1;
}

static int zmsg_display_width(uint32_t cp)
{
  if (cp < 0x80) return 1;
  if (cp >= 0xFF61 && cp <= 0xFF9F) return 1;
  return 2;
}


static char zmsg_ascii_fallback(uint32_t cp)
{
  // japanese1 fonts omit some punctuation/full-width forms.
  // Keep all other Unicode untouched; only known-problem symbols fall back.
  switch (cp) {
    case 0xFF1F: return '?';  // ？
    case 0xFF01: return '!';  // ！
    case 0x3002: return '.';  // 。
    case 0x3001: return ',';  // 、
    case 0xFF1A: return ':';  // ：
    case 0xFF1B: return ';';  // ；
    case 0xFF08: return '(';  // （
    case 0xFF09: return ')';  // ）
    case 0xFF3B: return '[';  // ［
    case 0xFF3D: return ']';  // ］
    case 0x3000: return ' ';  // ideographic space
    case 0x30FB: return '.';  // ・
    default: return 0;
  }
}

static void zmsg_wrap_utf8_20cols(const char *src, char *dst, size_t dst_size)
{
  static const int MAX_COLS = 20;
  static const int MAX_LINES = 5;

  if (!dst || dst_size == 0) return;

  size_t si = 0;
  size_t di = 0;
  int cols = 0;
  int lines = 1;

  while (src && src[si] != '\0' && di + 1 < dst_size && lines <= MAX_LINES) {
    if (src[si] == '\r') {
      si++;
      continue;
    }
    if (src[si] == '\n') {
      if (lines >= MAX_LINES || di + 1 >= dst_size) break;
      dst[di++] = '\n';
      si++;
      cols = 0;
      lines++;
      continue;
    }

    uint32_t cp = 0;
    size_t clen = zmsg_utf8_decode(src + si, &cp);
    char fallback = zmsg_ascii_fallback(cp);
    int width = fallback ? 1 : zmsg_display_width(cp);

    if (cols > 0 && cols + width > MAX_COLS) {
      if (lines >= MAX_LINES || di + 1 >= dst_size) break;
      dst[di++] = '\n';
      cols = 0;
      lines++;
    }

    if (fallback) {
      if (di + 1 >= dst_size) break;
      dst[di++] = fallback;
    } else {
      if (di + clen >= dst_size) break;
      for (size_t i = 0; i < clen; ++i) dst[di++] = src[si + i];
    }

    si += clen;
    cols += width;
  }

  dst[di] = '\0';
}

void handleData_zserver(void *arg, AsyncClient *client, void *data, size_t len)
{
  if (verbose & 16) {
    plogw->ostream->print("Z:");
    plogw->ostream->write((uint8_t *)data, len);
  }
  if (zserver.stat == 4 || zserver.stat == 5 || zserver.stat == 6) {
    zserver.timeout_alive = millis() + 120000; // not used but timeout counter 
    char c;
    int ret;
    for (int i=0;i<len;i++) {
      c=(char)((uint8_t *)data)[i];
      
      write_ringbuf(&zserver.ringbuf, c);

      ret = readfrom_ringbuf(&zserver.ringbuf, zserver.cmdbuf + zserver.cmdbuf_ptr, (char)0x0d, (char)0x0a, zserver.cmdbuf_len - zserver.cmdbuf_ptr);
      if (ret < 0) {
	// one line read. During zmerge/zmergenew, avoid printing every wire
	// response from the AsyncTCP callback: serial output here can delay queue
	// service and itself contribute to receive overflow.  Merge progress is
	// reported from the MAIN task roughly every 100 QSOs when verbose != 0.
	bool merge_wire_line =
	    !strncmp(zserver.cmdbuf, "#ZLOG# QSOIDS", 13) ||
	    !strcmp(zserver.cmdbuf, "#ZLOG# ENDQSOIDS") ||
	    !strncmp(zserver.cmdbuf, "#ZLOG# PUTLOGEX", 15) ||
	    !strncmp(zserver.cmdbuf, "#ZLOG# CHECKQSOIDS", 18) ||
	    !strcmp(zserver.cmdbuf, "#ZLOG# NEWENDMERGE") ||
	    !strcmp(zserver.cmdbuf, "#ZLOG# BEGINMERGE-OK") ||
	    !strcmp(zserver.cmdbuf, "#ZLOG# BEGINMERGE-NG");
	if (!plogw->f_console_emu && zserver.stat != 5 && zserver.stat != 6 &&
	    !merge_wire_line) {
	  console->print("Z readline:");
	  console->println(zserver.cmdbuf);
	}
	// Keep the AsyncTCP callback short. Merge commands are handled later
	// from zserver_process().
	if (zserver.stat == 5) {
          if (f_spiram) zmerge_queue_line(zserver.cmdbuf);
          else zmerge_sd_callback_line(zserver.cmdbuf);
        } else if (zserver.stat == 6) {
          zmergenew_queue_line(zserver.cmdbuf);
        }
	// check commands
	if (strncmp(zserver.cmdbuf,"#ZLOG# PUTMESSAGE",17)==0) {
	  // Z-Server wire format is CP932. Convert the message payload to
	  // UTF-8 before handing it to the LCD/display path.
	  const char *msg_cp932 = zserver.cmdbuf + 17;
	  while (*msg_cp932 == ' ') msg_cp932++;

	  // Keep these off the callback stack.
	  static char msg_utf8[256];
	  static char msg_wrapped[256];

	  cp932_to_utf8((const uint8_t *)msg_cp932, strlen(msg_cp932),
	                 msg_utf8, sizeof(msg_utf8));

	  // The LCD renderer does not auto-wrap long strings. Insert newlines
	  // at 20 half-width columns. ASCII/half-width kana count as 1 column,
	  // Japanese/full-width characters as 2 columns. Never split UTF-8.
	  zmsg_wrap_utf8_20cols(msg_utf8, msg_wrapped, sizeof(msg_wrapped));

	  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
	           "ZserverMSG:\n%s", msg_wrapped);
	  upd_display_info_flash(dp->lcdbuf);
	}
	  
	*zserver.cmdbuf = '\0';
	zserver.cmdbuf_ptr = 0;
      } else {
	if (ret > 0) {
	  zserver.cmdbuf_ptr += ret;
	  //plogw->ostream->println(ret);
	  if (zserver.cmdbuf_ptr == zserver.cmdbuf_len) {
	    // reached end of cmdbuf
	    if (f_show_zserver >= 0) {
	      if (!plogw->f_console_emu) {
		plogw->ostream->print("OVERFLOW CMDBUF:");
		plogw->ostream->println(zserver.cmdbuf);
	      }
	    }
	    zserver.cmdbuf_ptr = 0;
	  }
	}
      }
    }
  }
}

  
void onDisconnect_zserver(void *arg, AsyncClient *client)
{
  memtrace_event("zserver disconnected");
  //  zserver.stat = 0;
  //  zserver.timeout = millis() + 2000;
  if (zmerge.phase != 0) zmerge.abort_requested = true;
  if (zmergenew.phase != 0) zmergenew.abort_requested = true;
  const bool was_connected = zserver_connected_state;
  zserver_connected_state = false;
  const bool suppress_notice = zserver_suppress_disconnect_notice;
  zserver_suppress_disconnect_notice = false;
  if (!plogw->f_console_emu) {
    plogw->ostream->println("onDisconnect_zserver():disconnected from zserver.");
  }
  if (zserver_auto_enable && zserver_is_configured() && zserver.stat != 11) {
    zserver.stat = 10;
    zserver.timeout = millis() + 60000;
  }
  if (was_connected && !suppress_notice && zserver_auto_enable &&
      WiFi.status() == WL_CONNECTED && zserver_is_configured()) {
    network_display_error("NETWORK ERROR\nZserver lost\nRetry in 60 sec");
  }
}

void onConnect_zserver(void *arg, AsyncClient *client)
{
  memtrace_event("zserver connected");

  // The setting can be cleared while an asynchronous connection attempt is
  // still in progress.  Do not accept that late connection.
  if (!zserver_auto_enable || !zserver_is_configured() || zserver.stat == 11) {
    if (!plogw->f_console_emu) {
      plogw->ostream->println("zserver connected after being disabled; closing.");
    }
    client->stop();
    zserver.stat = 11;
    return;
  }
  if (!plogw->f_console_emu) {
    plogw->ostream->print("connected to zserver ");
    plogw->ostream->print(zserver_server);
    plogw->ostream->print(" port:");
    plogw->ostream->println(zserver_port);
  }

  zserver_connected_state = true;
  sprintf(dp->lcdbuf, "ZSERVER\nConnected\n%s\nPort %d", zserver_server, zserver_port);
  upd_display_info_flash(dp->lcdbuf);

  zserver.stat = 1;
  // A reconnect cannot continue an old merge wire transaction.  Recover any
  // phase left behind by a disconnect/software reset before normal login.
  zmerge_recover_stale_state("zserver reconnect");
  zserver.timeout = millis() + 2000;
  zserver.timeout_alive = millis() + 120000;
}


void init_zserver_info() {
  zserver.ringbuf.buf = zserver_buf;
  zserver.ringbuf.len = NCHR_ZSERVER_RINGBUF;
  zserver.ringbuf.wptr = 0;
  zserver.ringbuf.rptr = 0;
  zserver.timeout = 0;
  zserver.stat = 0;
  zserver.timeout_count=0;
  zserver.cmdbuf_ptr = 0;
  zserver.cmdbuf_len = NCHR_ZSERVER_CMD;
  memset(zserver.cmdbuf, '\0', NCHR_ZSERVER_CMD + 1);

  // Merge state is RAM-only and cannot survive a new Z-Server session.
  // Explicitly reset it here as well as relying on BSS initialization, so a
  // software reset/re-init can never leave a stale phase blocking a new merge.
  zmerge_reset(false);
  zmergenew_reset();

  // define handler
  zserver_client->onData(handleData_zserver, zserver_client);
  zserver_client->onConnect(onConnect_zserver, zserver_client);
  zserver_client->onDisconnect(onDisconnect_zserver, zserver_client);  
  
}

void zserver_process() {
  //  plogw->ostream->println(zserver.stat);
  int ret;
  struct radio *radio;
  radio = so2r.radio_selected();

  switch (zserver.stat) {
  case 0:  // not logged in

    // Empty configuration means disabled.  This also protects the short period
    // during startup before settings have been loaded.
    if (!zserver_auto_enable || !zserver_is_configured()) {
      if (zserver_client->connected()) {
        zserver_suppress_disconnect_notice = true;
        zserver_client->stop();
      }
      zserver_connected_state = false;
      zserver.stat = 11;
      zserver.timeout_count = 0;
      break;
    }
    
    if (!network_external_service_ready(15000)) {
      zserver.stat = 10;
      zserver.timeout = millis() + 1000;
      break;
    } else {
      if (!zserver_client->connected()) {
	connect_zserver(); // try connecting
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("Zserver connection tried. count=");
	  plogw->ostream->println(zserver.timeout_count);
	}
	// AUTO mode retries indefinitely, but only once per minute.
	zserver.stat = 10;
	zserver.timeout_count++;
	zserver.timeout = millis() + 60000;
      } else {
	// already connected but status not updated !?
	if (!plogw->f_console_emu) {    	
	  plogw->ostream->println("zserver already connected but status not updated !?");
	}
	zserver.stat = 0;
      }
    }
    break;
  case 10:
    if (zserver.timeout < millis()) {
      zserver.stat = 0;  // try again
    }
    break;
  case 11:  // after forced disconnection, not try to connect
    break;
  case 1:  // connected and wait for a while to send band
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# BAND %d",zserver_bandid_freqcodes_map[radio->bandid]);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 2;
      zserver.timeout = millis() + 500;
    }
    break;
  case 2:// op name
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# OPERATOR %s",plogw->my_name+2);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 3;
      zserver.timeout = millis() + 500;
    }
    break;
  case 3://pc name
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# PCNAME %s",plogw->hostname+2);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 4;
      zserver.timeout = millis() + 500;
    }
    break;      
  case 4: //
    /// zserver is connected and initialized when 
    if (!zserver_client->connected()) {
      if (verbose & 16) plogw->ostream->println("zserver link is down.");
      zserver_connected_state = false;
      zserver.stat = zserver_auto_enable ? 10 : 11;
      zserver.timeout = zserver_auto_enable ? millis() + 60000 : 0;
    } 
    break;
  case 6: { // NEW low-memory merge protocol
    if (zmergenew.abort_requested || !zserver_client->connected()) {
      zmergenew_finish(false,"connection lost"); break;
    }
    if (zmergenew_line_too_long) { zmergenew_finish(false,"receive line too long"); break; }
    if (zmergenew_line_overflow) { zmergenew_finish(false,"receive queue overflow"); break; }
    char line[ZMERGENEW_LINE_SIZE+1];
    while (zmergenew_dequeue_line(line,sizeof(line))) {
      zmergenew_process_line(line);
      if (zmergenew.phase==0) break;
    }
    if (zmergenew.phase==0) break;
    if ((int32_t)(millis()-zmergenew.deadline)>0) {
      zmergenew_finish(false,"timeout"); break;
    }
    if (zmergenew.phase==1) {
      // Fill one bounded batch. Skip deleted/non-QSO/no-QSOID records.
      while (zmergenew.batch_count<zmergenew.batch_size) {
        union qso_union_tag rec;
        int n=read_qso_log_record(&zmergenew_scanf,&rec);
        if (n==0) break;
        if (n!=sizeof(rec.all)) { zmergenew_finish(false,"short QSO log record"); break; }
        zmergenew.total_records++;
        if (rec.entry.type[0]=='D') { zmergenew.skipped_deleted++; continue; }
        if (rec.entry.type[0]!='Q') { zmergenew.skipped_no_qsoid++; continue; }
        uint32_t id=0; bool safe_upload=false;
        if (!zmerge_qsoid_from_record(&rec,&id,&safe_upload)) { zmergenew.skipped_no_qsoid++; continue; }
        if (!safe_upload) {
          zmergenew.skipped_legacy++;
          continue;
        }
        uint8_t i=zmergenew.batch_count++;
        zmergenew.batch_ids[i]=id;
        memcpy(&zmergenew.batch_records[i],&rec,sizeof(rec));
        zmergenew.valid_records++;
      }
      if (zmergenew.phase==0) break;
      if (zmergenew.batch_count) {
        char req[256]; strlcpy(req,"#ZLOG# CHECKQSOIDS",sizeof(req));
        for (uint8_t i=0;i<zmergenew.batch_count;i++) {
          char idbuf[16]; snprintf(idbuf,sizeof(idbuf)," %lu",(unsigned long)zmergenew.batch_ids[i]);
          strlcat(req,idbuf,sizeof(req));
        }
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->printf("ZMERGENEW TRACE TX CHECK round=%lu count=%u: <%s>\n",
                               zmergenew.check_rounds+1, (unsigned)zmergenew.batch_count, req);
        println_tcpserver(zserver_client,req);
        zmergenew.check_rounds++; zmergenew.phase=2;
        zmergenew.deadline=millis()+ZMERGE_REPLY_TIMEOUT_MS;
      } else {
        close_qso_log_readonly(&zmergenew_scanf);
  if (ZMERGENEW_TRACE_ENABLED())
  plogw->ostream->println("ZMERGENEW TRACE TX: <#ZLOG# GETUNREGQSOS>");
        println_tcpserver(zserver_client,"#ZLOG# GETUNREGQSOS");
        zmergenew.phase=3; zmergenew.deadline=millis()+120000UL;
        plogw->ostream->printf("zmergenew: local scan done; checked=%lu common=%lu uploaded=%lu; requesting server-only QSOs (checks=%lu)\n",
                               zmergenew.valid_records, zmergenew.common_records,
                               zmergenew.sent_records, zmergenew.check_rounds);
        zmergenew_show_progress("DOWNLOAD");
      }
    }
    break;
  }

  case 5: { // merging log with zserver
    zmerge_show_progress(false);
    if (zmerge.abort_requested || !zserver_client->connected()) {
      zmerge_finish(false, "connection lost");
      break;
    }
    if (f_spiram) {
      if (zmerge_line_overflow) { zmerge_finish(false, "receive queue overflow"); break; }
      while (zmerge_dequeue_line(zmerge_rx_line, NCHR_ZSERVER_CMD + 1)) {
        zmerge_process_line(zmerge_rx_line);
        if (zmerge.phase == 0) break;
      }
    } else {
      bool overflow;
      uint8_t ev;
      bool putlogex_ready;
      static char putlogex_line[NCHR_ZSERVER_CMD + 1];
      portENTER_CRITICAL(&zmerge_sd_mux);
      overflow = zmerge_sd_idq_overflow;
      ev = zmerge_sd_events;
      zmerge_sd_events = 0;
      putlogex_ready = zmerge_sd_putlogex_ready;
      if (putlogex_ready) {
        memcpy(putlogex_line, zmerge_sd_putlogex, sizeof(putlogex_line));
        zmerge_sd_putlogex_ready = false;
        zmerge_sd_putlogex[0] = '\0';
      }
      portEXIT_CRITICAL(&zmerge_sd_mux);
      if (overflow) { zmerge_finish(false, "QSOID receive queue overflow"); break; }

      // Drain all queued IDs before honoring ENDQSOIDS, preserving wire order.
      if (!zmerge_sd_drain_id_queue()) { zmerge_finish(false, "SD QSOID spool write failed"); break; }

      if ((ev & ZMERGE_SD_EV_BEGIN_OK) && zmerge.phase==1) {
        zmerge.locked=true; zmerge.phase=2; zmerge.deadline=millis()+ZMERGE_REPLY_TIMEOUT_MS;
        println_tcpserver(zserver_client,"#ZLOG# GETQSOIDS");
        plogw->ostream->printf("zmerge: lock acquired; spooling QSOIDs to SD via %u-byte queue\n",
                               (unsigned)sizeof(zmerge_sd_idq));
      }
      if (ev & ZMERGE_SD_EV_BEGIN_NG) { zmerge_finish(false,"Z-Server is already being merged"); }
      if ((ev & ZMERGE_SD_EV_END_IDS) && zmerge.phase==2) {
        plogw->ostream->printf("zmerge: received %u server QSOIDs; partitioning SD index\n",(unsigned)zmerge.server_count);
        if (!zmerge_sd_partition_server_ids()) { zmerge_finish(false,"cannot build SD QSOID index"); break; }
        close_qso_log_readonly(&zmerge_scanf);
        if (!open_qso_log_readonly(&zmerge_scanf)) { zmerge_finish(false,"cannot open QSO log for read"); break; }
        zmerge.phase=3; zmerge.next_send=millis(); zmerge.deadline=millis()+120000UL;
      }
      if ((ev & ZMERGE_SD_EV_PUTLOGEX) && putlogex_ready &&
          zmerge.phase==4 && !zmerge.pending_valid && zmerge.requested_qsoid) {
        // GETLOGQSOID is request/reply; parse the line copied by the callback,
        // never the shared zserver.cmdbuf which may already have been reused.
        if (!zmerge_parse_putlogex(putlogex_line,&zmerge.pending_record,zmerge.requested_qsoid))
          zmerge_finish(false,"invalid PUTLOGEX data");
        else { zmerge.pending_valid=true; zmerge.deadline=millis()+ZMERGE_REPLY_TIMEOUT_MS; }
      }
    }
    if (zmerge.phase == 0) break;
    if ((int32_t)(millis() - zmerge.deadline) > 0) {
      plogw->ostream->printf(
          "zmerge timeout: phase=%u(%s) server=%u local=%lu common=%lu "
          "uploaded=%lu downloaded=%lu requested=%lu pending=%u\n",
          (unsigned)zmerge.phase, zmerge_phase_name(zmerge.phase),
          (unsigned)zmerge.server_count, zmerge.total_records,
          zmerge.common_records, zmerge.sent_records, zmerge.received_records,
          (unsigned long)zmerge.requested_qsoid,
          zmerge.pending_valid ? 1U : 0U);
      zmerge_finish(false, "timeout");
      break;
    }
    if (zmerge.phase == 3 && (int32_t)(millis() - zmerge.next_send) >= 0) {
      if (!zmerge_scanf) {
        zmerge_finish(false, "QSO scan file is not open");
        break;
      }
      union qso_union_tag *workrec = f_spiram ? zmerge_record : &zmerge.pending_record;
      int n = read_qso_log_record(&zmerge_scanf, workrec);
      if (n == 0) {
        close_qso_log_readonly(&zmerge_scanf);
        if (zmerge.dry_run) {
          zmerge_finish(true, NULL);
        } else {
          zmerge.phase = 4;
          zmerge.fetch_index = 0;
          zmerge.requested_qsoid = 0;
          zmerge.pending_valid = false;
          zmerge.deadline = millis() + 120000UL;
          plogw->ostream->println("zmerge: requesting server-only QSOs");
        }
        break;
      }
      if (n != sizeof(workrec->all)) {
        zmerge_finish(false, "short QSO log record");
        break;
      }
      zmerge.total_records++;
      // A large local log is intentionally scanned incrementally (one record
      // per main-loop pass).  Progress is activity, so refresh the watchdog
      // deadline instead of timing out after a fixed 120 s while still moving.
      zmerge.deadline = millis() + 120000UL;
      if ((zmerge.total_records % 100UL) == 0) {
        const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const size_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
        plogw->ostream->printf(
            "ZMERGE local: %lu common=%lu upload=%lu local-only=%lu "
            "free=%u largest=%u min=%u stack_hwm=%u\n",
            zmerge.total_records, zmerge.common_records, zmerge.sent_records,
            zmerge.local_only_records, (unsigned)free8, (unsigned)largest8,
            (unsigned)min8, (unsigned)stack_hwm);
      }
      if (workrec->entry.type[0] == 'D') {
        zmerge.skipped_deleted++;
      } else if (workrec->entry.type[0] != 'Q') {
        // Ignore unused/corrupt records rather than interpreting them as QSOs.
        zmerge.skipped_no_qsoid++;
      } else {
        uint32_t qsoid; bool safe_upload=false;
        if (!zmerge_qsoid_from_record(workrec, &qsoid, &safe_upload)) {
          zmerge.skipped_no_qsoid++;
        } else if (!safe_upload) {
          zmerge.skipped_legacy++;
        } else {
          int server_index = zmerge_find_server_id(qsoid);
          if (server_index >= 0) {
            if (f_spiram) zmerge.server_seen[server_index] = 1;
            zmerge.common_records++;
          } else {
            zmerge.local_only_records++;
            if (zmerge.dry_run) {
              zmerge.next_send = millis();
              break;
            }
            char *txbuf = f_spiram ? zmerge_tx_line : zserver.cmdbuf;
            if (!zmerge_build_putlog(workrec, qsoid,
                                     txbuf, NCHR_ZSERVER_CMD + 1)) {
              zmerge_finish(false, "PUTLOG command too long");
              break;
            }
            println_tcpserver(zserver_client, txbuf);
            zmerge.sent_records++;
            zmerge.next_send = millis() + ZMERGE_SEND_INTERVAL_MS;
            zmerge.deadline = millis() + 120000UL;
          }
        }
      }
    }
    if (zmerge.phase == 4) {
      if (zmerge.pending_valid) {
        if (!zmerge_append_pending_record()) {
          zmerge_finish(false, "cannot append PUTLOGEX to local log");
          break;
        }
        zmerge.requested_qsoid = 0;
      }
      if (zmerge.requested_qsoid == 0) {
        uint32_t next_id=0; bool have_next=false;
        if (f_spiram) {
          while (zmerge.fetch_index < zmerge.server_count && zmerge.server_seen[zmerge.fetch_index]) zmerge.fetch_index++;
          if (zmerge.fetch_index < zmerge.server_count) { next_id=zmerge.server_ids[zmerge.fetch_index++]; have_next=true; }
        } else {
          have_next=zmerge_sd_next_unseen(&next_id);
        }
        if (!have_next) {
          println_tcpserver(zserver_client, "#ZLOG# SENDRENEW");
          zmerge_finish(true, NULL);
          // Do not rebuild the complete dupe/score database synchronously here.
          // read_qso_log(READQSO_MAKEDUPE) walks the whole log and exchanges bulk
          // data with the sub CPU from the main Arduino task.  Running it directly
          // after a network merge can starve IDLE1 long enough to trip the task
          // watchdog.  The downloaded records are already durable in QSO.txt;
          // rebuild dupe/score state later through the normal startup/manual path.
          if (zmerge.received_records != 0)
            plogw->ostream->println("zmerge: QSO log updated; dupe/score rebuild deferred");
          break;
        }
        zmerge.requested_qsoid = next_id;
        char req[96];
        snprintf(req, sizeof(req), "#ZLOG# GETLOGQSOID %lu",
                 (unsigned long)zmerge.requested_qsoid);
        println_tcpserver(zserver_client, req);
        zmerge.deadline = millis() + ZMERGE_REPLY_TIMEOUT_MS;
      }
    }
    break;
  }
    
  } 
}

void zserver_send(char *buf)
{
  if (zserver.stat==4) {
    // if stat is connected to zserver
    const size_t len = strlen(buf);
    const size_t space = zserver_client->space();
    const bool can_send = zserver_client->canSend();
    if (verbose & 1) {
      plogw->ostream->printf(
          "ZSERVER_SEND before len=%u space=%u canSend=%d connected=%d text=<%s>\n",
          (unsigned)len, (unsigned)space, can_send ? 1 : 0,
          zserver_client->connected() ? 1 : 0, buf);
    }
    const int ret = println_tcpserver(zserver_client,buf);
    if (verbose & 1) {
      plogw->ostream->printf(
          "ZSERVER_SEND after ret=%d space=%u canSend=%d connected=%d\n",
          ret, (unsigned)zserver_client->space(),
          zserver_client->canSend() ? 1 : 0,
          zserver_client->connected() ? 1 : 0);
    }
  } else {
    if (!plogw->f_console_emu) {    
      //      plogw->ostream->println("zserver not linked."); // suppress busy message
    }
  }
}

void reconnect_zserver()
{
  // Stop the old connection before changing the destination.  AsyncTCP may
  // still deliver a late onConnect callback; onConnect_zserver() checks the
  // disabled state as well.
  if (zserver_client->connected()) {
    zserver_suppress_disconnect_notice = true;
    zserver_connected_state = false;
    zserver_client->stop();
  }
  zserver.timeout_count = 0;
  zserver.timeout = 0;
  console->println("reconnect_zserver()");

  // Set the new server name from plogw->zserver_name.  An empty field is an
  // explicit disable request, so also erase the cached destination.
  if (strlen(plogw->zserver_name + 2) != 0) {
    strncpy(zserver_server, plogw->zserver_name + 2,
            sizeof(zserver_server) - 1);
    zserver_server[sizeof(zserver_server) - 1] = '\0';
    zserver.stat = zserver_auto_enable ? 0 : 11;
    sprintf(dp->lcdbuf, "set new zserver name:\n%s\n%s",
            zserver_server, zserver_auto_enable ? "AUTO" : "OFF");
  } else {
    zserver_server[0] = '\0';
    zserver.stat = 11; // do not connect until a server name is entered
    sprintf(dp->lcdbuf, "ZSERVER\nDisabled");
  }
  upd_display_info_flash(dp->lcdbuf);
}

void set_zserver_auto(int enabled)
{
  zserver_auto_enable = enabled ? 1 : 0;
  zserver.timeout_count = 0;
  zserver.timeout = 0;
  if (!zserver_auto_enable) {
    if (zserver_client->connected()) {
      zserver_suppress_disconnect_notice = true;
      zserver_connected_state = false;
      zserver_client->stop();
    }
    zserver.stat = 11;
  } else {
    zserver.stat = zserver_is_configured() ? 0 : 11;
  }
}

int get_zserver_auto() { return zserver_auto_enable ? 1 : 0; }

bool zserver_is_connected()
{
  return zserver_auto_enable && zserver_connected_state &&
         zserver_client && zserver_client->connected();
}

const char *zserver_connection_state()
{
  if (!zserver_auto_enable) return "OFF";
  if (!zserver_is_configured()) return "NOT CONFIGURED";
  if (zserver_is_connected()) return "CONNECTED";
  if (zserver.stat == 10) return "RETRY WAIT";
  return "CONNECTING";
}

void zserver_freq_notification()
{
  struct radio *radio;
  int zlog_ifreq;const char *zlog_freqcode,*zlog_mode;
  radio=so2r.radio_selected() ;
  
  zlog_ifreq=zserver_bandid_freqcodes_map[radio->bandid];
  zlog_freqcode=zserver_freqcodes[zlog_ifreq];
  zlog_mode=zserver_modecodes[opmode2zLogmode(radio->opmode)];
  
  sprintf(buf,"#ZLOG# FREQ %-3d%-5s%-11.1f%-5s%-3s%-10s%s",
	  zlog_ifreq, // band
	  zlog_freqcode, // bandstr
	  (radio->freq/1000.0)*FREQ_UNIT, // freq
	  zlog_mode, // modestr
	  (radio->cq[radio->modetype]==LOG_CQ) ? "CQ":"SP", // cq sp
	  plogw->tm+9, // timestr
	  plogw->hostname+2 //  pc name
	  );
  zserver_send(buf);
}
