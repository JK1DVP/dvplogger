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

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "dupechk.h"
#include "callhist.h"
#include "qso.h"
#include "display.h"
#include "multi_process.h"
#include "multi.h"
#include "log.h"
#include "cw_keying.h"
#include "tcp_server.h"
#include "timekeep.h"

#include "FS.h"
#include "SD.h"
//#include "SdFat.h"
#include "sd_files.h"
#include "SPI.h"
//#include "DS3231.h"
#include "zserver.h"
#include "misc.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "so2r.h"


File qsologf;

// A synchronous SD flush can take long enough to block the keyboard path.
// Commit the record immediately, but defer the media flush briefly so the
// next key/PTT event can be handled first.
static bool qso_log_flush_pending = false;
static uint32_t qso_log_flush_due_ms = 0;
static const uint32_t QSO_LOG_FLUSH_DELAY_MS = 250;            // qso logf

bool qso_log_is_open()
{
  return (bool)qsologf;
}

bool open_qso_log_readonly(File *f)
{
  if (f == NULL) return false;
  if (*f) f->close();
  *f = SD.open(qsologfn, FILE_READ);
  return (bool)(*f);
}

void close_qso_log_readonly(File *f)
{
  if (f != NULL && *f) f->close();
}

int read_qso_log_record(File *f, union qso_union_tag *record)
{
  if (f == NULL || record == NULL || !(*f)) return -1;
  return f->read(record->all, sizeof(record->all));
}

size_t append_qso_log_record(const union qso_union_tag *record,
                             size_t *size_before, size_t *size_after)
{
  if (size_before) *size_before = 0;
  if (size_after) *size_after = 0;
  if (record == NULL || !qsologf) return 0;

  const size_t before = qsologf.size();
  const size_t written = qsologf.write(record->all, sizeof(record->all));
  qsologf.flush();
  const size_t after = qsologf.size();

  if (size_before) *size_before = before;
  if (size_after) *size_after = after;
  return written;
}


struct qso_repair_meta {
  uint32_t hash;
  uint32_t qsoid;
  uint32_t leader;
  uint32_t group_count;
  uint32_t server_count;
  uint8_t qsoid_valid;
  uint8_t server_match;
  uint8_t keep;
  uint8_t reserved;
};

static void qso_repair_fixed_to_cstr(char *dst, size_t dst_size,
                                     const char *src, size_t src_size)
{
  size_t n = src_size;
  while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\r' ||
                   src[n - 1] == '\n' || src[n - 1] == '\0')) n--;
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static bool qso_repair_get_qsoid(const union qso_union_tag *rec, uint32_t *id)
{
  char seqbuf[sizeof(rec->entry.seqnr) + 1];
  char remarks[sizeof(rec->entry.remarks) + 1];
  char run[3] = {0};
  unsigned int tx = 0, rnd = 0;

  qso_repair_fixed_to_cstr(seqbuf, sizeof(seqbuf), rec->entry.seqnr,
                           sizeof(rec->entry.seqnr));
  qso_repair_fixed_to_cstr(remarks, sizeof(remarks), rec->entry.remarks,
                           sizeof(rec->entry.remarks));
  unsigned long seq = strtoul(seqbuf, NULL, 10);
  if (seq == 0) return false;
  if (sscanf(remarks, "%2s %1u%2u", run, &tx, &rnd) != 3) return false;
  if ((strcmp(run, "CQ") != 0 && strcmp(run, "SP") != 0) || tx > 9 || rnd > 99)
    return false;
  *id = (uint32_t)(tx * 100000000UL + seq * 10000UL + rnd * 100UL);
  return true;
}

static bool qso_repair_server_has(const uint32_t *ids, size_t count, uint32_t id)
{
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ids[mid] == id) return true;
    if (ids[mid] < id) lo = mid + 1; else hi = mid;
  }
  return false;
}

static void qso_repair_normalize(union qso_union_tag *dst,
                                 const union qso_union_tag *src)
{
  memcpy(dst->all, src->all, sizeof(dst->all));
  memset(dst->entry.seqnr, 0, sizeof(dst->entry.seqnr));

  char remarks[sizeof(src->entry.remarks) + 1];
  qso_repair_fixed_to_cstr(remarks, sizeof(remarks), src->entry.remarks,
                           sizeof(src->entry.remarks));
  const char *body = remarks;
  char run[3] = {0};
  unsigned int tx = 0, rnd = 0;
  int used = 0;
  if (sscanf(remarks, "%2s %1u%2u %n", run, &tx, &rnd, &used) >= 3 &&
      (strcmp(run, "CQ") == 0 || strcmp(run, "SP") == 0) &&
      tx <= 9 && rnd <= 99) {
    body = remarks + used;
  }
  memset(dst->entry.remarks, 0, sizeof(dst->entry.remarks));
  strlcpy(dst->entry.remarks, body, sizeof(dst->entry.remarks));
}

static uint32_t qso_repair_hash(const union qso_union_tag *rec)
{
  union qso_union_tag normalized;
  qso_repair_normalize(&normalized, rec);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < sizeof(normalized.all); i++) {
    h ^= normalized.all[i];
    h *= 16777619UL;
  }
  return h;
}

static bool qso_repair_same(const union qso_union_tag *a,
                            const union qso_union_tag *b)
{
  union qso_union_tag na, nb;
  qso_repair_normalize(&na, a);
  qso_repair_normalize(&nb, b);
  return memcmp(na.all, nb.all, sizeof(na.all)) == 0;
}

bool repair_qso_log(const uint32_t *server_ids, size_t server_count,
                    struct qso_repair_stats *stats)
{
  if (stats) memset(stats, 0, sizeof(*stats));
  if (!server_ids && server_count != 0) return false;

  File src = SD.open(qsologfn, FILE_READ);
  if (!src) return false;
  const size_t record_size = QSO_RECORD_SIZE;
  const size_t count = src.size() / record_size;
  if (stats) stats->total_records = count;

  struct qso_repair_meta *meta = NULL;
  if (count != 0) {
    meta = (struct qso_repair_meta *)heap_caps_calloc(
        count, sizeof(*meta), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!meta) meta = (struct qso_repair_meta *)calloc(count, sizeof(*meta));
    if (!meta) { src.close(); return false; }
  }

  union qso_union_tag rec, prev;
  bool ok = true;
  for (size_t i = 0; i < count && ok; i++) {
    if (!src.seek(i * record_size) || src.read(rec.all, record_size) != (int)record_size) {
      ok = false;
      break;
    }
    meta[i].hash = qso_repair_hash(&rec);
    meta[i].leader = i;
    meta[i].group_count = 1;
    meta[i].keep = 1;
    meta[i].qsoid_valid = qso_repair_get_qsoid(&rec, &meta[i].qsoid);
    meta[i].server_match = meta[i].qsoid_valid &&
        qso_repair_server_has(server_ids, server_count, meta[i].qsoid);
    meta[i].server_count = meta[i].server_match ? 1 : 0;

    if (rec.entry.type[0] == 'Q') {
      for (size_t j = 0; j < i; j++) {
        if (meta[j].leader != j || meta[j].hash != meta[i].hash) continue;
        if (!src.seek(j * record_size) ||
            src.read(prev.all, record_size) != (int)record_size) {
          ok = false;
          break;
        }
        if (qso_repair_same(&rec, &prev)) {
          meta[i].leader = j;
          meta[j].group_count++;
          if (meta[i].server_match) meta[j].server_count++;
          break;
        }
      }
    }
    if ((i & 15U) == 15U) delay(1);
  }

  if (ok) {
    for (size_t i = 0; i < count; i++) {
      if (meta[i].leader != i || meta[i].group_count <= 1) continue;
      if (stats) stats->duplicate_groups++;
      bool kept_server = false;
      for (size_t j = i; j < count; j++) {
        if (meta[j].leader != i && j != i) continue;
        if (meta[i].server_count != 0) {
          meta[j].keep = meta[j].server_match && !kept_server;
          if (meta[j].keep) kept_server = true;
        } else {
          // Repeated records whose reconstructed IDs are all absent from the
          // server are removed as a group. A following normal zmerge restores
          // one authoritative record with the correct ID.
          meta[j].keep = 0;
        }
      }
      if (meta[i].server_count == 0 && stats) stats->groups_restored_by_zmerge++;
    }
  }

  // FAT short-name compatible repair files (8.3 format).
  const char *tmpname = "/QSOTMP.TXT";
  const char *bakname = "/QSOBAK.TXT";
  SD.remove(tmpname);
  File dst;
  if (ok) {
    dst = SD.open(tmpname, FILE_WRITE);
    if (!dst) ok = false;
  }
  if (ok) {
    for (size_t i = 0; i < count; i++) {
      if (!src.seek(i * record_size) ||
          src.read(rec.all, record_size) != (int)record_size) { ok = false; break; }
      if (meta[i].keep) {
        if (dst.write(rec.all, record_size) != record_size) { ok = false; break; }
        if (stats) stats->kept_records++;
      } else if (stats) {
        stats->removed_records++;
      }
      if ((i & 15U) == 15U) delay(1);
    }
  }
  if (dst) { dst.flush(); dst.close(); }
  src.close();
  if (meta) free(meta);
  if (!ok) { SD.remove(tmpname); return false; }

  close_qsolog();
  SD.remove(bakname);
  if (!SD.rename(qsologfn, bakname)) {
    SD.remove(tmpname);
    open_qsolog();
    return false;
  }
  if (!SD.rename(tmpname, qsologfn)) {
    SD.rename(bakname, qsologfn);
    open_qsolog();
    return false;
  }
  open_qsolog();
  return (bool)qsologf;
}

void init_qsofiles() {
  strcpy(qsologfn, "/qso.txt");
  strcpy(callhistfn, "/callhist.txt");
}

void init_qso() {
  init_qsofiles();
  init_score();
  //  init_dupechk(NMAXQSO,0);
  init_dupechk_maincpu();
  init_callhist();
}


void makedupe_qso_entry() {
  // update dupe list from current qso
  // char *call, byte bandid, byte mask)

  // check
  //plogw->ostream->println("makedupe_qso_entry()");

  if (qso.entry.type[0] != 'Q') return;

  // check if we makedupe this entry or not
  // if Remarks contains C:- entry, not do makedupe
  //      ..             C:name and name is not the same as current contest_name, not do makedupe
  char *p;
  char tmpbuf[200];
  strcpy (tmpbuf,qso.entry.remarks);
  if ((p=parse_strings(tmpbuf,"C:"))!=NULL) {
    if (strcmp(p,"-")==0) {
      // off the contest
      if (verbose&4) {
	console->println("off the contest");
      }
      return ;
    } else {
      if (strcasecmp(p,plogw->contest_name+2)!=0) {
	if (verbose&4) {
	  char buf[100];
	  strcpy(buf,p);
	  console->print(buf);console->print(" not match current contest:");
	  console->println(plogw->contest_name+2);
	}
	return;
      }
    }
  }
  
  // set bandid and modetype according to what is written in the log (needed in dupe checking)
  int bandid, modetype;
  int bandmode;
  bandid = freq2bandid((unsigned long)(atoll(qso.entry.freq)/FREQ_UNIT));
  modetype = 0;
  for (int i = 0; i < 4; i++) {
    if (strncmp(qso.entry.mode, modetype_str[i], 2) == 0) {
      modetype = i;
      break;
    }
  }
  bandmode = bandmode_param(bandid,modetype);

  // update seqnr for the band
  plogw->seqnr_band[bandid-1]++;
  
  //  bandmode = bandid * 4 + modetype;
  //plogw->ostream->print("makedupe3");

  // const char *modetype_str[4] = {"*", "CW", "PH", "DG"};
  // plogw->ostream->println("makedupe_qso_entry()");
  if (dupechk->dupechk_at == 1) {
    // MAKEDUPE bulk mode: do not query and wait for every QSO.
    // The subcpu rejects duplicates and returns score totals at the end.
    entry_makedupe_subcpu_data(qso.entry.hiscall, qso.entry.rcvexch, bandmode);
  } else if (!dupe_check_nocallhist(qso.entry.hiscall, bandmode, plogw->mask)) {
    if (dupechk->ncallsign < dupechk->nmaxqso) {
      entry_dupechk_data(qso.entry.hiscall, qso.entry.rcvexch, bandmode);
      score.worked[modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;
    } else {
      plogw->ostream->println(" dupechk overflow ");
    }
  } else if (verbose & 1) {
    plogw->ostream->print(qso.entry.hiscall);
    plogw->ostream->println(" already in dupechk");
  }

  // The maximum serial number is independent of duplicate elimination.
  if (isdigit(*qso.entry.seqnr)) {
    int seqnr = atoi(qso.entry.seqnr);
    if (seqnr <= 3000 && plogw->seqnr < seqnr) plogw->seqnr = seqnr + 1;
  }


  // multi check and entry multi
  int found;
  found = 0;
  int multi;

  if (*multi_list.multi != NULL) {
    int len;
    char rcvexch[10];
    len = strlen(qso.entry.rcvexch);
    if ((plogw->multi_type == 1) || (plogw->multi_type == 3) || (plogw->multi_type == 4) ) {
      // jarl contest, ACAG , JA8
      len--;
    }
    if (len >= 1) {
      *rcvexch = '\0';

      strncat(rcvexch, qso.entry.rcvexch, len);
      if (verbose & 1) {
        plogw->ostream->print("len=");
        plogw->ostream->print(len);
        plogw->ostream->print(" rcvexch:");
        plogw->ostream->println(rcvexch);
      }
      for (multi = 0; multi < multi_list.n_multi[bandid-1]; multi++) {
        /*	plogw->ostream->print("multi=");
        	plogw->ostream->println(multi);
        	plogw->ostream->print(multi_list.multi->mul[multi]);
        	plogw->ostream->print("<-->");
        	plogw->ostream->print(qso.entry.rcvexch);
        	plogw->ostream->println(":");
        */
        if (strcmp(multi_list.multi[bandid-1]->mul[multi], rcvexch) == 0) {
          // hit
          found = 1;
          break;
        }
      }
    }
    if (found) {
      /*	plogw->ostream->print("bandid=");
      	plogw->ostream->print(bandid);
      	plogw->ostream->print(" multi= ");plogw->ostream->println(multi);
      */
      // valid multiplier, so entry into multi check list for the band

      if (multi_list.multi_worked[bandid - 1][multi] == 0) {
        // new multi found
        score.nmulti[bandid - 1]++;
        if (verbose & 1) {
          plogw->ostream->print("new multi:");
          plogw->ostream->print(qso.entry.rcvexch);
          plogw->ostream->println(" nmulti=");
          plogw->ostream->println(score.nmulti[bandid - 1]);
        }
      }
      multi_list.multi_worked[bandid - 1][multi] = 1;
    }
  }
  //upd_display_info_contest_settings();
}

void reformat_qso_entry(union qso_union_tag *qso) {
  // reformatting the entry
  // time
  string_trim_right(qso->entry.tm, ' ');
  //    plogw->ostream->print(" T:");
  qso->entry.tm[8] = ' ';  // put a space between date and time


  string_trim_right(qso->entry.seqnr, ' ');
  //plogw->ostream->print("seqnr:");plogw->ostream->print(qso.entry.seqnr);plogw->ostream->println(":");

  string_trim_right(qso->entry.hiscall, ' ');
  string_trim_right(qso->entry.mycall, ' ');
  //plogw->ostream->print(" Callsign:");

  string_trim_right(qso->entry.freq, ' ');
  string_trim_right(qso->entry.opmode, ' ');
  string_trim_right(qso->entry.sentrst, ' ');
  // sent rst
  if (strncmp(qso->entry.mode, "PH", 2) == 0) {
    if (strcmp(qso->entry.sentrst, "599") == 0) {
      // phone but RST
      qso->entry.sentrst[2] = '\0';
    }
  }
  string_trim_right(qso->entry.sentexch, ' ');

  string_trim_right(qso->entry.rcvrst, ' ');
  if (strncmp(qso->entry.mode, "PH", 2) == 0) {
    if (strcmp(qso->entry.rcvrst, "599") == 0) {
      // phone but RST --> remove T
      qso->entry.rcvrst[2] = '\0';
    }
  }

  string_trim_right(qso->entry.rcvexch, ' ');
  string_trim_right(qso->entry.remarks, '\n');
}


// read the previous qso and print
void read_qso_log(int option, Stream *out) {
  if (!out) out = console;
  // seek to the first byte and dump

  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;

  // pos = pos - len;  // start from the end record
  pos = 0;  // start from the beginning


  int count;
  count = 0;

  if ((option & READQSO_MAKEDUPE) && dupechk->dupechk_at == 1)
    begin_makedupe_subcpu(plogw->mask);

  while (1) {
    if (!qsologf.seek(pos)) {
      if (!plogw->f_console_emu) out->println("file seek failed");
      goto end;
    }
    //    Serial.print("-");    
    ret = qsologf.read(qso.all, len);
    delay(1);
    //    Serial.print("*");
    if (ret != len) {
      //
      if (!plogw->f_console_emu) {
        out->print("qso not read bytes=");
        out->println(ret);
      }
      goto end;
    } else {
      //      out->print("read ");
      //      out->print(ret);
      //      out->println("bytes");
    } 
    if (option & READQSO_PRINT) {
      sprintf(dp->lcdbuf, "Reading QSO\nRead %d bytes", pos);
      upd_display_info_flash(dp->lcdbuf);
    }
    //    Serial.print("a");
    // check type
    if (qso.entry.type[0] != 'Q') {
      // not vaild qso
      if (!plogw->f_console_emu) {
	out->print("type:");		
	out->print(qso.entry.type[0]);
	out->print(" all:");			
	out->print((char *)(qso.all));	
	out->println(":not valid qso encountered");
      }
      //      goto end; // before finish
      //      skip
      pos = pos + len;
      continue;
    }
    // print content
    //    out->print("Pos:");out->println(pos);
    //    out->println("");

    reformat_qso_entry(&qso);
    //    Serial.print("b");

    // operations

    if (option & READQSO_PRINT) print_qso_entry(&qso, out);
    if (option & READQSO_MAKEDUPE) makedupe_qso_entry();
    esp_task_wdt_reset();  // WDTをリセット

    //    Serial.print("c");
    pos = pos + len;
  }

end:
  if ((option & READQSO_MAKEDUPE) && dupechk->dupechk_at == 1)
    finish_makedupe_subcpu();

  sprintf(dp->lcdbuf, "Reading QSO\nFinished\nPos=%d\n",pos);

  upd_display_info_flash(dp->lcdbuf);
  if (!plogw->f_console_emu) out->println("end of read_qso_log");
  out->print("Pos:");out->println(pos);    
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) out->println("file seek to end failed");
  }
  delay(300);
  upd_display_info_contest_settings(so2r.radio_selected());
  return;
}

int read_qso_log_to_file() {
  // seek to the first byte and dump

  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;


  f = SD.open("/qsomail.txt", FILE_WRITE);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file qsomail.txt for writing");
    return 0;
  }

  // pos = pos - len;  // start from the end record
  pos = 0;  // start from the beginning

  int count;
  count = 0;
  plogw->ostream->println("read qso log to file started");
  while (1) {
    if (!plogw->f_console_emu) plogw->ostream->println(pos);
    if (!qsologf.seek(pos)) {
      if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
      goto end;
    }
    ret = qsologf.read(qso.all, len);
    if (ret != len) {
      //
      if (!plogw->f_console_emu) plogw->ostream->print("qso not read bytes=");
      plogw->ostream->println(ret);
      goto end;
    }

    sprintf(dp->lcdbuf, "Mail QSO Log\nRead %d bytes", pos);
    upd_display_info_flash(dp->lcdbuf);

    // check type
    if (qso.entry.type[0] != 'Q') {
      // not vaild qso
      if (!plogw->f_console_emu) plogw->ostream->println("not valid qso encountered");

      //goto end;
    } else {
      // print content
      //plogw->ostream->print("Pos:");plogw->ostream->print(pos);
      //plogw->ostream->print(" ");

      reformat_qso_entry(&qso);


      // operations

      print_qso_entry_file(&f);
    }
    pos = pos + len;
  }

end:
  f.close();
  if (!plogw->f_console_emu) plogw->ostream->println("end of read_qso_log_to_file");
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
    return 0;
  }
  return 1;
}

// qso entry -> radio qso edit buffers fill
void set_qsodata_from_qso_entry() {
  struct radio *radio;
  radio = so2r.radio_selected();
  if (!plogw->f_console_emu) plogw->ostream->println("set_qsodata_from_qso_entry");
  //  strcpy(radio->tm_loaded,"20");
  strcpy(radio->tm_loaded, qso.entry.tm);
  radio->tm_loaded[8] = '-';  // restore '-' between date and time
  long long tmp;
  sscanf(qso.entry.freq, "%lld", &tmp);
  radio->freq_loaded=tmp/FREQ_UNIT;
  sscanf(qso.entry.seqnr, "%d", &radio->seqnr_loaded);
  strcpy(radio->opmode_loaded, qso.entry.opmode);
  //  strcpy(plogw->my_callsign + 2,qso.entry.mycall); // do not load my_callsign
  strcpy(radio->sent_rst + 2, qso.entry.sentrst);
  radio->sent_rst[1] = strlen(radio->sent_rst + 2);
  // strcpy(plogw->sent_exch + 2,qso.entry.sentexch); // do not load sent_exch
  strcpy(radio->callsign + 2, qso.entry.hiscall);
  radio->callsign[1] = strlen(radio->callsign + 2);
  strcpy(radio->recv_rst + 2, qso.entry.rcvrst);
  radio->recv_rst[1] = strlen(radio->recv_rst + 2);

  strcpy(radio->recv_exch + 2, qso.entry.rcvexch);
  radio->recv_exch[1] = strlen(radio->recv_exch + 2);
  strcpy(radio->remarks + 2, qso.entry.remarks);
  radio->remarks[1] = 0;
}

void print_qso_entry_file(File *f) {
  // print entry
  f->print("20");
  f->write((uint8_t *)qso.entry.tm, strlen(qso.entry.tm));
  f->print(" ");

  // callsign
  f->write((uint8_t *)qso.entry.hiscall, strlen(qso.entry.hiscall));
  f->print(" ");

  // freq
  unsigned long freq;
  freq = atoll(qso.entry.freq)/FREQ_UNIT; // freq in FREQ_UNIT Hz
  char buf[10];
  dtostrf((float)freq / (1000000/FREQ_UNIT), 5, 1, buf);
  f->print(buf);
  f->print(" ");

  // opmode
  f->write((uint8_t *)qso.entry.opmode, strlen(qso.entry.opmode));
  f->print(" ");

  // check rst in phone

  f->write((uint8_t *)qso.entry.sentrst, strlen(qso.entry.sentrst));
  //plogw->ostream->print(" ");

  // sentexch
  f->write((uint8_t *)qso.entry.sentexch, strlen(qso.entry.sentexch));
  f->print(" ");

  // rcv rst
  f->write((uint8_t *)qso.entry.rcvrst, strlen(qso.entry.rcvrst));
  //plogw->ostream->print(" ");

  // rcvexch
  f->write((uint8_t *)qso.entry.rcvexch, strlen(qso.entry.rcvexch));
  f->print(" ");

  // remarks
  f->write((uint8_t *)qso.entry.remarks, strlen(qso.entry.remarks));
  f->println("");
}


void open_qsolog() {
  // open qsolog
  // appendFile(fs::FS &fs, const char * path, const char * message){
  if (!plogw->f_console_emu) plogw->ostream->printf("Appending to file: %s\n", qsologfn);

  qsologf = SD.open(qsologfn, "a+");

  if (!qsologf) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file for appending");
    return;
  }
  //    if(qsologf.print("test item")){
  //        plogw->ostream->println("Message appended");
  //    } else {
  //        plogw->ostream->println("Append failed");
  //    }


  //dump_qso_log();

  //    file.close();
}


void close_qsolog() {
  if (qsologf) {
    if (qso_log_flush_pending) {
      qsologf.flush();
      qso_log_flush_pending = false;
    }
    qsologf.close();
    if (verbose&4) 	    console->println("closed qsolog.");
  }
}



enum qso_file_op_state_t {
  QSO_FILE_OP_IDLE = 0,
  QSO_FILE_OP_DIR_OPEN,
  QSO_FILE_OP_DIR_NEXT,
  QSO_FILE_OP_LIST_PREPARE,
  QSO_FILE_OP_LIST_OPEN,
  QSO_FILE_OP_LIST_SCAN_FIRST,
  QSO_FILE_OP_LIST_SCAN_LAST,
  QSO_FILE_OP_SWITCH_VALIDATE,
  QSO_FILE_OP_SWITCH_CLOSE_CURRENT,
  QSO_FILE_OP_SWITCH_RENAME_CURRENT,
  QSO_FILE_OP_SWITCH_OPEN_COPY,
  QSO_FILE_OP_SWITCH_COPY,
  QSO_FILE_OP_SWITCH_PREPARE_REBUILD,
  QSO_FILE_OP_SWITCH_REBUILD,
  QSO_FILE_OP_SWITCH_FINISH,
  QSO_FILE_OP_ERROR
};

struct qso_file_op_context_t {
  qso_file_op_state_t state;
  int requested_number;
  int scan_number;
  int latest_number;
  int backup_numbers[6];
  int backup_count;
  int backup_index;
  bool directory_for_switch;
  unsigned int directory_entries_seen;
  uint32_t last_progress_ms;
  int saved_number;
  int shown;
  char result_buf[360];
  size_t result_used;
  size_t record_index;
  size_t record_count;
  bool have_first;
  bool have_last;
  bool current_log_saved;
  bool bulk_started;
  char source_name[20];
  char saved_name[20];
  char candidate_name[20];
  char first_tm[18];
  char last_tm[18];
  File file;
  File root;
  File src;
  File dst;
};

static qso_file_op_context_t qso_file_op = { QSO_FILE_OP_IDLE };

static void close_qso_file_op_files() {
  if (qso_file_op.file) qso_file_op.file.close();
  if (qso_file_op.root) qso_file_op.root.close();
  if (qso_file_op.src) qso_file_op.src.close();
  if (qso_file_op.dst) qso_file_op.dst.close();
}

static void reset_qso_file_op_context() {
  close_qso_file_op_files();
  qso_file_op.state = QSO_FILE_OP_IDLE;
  qso_file_op.requested_number = 0;
  qso_file_op.scan_number = 0;
  qso_file_op.latest_number = -1;
  qso_file_op.saved_number = -1;
  for (int i = 0; i < 6; ++i) qso_file_op.backup_numbers[i] = -1;
  qso_file_op.backup_count = 0;
  qso_file_op.backup_index = 0;
  qso_file_op.directory_for_switch = false;
  qso_file_op.directory_entries_seen = 0;
  qso_file_op.last_progress_ms = 0;
  qso_file_op.shown = 0;
  qso_file_op.result_buf[0] = '\0';
  qso_file_op.result_used = 0;
  qso_file_op.record_index = 0;
  qso_file_op.record_count = 0;
  qso_file_op.have_first = false;
  qso_file_op.have_last = false;
  qso_file_op.current_log_saved = false;
  qso_file_op.bulk_started = false;
  qso_file_op.source_name[0] = '\0';
  qso_file_op.saved_name[0] = '\0';
  qso_file_op.candidate_name[0] = '\0';
  qso_file_op.first_tm[0] = '\0';
  qso_file_op.last_tm[0] = '\0';
}

static void finish_qso_file_op() {
  reset_qso_file_op_context();
}

static void fail_qso_file_op(const char *message) {
  close_qso_file_op_files();
  if (qso_file_op.bulk_started) {
    finish_makedupe_subcpu();
    qso_file_op.bulk_started = false;
  }

  if (qso_file_op.current_log_saved) {
    SD.remove(qsologfn);
    SD.rename(qso_file_op.saved_name, qsologfn);
    qso_file_op.current_log_saved = false;
  }
  open_qsolog();
  char display_buf[96];
  snprintf(display_buf, sizeof(display_buf), "QSO file error\n%.40s", message);
  upd_display_info_flash(display_buf);
  reset_qso_file_op_context();
}


static int qsobak_number_from_name(const char *name) {
  if (name == NULL) return -1;
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (strncasecmp(base, "qsobak.", 7) != 0) return -1;
  if (!isdigit((unsigned char)base[7]) ||
      !isdigit((unsigned char)base[8]) ||
      !isdigit((unsigned char)base[9]) || base[10] != '\0') return -1;
  return (base[7] - '0') * 100 + (base[8] - '0') * 10 + (base[9] - '0');
}

static void remember_qsobak_number(int number) {
  if (number < 0 || number > 999) return;
  int pos = 0;
  while (pos < qso_file_op.backup_count &&
         qso_file_op.backup_numbers[pos] > number) pos++;
  if (pos < qso_file_op.backup_count &&
      qso_file_op.backup_numbers[pos] == number) return;
  int limit = qso_file_op.backup_count < 6 ? qso_file_op.backup_count : 5;
  for (int i = limit; i > pos; --i)
    qso_file_op.backup_numbers[i] = qso_file_op.backup_numbers[i - 1];
  if (pos < 6) {
    qso_file_op.backup_numbers[pos] = number;
    if (qso_file_op.backup_count < 6) qso_file_op.backup_count++;
  }
}

static void show_qso_file_op_progress(bool force) {
  uint32_t now = millis();
  if (!force && (uint32_t)(now - qso_file_op.last_progress_ms) < 500) return;
  qso_file_op.last_progress_ms = now;

  char progress[96];
  switch (qso_file_op.state) {
    case QSO_FILE_OP_DIR_OPEN:
      snprintf(progress, sizeof(progress),
               "%s\nOpening SD directory",
               qso_file_op.directory_for_switch ? "SWITCHLOG" : "LISTQSOFILE");
      break;
    case QSO_FILE_OP_DIR_NEXT:
      snprintf(progress, sizeof(progress),
               "%s\nDirectory scan: %u\nQSOBAK found: %d",
               qso_file_op.directory_for_switch ? "SWITCHLOG" : "LISTQSOFILE",
               qso_file_op.directory_entries_seen,
               qso_file_op.directory_for_switch ?
                 (qso_file_op.latest_number >= 0 ? 1 : 0) : qso_file_op.backup_count);
      break;
    case QSO_FILE_OP_LIST_PREPARE:
    case QSO_FILE_OP_LIST_OPEN:
      snprintf(progress, sizeof(progress),
               "LISTQSOFILE\nOpening %03d\n%d/6 displayed",
               qso_file_op.scan_number, qso_file_op.shown);
      break;
    case QSO_FILE_OP_LIST_SCAN_FIRST:
      snprintf(progress, sizeof(progress),
               "LISTQSOFILE %03d\nFirst QSO %u/%u\n%d/6 displayed",
               qso_file_op.scan_number,
               (unsigned int)qso_file_op.record_index,
               (unsigned int)qso_file_op.record_count,
               qso_file_op.shown);
      break;
    case QSO_FILE_OP_LIST_SCAN_LAST:
      snprintf(progress, sizeof(progress),
               "LISTQSOFILE %03d\nLast QSO %u/%u\n%d/6 displayed",
               qso_file_op.scan_number,
               (unsigned int)qso_file_op.record_index,
               (unsigned int)qso_file_op.record_count,
               qso_file_op.shown);
      break;
    case QSO_FILE_OP_SWITCH_VALIDATE:
      snprintf(progress, sizeof(progress), "SWITCHLOG%03d\nChecking source",
               qso_file_op.requested_number);
      break;
    case QSO_FILE_OP_SWITCH_CLOSE_CURRENT:
    case QSO_FILE_OP_SWITCH_RENAME_CURRENT:
      snprintf(progress, sizeof(progress), "SWITCHLOG%03d\nSaving current log",
               qso_file_op.requested_number);
      break;
    case QSO_FILE_OP_SWITCH_OPEN_COPY:
    case QSO_FILE_OP_SWITCH_COPY:
      snprintf(progress, sizeof(progress), "SWITCHLOG%03d\nCopying QSO log",
               qso_file_op.requested_number);
      break;
    case QSO_FILE_OP_SWITCH_PREPARE_REBUILD:
    case QSO_FILE_OP_SWITCH_REBUILD:
      snprintf(progress, sizeof(progress),
               "SWITCHLOG%03d\nRebuild %u/%u",
               qso_file_op.requested_number,
               (unsigned int)qso_file_op.record_index,
               (unsigned int)qso_file_op.record_count);
      break;
    case QSO_FILE_OP_SWITCH_FINISH:
      snprintf(progress, sizeof(progress), "SWITCHLOG%03d\nFinishing",
               qso_file_op.requested_number);
      break;
    default:
      return;
  }
  upd_display_info_flash(progress);
}

static void append_qso_backup_line(int number) {
  char line[32];
  char first_date[9];
  char first_hour[3];
  char last_day[3];
  char last_hour[3];

  memcpy(first_date, qso_file_op.first_tm, 8);
  first_date[8] = '\0';
  memcpy(first_hour, qso_file_op.first_tm + 9, 2);
  first_hour[2] = '\0';
  memcpy(last_day, qso_file_op.last_tm + 6, 2);
  last_day[2] = '\0';
  memcpy(last_hour, qso_file_op.last_tm + 9, 2);
  last_hour[2] = '\0';

  snprintf(line, sizeof(line), "%03d %s %s-%s %s",
           number, first_date, first_hour, last_day, last_hour);
  int nw = snprintf(qso_file_op.result_buf + qso_file_op.result_used,
                    sizeof(qso_file_op.result_buf) - qso_file_op.result_used,
                    "%s%s", qso_file_op.shown ? "\n" : "", line);
  if (nw > 0 &&
      (size_t)nw < sizeof(qso_file_op.result_buf) - qso_file_op.result_used) {
    qso_file_op.result_used += (size_t)nw;
    qso_file_op.shown++;
  }
}


static volatile bool pending_makedupe_rebuild = false;

void request_makedupe_rebuild()
{
  pending_makedupe_rebuild = true;
}

bool qso_file_operation_busy()
{
  return qso_file_op.state != QSO_FILE_OP_IDLE;
}

void process_pending_makedupe_rebuild()
{
  if (!pending_makedupe_rebuild) return;
  if (qso_file_operation_busy() || !qso_log_is_open()) return;

  pending_makedupe_rebuild = false;
  plogw->ostream->printf("Contest changed to %s: rebuilding dupe/multiplier data\n",
                         plogw->contest_name + 2);
  upd_display_info_flash("Contest changed\nRebuilding dupe/multi");
  init_score();
  clear_multi_worked();
  init_dupechk_maincpu();
  reset_dupechk_subcpu();
  read_qso_log(READQSO_MAKEDUPE);
}

void list_qso_backup_files() {
  if (qso_file_op.state != QSO_FILE_OP_IDLE) {
    char display_buf[64];
    snprintf(display_buf, sizeof(display_buf), "QSO file operation\nin progress");
    upd_display_info_flash(display_buf);
    return;
  }

  reset_qso_file_op_context();
  qso_file_op.state = QSO_FILE_OP_DIR_OPEN;
  qso_file_op.directory_for_switch = false;
  qso_file_op.result_buf[0] = '\0';
  qso_file_op.result_used = 0;
  show_qso_file_op_progress(true);
}

bool switch_qso_log(int backup_number) {
  if (qso_file_op.state != QSO_FILE_OP_IDLE) {
    char display_buf[64];
    snprintf(display_buf, sizeof(display_buf), "QSO file operation\nin progress");
    upd_display_info_flash(display_buf);
    return false;
  }
  if (backup_number < 0 || backup_number > 999) return false;

  reset_qso_file_op_context();
  qso_file_op.requested_number = backup_number;
  snprintf(qso_file_op.source_name, sizeof(qso_file_op.source_name),
           "/qsobak.%03d", backup_number);
  qso_file_op.state = QSO_FILE_OP_SWITCH_VALIDATE;
  char display_buf[64];
  snprintf(display_buf, sizeof(display_buf), "SWITCHLOG%03d\nChecking file", backup_number);
  upd_display_info_flash(display_buf);
  show_qso_file_op_progress(true);
  return true;
}

void process_qso_file_operation() {
  union qso_union_tag rec;

  if (qso_log_flush_pending &&
      (int32_t)(millis() - qso_log_flush_due_ms) >= 0) {
    if (qsologf) qsologf.flush();
    qso_log_flush_pending = false;
  }

  if (qso_file_op.state != QSO_FILE_OP_IDLE) show_qso_file_op_progress(false);

  switch (qso_file_op.state) {
    case QSO_FILE_OP_IDLE:
      return;

    case QSO_FILE_OP_DIR_OPEN:
      qso_file_op.root = SD.open("/");
      if (!qso_file_op.root || !qso_file_op.root.isDirectory()) {
        fail_qso_file_op("Cannot open SD root");
        return;
      }
      qso_file_op.state = QSO_FILE_OP_DIR_NEXT;
      return;

    case QSO_FILE_OP_DIR_NEXT: {
      File entry = qso_file_op.root.openNextFile();
      if (!entry) {
        qso_file_op.root.close();
        if (qso_file_op.directory_for_switch) {
          qso_file_op.saved_number = qso_file_op.latest_number + 1;
          if (qso_file_op.saved_number > 999) {
            fail_qso_file_op("QSOBAK number full");
            return;
          }
          snprintf(qso_file_op.saved_name, sizeof(qso_file_op.saved_name),
                   "/qsobak.%03d", qso_file_op.saved_number);
          qso_file_op.state = QSO_FILE_OP_SWITCH_CLOSE_CURRENT;
        } else {
          qso_file_op.backup_index = 0;
          qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
        }
        return;
      }
      qso_file_op.directory_entries_seen++;
      if (!entry.isDirectory() && entry.size() >= QSO_RECORD_SIZE) {
        int number = qsobak_number_from_name(entry.name());
        if (number >= 0) {
          if (number > qso_file_op.latest_number)
            qso_file_op.latest_number = number;
          if (!qso_file_op.directory_for_switch)
            remember_qsobak_number(number);
        }
      }
      entry.close();
      return;
    }

    case QSO_FILE_OP_LIST_PREPARE:
      if (qso_file_op.backup_index >= qso_file_op.backup_count) {
        if (qso_file_op.shown == 0)
          snprintf(qso_file_op.result_buf, sizeof(qso_file_op.result_buf),
                   "No QSO backup files");
        upd_display_info_flash(qso_file_op.result_buf);
        finish_qso_file_op();
        return;
      }
      qso_file_op.scan_number =
          qso_file_op.backup_numbers[qso_file_op.backup_index++];
      snprintf(qso_file_op.candidate_name, sizeof(qso_file_op.candidate_name),
               "/qsobak.%03d", qso_file_op.scan_number);
      qso_file_op.state = QSO_FILE_OP_LIST_OPEN;
      return;

    case QSO_FILE_OP_LIST_OPEN:
      qso_file_op.file = SD.open(qso_file_op.candidate_name, FILE_READ);
      if (!qso_file_op.file || qso_file_op.file.size() < QSO_RECORD_SIZE) {
        if (qso_file_op.file) qso_file_op.file.close();
        qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
        return;
      }
      qso_file_op.record_count = qso_file_op.file.size() / QSO_RECORD_SIZE;
      qso_file_op.record_index = 0;
      qso_file_op.have_first = false;
      qso_file_op.have_last = false;
      qso_file_op.state = QSO_FILE_OP_LIST_SCAN_FIRST;
      return;

    case QSO_FILE_OP_LIST_SCAN_FIRST:
      if (qso_file_op.record_index >= qso_file_op.record_count) {
        qso_file_op.file.close();
        qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
        return;
      }
      if (!qso_file_op.file.seek(qso_file_op.record_index * QSO_RECORD_SIZE) ||
          qso_file_op.file.read(rec.all, QSO_RECORD_SIZE) != QSO_RECORD_SIZE) {
        qso_file_op.file.close();
        qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
        return;
      }
      qso_file_op.record_index++;
      if (rec.entry.type[0] == 'Q') {
        snprintf(qso_file_op.first_tm, sizeof(qso_file_op.first_tm), "%.17s", rec.entry.tm);
        qso_file_op.have_first = true;
        qso_file_op.record_index = qso_file_op.record_count;
        qso_file_op.state = QSO_FILE_OP_LIST_SCAN_LAST;
      }
      return;

    case QSO_FILE_OP_LIST_SCAN_LAST:
      if (qso_file_op.record_index == 0) {
        qso_file_op.file.close();
        qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
        return;
      }
      qso_file_op.record_index--;
      if (!qso_file_op.file.seek(qso_file_op.record_index * QSO_RECORD_SIZE) ||
          qso_file_op.file.read(rec.all, QSO_RECORD_SIZE) != QSO_RECORD_SIZE) return;
      if (rec.entry.type[0] == 'Q') {
        snprintf(qso_file_op.last_tm, sizeof(qso_file_op.last_tm), "%.17s", rec.entry.tm);
        qso_file_op.have_last = true;
        qso_file_op.file.close();
        if (qso_file_op.have_first) append_qso_backup_line(qso_file_op.scan_number);
        qso_file_op.state = QSO_FILE_OP_LIST_PREPARE;
      }
      return;

    case QSO_FILE_OP_SWITCH_VALIDATE:
      qso_file_op.src = SD.open(qso_file_op.source_name, FILE_READ);
      if (!qso_file_op.src || qso_file_op.src.size() == 0) {
        if (qso_file_op.src) qso_file_op.src.close();
        char display_buf[64];
        snprintf(display_buf, sizeof(display_buf),
                 "SWITCHLOG%03d\nFile not found/empty",
                 qso_file_op.requested_number);
        upd_display_info_flash(display_buf);
        finish_qso_file_op();
        return;
      }
      qso_file_op.src.close();
      qso_file_op.latest_number = -1;
      qso_file_op.directory_for_switch = true;
      qso_file_op.state = QSO_FILE_OP_DIR_OPEN;
      return;

    case QSO_FILE_OP_SWITCH_CLOSE_CURRENT:
      close_qsolog();
      qso_file_op.state = QSO_FILE_OP_SWITCH_RENAME_CURRENT;
      return;

    case QSO_FILE_OP_SWITCH_RENAME_CURRENT:
      if (SD.exists(qsologfn)) {
        if (!SD.rename(qsologfn, qso_file_op.saved_name)) {
          fail_qso_file_op("Cannot save QSO.TXT");
          return;
        }
        qso_file_op.current_log_saved = true;
      }
      qso_file_op.state = QSO_FILE_OP_SWITCH_OPEN_COPY;
      return;

    case QSO_FILE_OP_SWITCH_OPEN_COPY:
      qso_file_op.src = SD.open(qso_file_op.source_name, FILE_READ);
      if (!qso_file_op.src) {
        fail_qso_file_op("Cannot open source");
        return;
      }
      SD.remove(qsologfn);
      qso_file_op.dst = SD.open(qsologfn, FILE_WRITE);
      if (!qso_file_op.dst) {
        fail_qso_file_op("Cannot create QSO.TXT");
        return;
      }
      qso_file_op.state = QSO_FILE_OP_SWITCH_COPY;
      return;

    case QSO_FILE_OP_SWITCH_COPY: {
      uint8_t buf[256];
      if (!qso_file_op.src.available()) {
        qso_file_op.dst.flush();
        qso_file_op.dst.close();
        qso_file_op.src.close();
        qso_file_op.state = QSO_FILE_OP_SWITCH_PREPARE_REBUILD;
        return;
      }
      size_t nr = qso_file_op.src.read(buf, sizeof(buf));
      if (nr == 0 || qso_file_op.dst.write(buf, nr) != nr) {
        fail_qso_file_op("Copy error");
      }
      return;
    }

    case QSO_FILE_OP_SWITCH_PREPARE_REBUILD:
      init_dupechk_maincpu();
      reset_dupechk_subcpu();
      init_score();
      clear_multi_worked();
      plogw->seqnr = 0;
      memset(plogw->seqnr_band, 0, sizeof(plogw->seqnr_band));
      if (dupechk->dupechk_at == 1) {
        begin_makedupe_subcpu(plogw->mask);
        qso_file_op.bulk_started = true;
      }
      qso_file_op.file = SD.open(qsologfn, FILE_READ);
      if (!qso_file_op.file) {
        fail_qso_file_op("Cannot rebuild log");
        return;
      }
      qso_file_op.record_count = qso_file_op.file.size() / QSO_RECORD_SIZE;
      qso_file_op.record_index = 0;
      qso_file_op.state = QSO_FILE_OP_SWITCH_REBUILD;
      return;

    case QSO_FILE_OP_SWITCH_REBUILD:
      if (qso_file_op.record_index >= qso_file_op.record_count) {
        qso_file_op.file.close();
        if (qso_file_op.bulk_started) {
          finish_makedupe_subcpu();
          qso_file_op.bulk_started = false;
        }
        qso_file_op.state = QSO_FILE_OP_SWITCH_FINISH;
        return;
      }
      if (!qso_file_op.file.seek(qso_file_op.record_index * QSO_RECORD_SIZE) ||
          qso_file_op.file.read(qso.all, QSO_RECORD_SIZE) != QSO_RECORD_SIZE) {
        fail_qso_file_op("Read error rebuilding");
        return;
      }
      qso_file_op.record_index++;
      if (qso.entry.type[0] == 'Q') {
        reformat_qso_entry(&qso);
        makedupe_qso_entry();
      }
      return;

    case QSO_FILE_OP_SWITCH_FINISH:
      open_qsolog();
      qso_file_op.current_log_saved = false;
      char display_buf[96];
      snprintf(display_buf, sizeof(display_buf),
               "SWITCHLOG%03d\nCurrent -> %03d\nQSO.TXT opened",
               qso_file_op.requested_number, qso_file_op.saved_number);
      upd_display_info_flash(display_buf);
      finish_qso_file_op();
      return;

    case QSO_FILE_OP_ERROR:
    default:
      fail_qso_file_op("Invalid state");
      return;
  }
}

void create_new_qso_log() {
  if (!plogw->f_console_emu) plogw->ostream->println("Creating new QSO logfile");

  // create backup of current QSO log
  qsologf.close();

  char fname[30];
  int n;
  n = 0;
  while (1) {
    sprintf(fname, "/qsobak.%03d", n);
    if (SD.exists(fname)) {
      n++;
      continue;
    }
    if (SD.rename(qsologfn, fname) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("Rename QSO logfile failed");
    }
    if (!plogw->f_console_emu) {
      plogw->ostream->print(qsologfn);
      plogw->ostream->print(" -> ");
      plogw->ostream->println(fname);
    }
    break;
  }

  open_qsolog();

  // reset  dupe check
  //  init_dupechk(NMAXQSO,0);
  init_dupechk_maincpu();
  reset_dupechk_subcpu();
  init_score();
  //

  plogw->seqnr = 0;
  new_log_entry();
}



void sprint_qso_entry(char *buf,union qso_union_tag *qso) {
  // print entry
  if (plogw->f_console_emu) return;

  //  Serial.print("d");
  *buf='\0';
  
  strcat(buf,"20");
  strcat(buf,qso->entry.tm);
  strcat(buf," ");

  // callsign
  strcat(buf,qso->entry.hiscall);
  strcat(buf," ");

  // freq
  unsigned long freq;
  freq = atoll(qso->entry.freq)/FREQ_UNIT;
  char bufa[10];
  dtostrf((double)freq / (1000000/FREQ_UNIT), 5, 1, bufa);
  strcat(buf,bufa);
  strcat(buf," ");

  // opmode
  strcat(buf,qso->entry.opmode);
  strcat(buf," ");

  // check rst in phone

  strcat(buf,qso->entry.sentrst);
  //plogw->ostream->print(" ");

  // sentexch
  strcat(buf,qso->entry.sentexch);
  strcat(buf," ");

  // rcv rst
  strcat(buf,qso->entry.rcvrst);
  //plogw->ostream->print(" ");

  // rcvexch
  strcat(buf,qso->entry.rcvexch);
  strcat(buf," ");

  // remarks
  strcat(buf,qso->entry.remarks);
  strcat(buf,"\r\n");
  //  Serial.print("e");
  //  plogw->ostream->println(buf);
  
}
void print_qso_entry(union qso_union_tag *qso, Stream *out) {
  if (!out) out = console;
  sprint_qso_entry(buf,qso);
  //  Serial.print("f");
  out->write(buf,strlen(buf));
  //  Serial.print("g");
}


// qso->entry.tm の形式をstruct tm に変換
//"%02d/%02d/%02d-%02d:%02d:%02d",
struct tm parse_datetime(const char *datetime_str) {
    struct tm tm_result = {0};
    int year, month, day, hour, min, sec;
    /*    console->print("datetime_str:");console->print(datetime_str);*/
    if (sscanf(datetime_str, "%2d/%2d/%2d %2d:%2d:%2d",
               &year, &month, &day, &hour, &min, &sec) == 6) {
        // 年は2000年以降と仮定（25 -> 2025）
        tm_result.tm_year = year + 100;
        tm_result.tm_mon  = month - 1;   // 0〜11
        tm_result.tm_mday = day;
        tm_result.tm_hour = hour;
        tm_result.tm_min  = min;
        tm_result.tm_sec  = sec;

	/*	console->print(" yr:");console->print(year);
	console->print(" mn:");console->print(month);
	console->print(" day:");console->print(day);
	console->print(" hr:");console->print(hour);
	console->print(" min:");console->print(min);
	console->print(" sec:");console->println(sec);*/
    } else {
      //      console->print("parse_datetime: 文字列の解析に失敗しました\n");
    }

    return tm_result;
}

int strcpy_to_chr(char *dest, char *src,char c) {
    const char *colon = strchr(src, c);
    size_t len = 0;

    if (colon != NULL) {
        len = colon - src;  // ':'までの長さ
    } else {
        len = strlen(src);  // ':'がなければ全文字列
    }

    strncpy(dest, src, len);
    dest[len] = '\0';  // null終端
    return len;
}

char *parse_strings(const char *remarks,char *parse_str) {
  char *p;
  char tmpbuf1[100];
  if ((p=strstr(remarks,parse_str))!=NULL) { // my park information in POTA activation
    strcpy(tmpbuf1,p+strlen(parse_str));
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      return p;
    }
  }
  return NULL;
}


void sprint_qso_entry_hamlogcsv(char *buf,union qso_union_tag *qso) {
  char tmpbuf[200];
  int len;
  //1.  No                : 1
  //  sprintf(tmpbuf,"%s,",qso->entry.seqnr);
  //  strcat(buf,tmpbuf);

  //4.  相手局コールサイン     : JA1ZLO
  sprintf(tmpbuf,"%s,",qso->entry.hiscall);
  strcat(buf,tmpbuf);
  
  //2.  交信日             : 2025/06/15
  //3.  交信時刻           : 1330
  //<QSO_DATE:8>20230611

  struct tm jst_tm;  
  jst_tm=parse_datetime(qso->entry.tm);
  // convert to gmt
  time_t jst_time = mktime(&jst_tm);
  //  time_t utc_time = jst_time - 9 * 3600;
  //  console->println(jst_time);
  //  console->println(utc_time);
  //  struct tm utc_tm = *localtime(&utc_time);  // UTCに変換
  struct tm utc_tm = *localtime(&jst_time);  // UTCに変換  
  sprintf(tmpbuf,"%04d/%02d/%02d,",
	  utc_tm.tm_year+1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);
  strcat(buf,tmpbuf);
  //<TIME_ON:4>0016  
  sprintf(tmpbuf,"%02d:%02dJ,",utc_tm.tm_hour, utc_tm.tm_min);
  strcat(buf,tmpbuf);

  //5.  送信RST           : 599
  //<RST_SENT:2>59
  sprintf(tmpbuf,"%s,",qso->entry.sentrst);
  strcat(buf,tmpbuf);
  
  //6.  受信RST           : 599
  //<RST_RCVD:2>59
  sprintf(tmpbuf,"%s,",qso->entry.rcvrst);
  strcat(buf,tmpbuf);
  //7.  周波数（MHz）      : 7
  //<BAND:2>2m
  float tmp;
  int ret;
  ret=sscanf(qso->entry.freq,"%f",&tmp);
  sprintf(tmpbuf,"%.5f,",tmp/1000000.0);
  strcat(buf,tmpbuf);
  //8.  モード             : CW
  //<MODE:2>FM
  sprintf(tmpbuf,"%s,",qso->entry.opmode);
  strcat(buf,tmpbuf);
  // code  blank
  // gl    blank
  //11. QSL送受           : J   （J:発行済 / N:未発行 / W:希望）
  char *p;

  if ((p=strstr(qso->entry.remarks,"JARL"))!=NULL) {
    strcat(buf,",,J,");
  } else if ((p=strstr(qso->entry.remarks,"hQSL"))!=NULL) {
    strcat(buf,",,H,");
  } else {
    strcat(buf,",,,");
  }
  // Hisname    blank
  // QTH        blank
  strcat(buf,",,");
  
  // Remarks1  --> received_exchange % Op Location J: POTA_MY: SOTA_MY: args from Remarks mycall %
  strcat(buf,"\"");  
  strcat(buf,qso->entry.rcvexch);
  strcat(buf," %");
  if ((p=parse_strings(qso->entry.remarks,"J:"))!=NULL) {
    strcat(buf,"JCC/JCG:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  
  if ((p=parse_strings(qso->entry.remarks,"POTA_MY:"))!=NULL) {
    strcat(buf,"POTA_MY:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  
  if ((p=parse_strings(qso->entry.remarks,"SOTA_MY:"))!=NULL) {
    strcat(buf,"SOTA_MY:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  strcat(buf,qso->entry.mycall);
  strcat(buf,"%\",");
      
  // Remarks2  --> % contest_name % Remarks Sent_exchange
  // 0 ???
  // hamloguser or not
  strcat(buf,"\"");
  if (strlen(plogw->contest_name+2)>0) {
    strcat(buf,"%");
    strcat(buf,plogw->contest_name+2);
    strcat(buf,"% ");    
  }
  strcat(buf,qso->entry.sentexch);
  strcat(buf," ");
  strcat(buf,qso->entry.remarks);
  strcat(buf,"\",");
  // JM1LDV/1,25/08/16,22:23J,599,599,7.01502,CW,CODE,GL,J,HisName,QTH,Remarks1,Remarks2,0,
  strcat(buf,"\n");
}

void sprint_qso_entry_adif(char *buf,union qso_union_tag *qso) {
  *buf='\0';
  char tmpbuf[300],tmpbuf1[100];
  size_t len;

  // sprint qso entry in adif format
  //<EOH>
  //  strcat(buf,"<EOH>");
  //<STATION_CALLSIGN:6>JP7VAI

  //<CALL:6>JA7YAB    
  len=strlen(qso->entry.hiscall);
  sprintf(tmpbuf,"<CALL:%d>%s",len,qso->entry.hiscall);
  strcat(buf,tmpbuf);
  //
  sprintf(tmpbuf,"<STATION_CALLSIGN:%d>%s",strlen(qso->entry.mycall),qso->entry.mycall);
  strcat(buf,tmpbuf);
  //<QSO_DATE:8>20230611
  struct tm jst_tm;  
  jst_tm=parse_datetime(qso->entry.tm);
  // convert to gmt
  time_t jst_time = mktime(&jst_tm);
  time_t utc_time = jst_time - 9 * 3600;
  //  console->println(jst_time);
  //  console->println(utc_time);
  struct tm utc_tm = *localtime(&utc_time);  // UTCに変換
  sprintf(tmpbuf,"<QSO_DATE:8>%04d%02d%02d",
	  utc_tm.tm_year+1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);
  strcat(buf,tmpbuf);
  //<TIME_ON:4>0016  
  sprintf(tmpbuf,"<TIME_ON:4>%02d%02d",utc_tm.tm_hour, utc_tm.tm_min);
  strcat(buf,tmpbuf);
  
  //<MODE:2>FM
  len=strlen(qso->entry.opmode);
  if ((strcmp(qso->entry.opmode,"USB")==0)||(strcmp(qso->entry.opmode,"LSB")==0)) {
    strcat(buf,"<MODE:3>SSB");
    sprintf(tmpbuf,"<SUBMODE:%d>%s",strlen(qso->entry.opmode),qso->entry.opmode);
    strcat(buf,tmpbuf);    
  } else if ((strcmp(qso->entry.opmode,"CW")==0)||(strcmp(qso->entry.opmode,"CW-R")==0)) {
    strcat(buf,"<MODE:2>CW");
  } else {
    sprintf(tmpbuf,"<MODE:%d>%s",strlen(qso->entry.opmode),qso->entry.opmode);
    strcat(buf,tmpbuf);        
  }
  
  //<BAND:2>2m
  int tmp,ret;
  tmp=1;
  ret=sscanf(qso->entry.band,"%d",&tmp);
  sprintf(tmpbuf,"<BAND:%d>%s",strlen(band_str_adif[tmp-1]),band_str_adif[tmp-1]);
  strcat(buf,tmpbuf);

  //<RST_RCVD:2>59
  sprintf(tmpbuf,"<RST_RCVD:%d>%s",strlen(qso->entry.rcvrst),qso->entry.rcvrst);
  strcat(buf,tmpbuf);

  // SRX_STRING
  if (strlen(qso->entry.rcvexch)>0) {
    sprintf(tmpbuf,"<SRX_STRING:%d>%s",strlen(qso->entry.rcvexch),qso->entry.rcvexch);
    strcat(buf,tmpbuf);    
  }
  
  //<RST_SENT:2>59
  sprintf(tmpbuf,"<RST_SENT:%d>%s",strlen(qso->entry.sentrst),qso->entry.sentrst);
  strcat(buf,tmpbuf);
  // STX_STRING
  if (strlen(qso->entry.sentexch)>0) {
    sprintf(tmpbuf,"<STX_STRING:%d>%s",strlen(qso->entry.sentexch),qso->entry.sentexch);
    strcat(buf,tmpbuf);    
  }
  
  //<MY_SIG:4>POTA
  //<MY_SIG_INFO:7>JA-0110
  //<SIG_INFO>

  char *p;
  if ((p=parse_strings(qso->entry.remarks,"POTA_MY:"))!=NULL) {
    strcat(buf,"<MY_SIG:4>POTA");
    sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"POTA:"))!=NULL) {
    strcat(buf,"<SIG:4>POTA");
    sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"SOTA_MY:"))!=NULL) {
    strcat(buf,"<MY_SIG:4>SOTA");
    sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"SOTA:"))!=NULL) {
    strcat(buf,"<SIG:4>SOTA");
    sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  /*
  char *p;
  if ((p=strstr(qso->entry.remarks,"POTA_MY:"))!=NULL) { // my park information in POTA activation
    // pota information in remarks
    strcpy(tmpbuf1,p+8);
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      // got POTA token
      strcat(buf,"<MY_SIG:4>POTA");
      sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
      strcat(buf,tmpbuf);
    }
    // SOTA in similar way
  }
  if ((p=strstr(qso->entry.remarks,"POTA:"))!=NULL) { // hunting park information 
    // pota information in remarks
    strcpy(tmpbuf1,p+5);
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      // got POTA token
      strcat(buf,"<SIG:4>POTA");
      sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
      strcat(buf,tmpbuf);
    }
  }
  */
  //<OPERATOR:6>JP7VAI
  len=strcpy_to_chr(tmpbuf1,qso->entry.mycall,'/');
  sprintf(tmpbuf,"<OPERATOR:%d>%s",len,tmpbuf1);
  strcat(buf,tmpbuf);
  //<COMMENT>
  if (strlen(qso->entry.remarks)>0) {
    sprintf(tmpbuf,"<COMMENT:%d>%s",strlen(qso->entry.remarks),qso->entry.remarks);
    strcat(buf,tmpbuf);
  }
  //<EOR>
  strcat(buf,"<EOR>\n");
  
}
  

void string_trim_right(char *s, char c) {
  // replace first encountered c with '\0'

  while (*s) {
    if (*s == c) {
      *s = '\0';
      return;
    }
    s++;
  }
}
void print_qso_logfile() {
  // check qsologf is open
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile ()");
  }

  if (!qsologf) {
    if (!plogw->f_console_emu) plogw->ostream->println("qso logfile is not open.");
    return;
  }
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():1");
  }

  make_qsolog_entry();
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():2");
  }
  int len = sizeof(qso.all);
  int ret;
  ret = qsologf.write(qso.all, len);
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():3");
  }

  if (verbose & 1) {
    plogw->ostream->print("written QSO ,");
    plogw->ostream->print(ret);
    plogw->ostream->println("bytes");
  }
  // Do not block the key-input path on an SD media flush.  The record is
  // already copied into the filesystem buffer; process_qso_file_operation()
  // performs the actual flush after a short grace period.
  qso_log_flush_pending = true;
  qso_log_flush_due_ms = millis() + QSO_LOG_FLUSH_DELAY_MS;
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():4 flush deferred");
  }

  if (!plogw->f_console_emu) plogw->ostream->write(qso.all, len);
  if (!plogw->f_console_emu) {  
    write_allTCPclients((char *)qso.all,len);
    print_allTCPclients("\n");
  }
			
  /*  
  if (!plogw->f_console_emu) {
    for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
      if (serverClients[i] && serverClients[i].connected()) {
        plogw->ostream = &serverClients[i];
        plogw->ostream->write(qso.all, len);
        plogw->ostream->println("");
      }
    }
    plogw->ostream = console;
  }
  */

}

void print_qso_log() {
  // print content of logw
  if (!plogw->f_console_emu) plogw->ostream->println("print_qso_log()");
  struct radio *radio;
  //  radio = so2r.radio_selected();
  radio=so2r.radio_qso_process();

  if (verbose & 1) {
    plogw->ostream->print("QSO:");
    plogw->ostream->print(plogw->tm);
    plogw->ostream->print(",Freq,");
    plogw->ostream->print(radio->freq);
    plogw->ostream->print(",Mode,");
    plogw->ostream->print(radio->opmode);
    plogw->ostream->print(",call,");
    plogw->ostream->print(radio->callsign + 2);
    plogw->ostream->print(",rcvd rst,");
    plogw->ostream->print(radio->recv_rst + 2);
    plogw->ostream->print(",rcvd exch,");
    plogw->ostream->print(radio->recv_exch + 2);
    plogw->ostream->print(",my call,");
    plogw->ostream->print(plogw->my_callsign + 2);
    plogw->ostream->print(",sent rst,");
    plogw->ostream->print(radio->sent_rst + 2);
    plogw->ostream->print(",sent exch,");
    plogw->ostream->print(plogw->sent_exch + 2);
    plogw->ostream->println("");
  }
  print_qso_logfile();
  make_zlogqsodata(buf);
}

void expand_sent_exch(char *out, size_t out_size)
{
    struct radio *radio = so2r.radio_qso_process();
    char tmpbuf[40];

    if ((radio->bandid >= 11) && (radio->bandid <= 13)) {
        copy_token(tmpbuf, plogw->sent_exch + 2, 1, "/,;");
    } else {
        copy_token(tmpbuf, plogw->sent_exch + 2, 0, "/,;");
    }
    expand_macro_string(out,out_size,tmpbuf);
}

// Copy a callsign into the historical fixed-width QSO record field.
// Longer interactive/CALLHIST callsigns are intentionally truncated here so
// that existing QSO.TXT records remain binary-layout compatible.
static void copy_qso_callsign(char *dst, const char *src) {
  if (dst == NULL) return;
  if (src == NULL) src = "";
  strncpy(dst, src, LEN_QSO_CALLSIGN);
  dst[LEN_QSO_CALLSIGN] = '\0';
}

// create a single QSO log file entry (fixed length string)
void make_qsolog_entry() {
  struct radio *radio;
  char stmp[20];
  char tmp_buf[100];
  char *p1;
  //  radio = so2r.radio_selected();
  radio= so2r.radio_qso_process();
  // clear all
  memset(qso.all, ' ', sizeof(qso.all));
  strcpy(qso.entry.type, "Q");
  if (radio->qsodata_loaded) {
    // freq_loaded and tm_loaded are wrongly encoded 22/7/12
    // because frequency is not loaded correctly (shifted) at   set_qsodata_from_qso_entry()

    //    sprintf(qso.entry.freq, "%-10lld", radio->freq_loaded*((long long)FREQ_UNIT));
    sprintf(stmp, "%-11lld", radio->freq_loaded*((long long)FREQ_UNIT));
    strncpy(qso.entry.freq,stmp,11);
    strcpy(qso.entry.tm, radio->tm_loaded);
    sprintf(qso.entry.seqnr, "%-d", radio->seqnr_loaded);
    sprintf(qso.entry.band, "%-d", freq2bandid(radio->freq_loaded));
    strcpy(qso.entry.opmode, radio->opmode_loaded);
    strcpy(qso.entry.mode, modetype_str[modetype_string(radio->opmode_loaded)]);
  } else {
    //    sprintf(qso.entry.freq, "%-10lld", radio->freq*((long long)FREQ_UNIT));
    sprintf(stmp, "%-11lld", radio->freq*((long long)FREQ_UNIT));
    strncpy(qso.entry.freq,stmp,11);
    strcpy(qso.entry.tm, plogw->tm);
    sprintf(qso.entry.seqnr, "%-d", plogw->seqnr);
    sprintf(qso.entry.band, "%-d", freq2bandid(radio->freq));
    strcpy(qso.entry.opmode, radio->opmode);
    strcpy(qso.entry.mode, modetype_str[modetype_string(radio->opmode)]);
  }
  copy_qso_callsign(qso.entry.mycall, plogw->my_callsign + 2);
  strcpy(qso.entry.sentrst, radio->sent_rst + 2);
  
  char sentexch_buf[100];
  expand_sent_exch(sentexch_buf, sizeof(sentexch_buf));
  strcpy(qso.entry.sentexch, sentexch_buf);
//  strcpy(qso.entry.sentexch, expand_sent_exch());
  copy_qso_callsign(qso.entry.hiscall, radio->callsign + 2);
  strcpy(qso.entry.rcvrst, radio->recv_rst + 2);
  strcpy(qso.entry.rcvexch, radio->recv_exch + 2);
  qso.entry.remarks[0] = '\0';

  if (!radio->qsodata_loaded) {
    // add CQ/SP and TXnumber(7)/Random digits(2bytes) for determining QSOID for zLog (zserver)
    char tmpbuf[10];
    sprintf(tmpbuf,"%s %1d%02d ",
	    (radio->cq[radio->modetype]==LOG_CQ) ? "CQ":"SP",
	    (plogw->qsoid/100000000)%10,
	    (plogw->qsoid/100)%100);
    strcat(qso.entry.remarks,tmpbuf);

    if ((radio->modetype==LOG_MODETYPE_PH) && (radio->f_tone_keying)) {
      // F2A
      strcat(qso.entry.remarks,"F2A ");
    }
    if (plogw->sat) {
      // satellite qso add satellite name and grid locator before remarks
      strcat(qso.entry.remarks, plogw->sat_name_set);
      strcat(qso.entry.remarks, " ");
      strcat(qso.entry.remarks, plogw->grid_locator_set);
      strcat(qso.entry.remarks, " ");
      char buf[30];
      sprintf(buf, "O:%d ", sat_info[plogw->sat_idx_selected].offset_freq);
      strcat(qso.entry.remarks, buf);
    }
    if (strlen(plogw->jcc + 2) > 0) {
      // check POTA and SOTA number designators P: S: (after JCC/JCG numbers )
      if ((p1=strstr(plogw->jcc+2,"POTA/"))!=NULL) { // my park information in POTA activation
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "POTA_MY:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks," ");
	}
      }
      if ((p1=strstr(plogw->jcc+2,"SOTA/"))!=NULL) { // my park information in POTA activation
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "SOTA_MY:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks," ");
	}
      }
      if ((plogw->jcc[2]!='P') && (plogw->jcc[2]!='S')) { // jcc/jcg at the beginning
	char tmpbuf1[100];
	strcpy(tmpbuf1,plogw->jcc+2);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "J:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks, " ");
	}
      }
    }
    if (radio->smeter_stat >= 1) {
      // record peak s-meter value
      if (radio->smeter_peak > SMETER_MINIMUM_DBM) {
        strcat(qso.entry.remarks, "S:");
        char buf[10];
		 dtostrf(radio->smeter_peak/(SMETER_UNIT_DBM*1.0),-1,1,buf);
        //sprintf(buf, "%d", radio->smeter_peak);
        strcat(qso.entry.remarks, buf);
        if (plogw->relay[0] != 0) {
          sprintf(buf, " A:%1d", plogw->relay[0] + plogw->relay[1] * 2);
          strcat(qso.entry.remarks, buf);
        }
        strcat(qso.entry.remarks, " ");
      }
      radio->smeter_stat = 0;  // end reading s-meter
    }
    if (plogw->f_off_contest) {
      // write contest name with C: prefix, if not NOMULTI
      // if f_off_contest, C:- is written
      strcat(qso.entry.remarks,"C:- ");
    } else {
      if (strlen(plogw->contest_name+2)>0) {
	if (strcmp(plogw->contest_name+2,"NOMULTI")!=0) {
	  sprintf(buf,"C:%s ",plogw->contest_name+2);
	  strcat(qso.entry.remarks,buf);
	}
      }
    }
    switch(radio->f_qsl) {
    case 1: // JARL
      strcat(qso.entry.remarks,"JARL ");
      break;
    case 2: // hQSL
      strcat(qso.entry.remarks,"hQSL ");
      break;
    }
  } else {
    strcpy(qso.entry.remarks, "*E ");
  }
  strcat(qso.entry.remarks, radio->remarks + 2);
  strcat(qso.entry.remarks, "\n");

  // replace 0x00 with 0x20 (spc)
  uint8_t *p;
  int i;
  for (i = 0, p = qso.all; i < sizeof(qso.all); i++) {
    if (*p == 0x00) {
      *p = ' ';
    }
    p++;
  }
  *(p - 1) = 0x0d;  // last character is CR
}

void make_zlogqsodata(char *buf)
{
  struct radio *radio;
  //  radio = so2r.radio_selected();
  radio = so2r.radio_qso_process();  
  
  char buf1[50];
  long st; double stfloat;
  //  DateTime reftime;
  //  reftime=DateTime(2024,4,2);
  
  //  st=rtctime.unixtime();
  //  console->print("secondstime:");
  //  console->println(st);
  //  console->print("reftime 2024/4/2 00:00:00");
  //  console->println(reftime.unixtime()); // 1712016000/86400 = 19815 2024/4/2 00:00:00 day 45384 in zLog
  // unixtime()/86400 + (45384-19815) = zLog day
  // unixtime()%86400/86400.0 = zLog time
  *buf='\0';
  strcat(buf,"#ZLOG# PUTQSO ZLOGQSODATA:~"); // 1 identification
  // DateTime
  //  st=rtctime.unixtime();
  st=my_rtc.unixtime();
  if (verbose&4) 	{  
    console->print("tm:");
    console->println(plogw->tm);
    console->print("sec of day:");
    console->println(st%86400);
  }
  stfloat=(st%86400)/86400.0L;
  stfloat+=((st/86400)+(45384-19815));
  
  sprintf(buf1,"%.8lf~",stfloat);
  strcat(buf,buf1); // 2 datetime
  strcat(buf,radio->callsign+2);strcat(buf,"~"); // 3 callsign
  //  strcat(buf,plogw->sent_exch+2);strcat(buf,"~"); // 4 sent exch
  
  char sentexch_buf[100];
  expand_sent_exch(sentexch_buf, sizeof(sentexch_buf));
  
  strcat(buf,sentexch_buf);strcat(buf,"~"); // 4 sent exch
  strcat(buf,radio->recv_exch+2);strcat(buf,"~"); // 5 recv exch 
  strcat(buf,radio->sent_rst+2);strcat(buf,"~");  // 6 sent rst
  strcat(buf,radio->recv_rst+2);strcat(buf,"~"); // 7 recv rst
  sprintf(buf1,"%d",plogw->seqnr);strcat(buf,buf1);strcat(buf,"~"); // 8 serial
  sprintf(buf1,"%d",opmode2zLogmode(radio->opmode));strcat(buf,buf1);strcat(buf,"~"); // 9 mode
  sprintf(buf1,"%d",zserver_bandid_freqcodes_map[radio->bandid]);strcat(buf,buf1);strcat(buf,"~"); // 10 band 
  char *s;int tmp,pwr,txnum;
  s=power_code(radio->bandid);
  if (strcmp(s,"P")==0) {
    tmp=2;pwr=5;
  } else if (strcmp(s,"L")==0) {
    tmp=3;pwr=10;
  } else if (strcmp(s,"M")==0) {
    tmp=6;pwr=50;
  } else if (strcmp(s,"H")==0) {
    tmp=10;pwr=1000;
  } else {
    tmp=6;pwr=50;
  }
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~");  // 11 power code
  strcat(buf,"~"); // 12 multi1 ?
  strcat(buf,"~"); // 13 multi2 ?
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 14 new multi flag1 ?  
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 15 new multi flag2 ?
  tmp=  (radio->modetype == LOG_MODETYPE_CW) ? plogw->cw_pts : 1;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 16 points
  strcat(buf,plogw->my_name+2); strcat(buf,"~"); // 17 op name
  strcat(buf,radio->remarks+2); strcat(buf,"~"); // 18 memo
  tmp= radio->cq[radio->modetype]==LOG_CQ ? 1:0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 19 CQ flag
  tmp= radio->dupe;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 20 Dupe flag
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1); strcat(buf,"~");  // 21 reserve  in zlog 10 in here
  //  txnum = 7; // txnum =7+ hostname last letter digit (<3) so that txnum <=9
  txnum = plogw->txnum; // txnum =7+ hostname last letter digit (<3) so that txnum <=9  
  sprintf(buf1,"%d",txnum);strcat(buf,buf1);strcat(buf,"~"); // 22 tx number
  sprintf(buf1,"%d",pwr);strcat(buf,buf1);strcat(buf,"~"); // 23 power code 2 ARRL DX
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 24 reserve 2
  // qsoid
  //  tmp= (txnum)* 100000000 + plogw->seqnr *10000 + random(100)*100;
  tmp=plogw->qsoid;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 25 qsoid
  if (verbose&4) 	{
    console->print("Freq:");console->println(radio->freq*FREQ_UNIT);
  }
  sprintf(buf1,"%lld",radio->freq*((long long)FREQ_UNIT));strcat(buf,buf1);strcat(buf,"~"); // 26 Freq
  tmp=0; 
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 27 QSY ihan 0 no ihan 1 ihan
  strcat(buf,plogw->hostname+2);strcat(buf,"~"); // 28 pc name
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 29 Force flag
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 30 QSL 0 not set 1 PSEQSL 2 NOQSL
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1); // 31 validity 0 valid 1 invalid
  // example from zLog
  //#ZLOG# PUTQSO ZLOGQSODATA:~45472.5651934954~JA1ZLO~11H~10H~599~599~1~0~12~3~10~~1~0~1~   ~~0~0~10~0~500~0~30252000  ~         ~0~thinkpad~0~0~0
  //#ZLOG# PUTQSO ZLOGQSODATA:~45472.56640625  ~JA1ZLO~11M~10H~599~599~1~0~11~6~  ~~0~0~1~Ron~~1~0~ ~10~ 50~0~1000010089~430054600~0~jk1dvplog~0~0~0
  // from this prog  
  // print	  
  if (verbose&4) 	{
    console->print("QSOzlog:");
    console->println(buf);
  }
  zserver_send(buf);
}


void dump_qso_current(Stream *out) {
  if (!out) out = console;
  // dump current qso
  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);
  pos = qsologf.position();
  memo_pos = pos;
  // pos = pos - len;  // start from the end record
  //pos = 0; // start from the beginning
  pos = info_disp.pos;
  out->print("pos= ");
  out->print(info_disp.pos);
  out->print(" # ");
  out->println(info_disp.pos/sizeof(qso.all));    
  
  if (!qsologf.seek(pos)) {
    if (!plogw->f_console_emu) out->println("file seek failed");
    goto end;
  }
  ret = qsologf.read(qso.all, len);
  if (ret != len) {
    //
    if (!plogw->f_console_emu) {
      out->print("qso not read bytes=");
      out->println(ret);
    }
    goto end;
  }
  // check type
  if (qso.entry.type[0] != 'Q') {
    // not vaild qso
    if (!plogw->f_console_emu) out->println("not valid qso encountered");
    //     goto end;
  }
  // print content
  out->write(qso.all,len); // flush
end:
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) out->println("file seek to end failed");
  }
}

void dump_qso_log(Stream *out) {
  if (!out) out = console;
  // seek to the first byte and dump
  if (!qsologf.seek(0)) {
    if (!plogw->f_console_emu) out->println("file seek failed");
  }
  if (!plogw->f_console_emu) {
    out->print("printing contents of ");
    out->println(qsologfn);
  }
  int count;
  count = 0;
  int count_buf;char *pbuf;
  count_buf=0;pbuf=buf;
  while (qsologf.available()) {
    char c;
    c = qsologf.read();
    if (count_buf>=512) {
      out->write(buf,count_buf); // flush
      out->flush();
      delay(1); // task switch
      count_buf=0;
      pbuf=buf;
    }
    *pbuf++=c; // store
    count_buf++;
    
  }
  // flush remaining
  out->write(buf,count_buf); // flush
  
  
  if (!plogw->f_console_emu) {
    out->println("\nend printing.");
    out->print("position in file:");
    out->println(qsologf.position());
  }
}

void dump_qso_bak(char *numstr, Stream *out) {
  if (!out) out = console;
  char fname[30];
  sprintf(fname, "/qsobak.%s", numstr);
  if (SD.exists(fname)) {

    out->println(fname);
    out->println("-- begin --");
    File file = SD.open(fname, "r");

    int count_buf;char *pbuf;
    count_buf=0;pbuf=buf;
    
    while (file.available()) {
      char c;
      c = file.read();


      if (count_buf>=512) {
	out->write(buf,count_buf); // flush
	out->flush();
	count_buf=0;
	pbuf=buf;
      }
      *pbuf++=c; // store
      count_buf++;
      
      //      out->print(c);
    }
    // flush remaining
    out->write(buf,count_buf); // flush
    
    out->println("\n-- end --");
    file.close();
  } else {
    out->print(fname);
    out->println("not exist");
  }
}
