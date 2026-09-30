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
#include "SD.h"
#include "decl.h"
#include "variables.h"
#include "multi_process.h"
#include "dupechk.h"
#include "multi.h"
#include "display.h"
#include "contest.h"
#include "qso.h"
#include "so2r.h"
#include "user_contest_md.h"
#include "web_server.h"
#include "esp_heap_caps.h"


struct contest_definition {
  int  id;  
  char *name;
  int mask;
  int cw_pts;  
  int multi_type;
  const struct multi_item *multi1;
  int multi1_start_band ;
  int multi1_stop_band ;  
  const struct multi_item *multi2;
  int multi2_start_band ;
  int multi2_stop_band ;  
};


const struct contest_definition contest_defs[N_CONTEST+1] = {
  // id , name     , CW_PH_DUPE, cw_pts,multi_type, multi1 ,multi_start/end band,   multi2 ... 
  { 0,"NOMULTI"    ,CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_acag,-1,-1,       NULL,-1,-1 },
  { 8,"JANoPwr"    ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_allja,-1,-1,NULL,-1,-1 },  
  { 43,"JANoPwrDualMode"    ,CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_allja,-1,-1,NULL,-1,-1 },  
  { 7,"AllJA"      ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR  ,&multi_allja,-1,-1,NULL,-1,-1 },
  {18,"ACAG"       ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR  ,&multi_acag, 1,13,NULL,-1,-1 },
  {19,"FD"         ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR  ,&multi_allja, 1,10,&multi_acag,11,-1 },
  {30,"6D"         ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR  ,&multi_allja, 1,10,&multi_acag,11,-1 }, 
  { 3,"CQWW"       ,CW_PH_DUPE_NG,1,MULTI_TYPE_CQWW,&multi_cqzones,-1,-1,NULL,-1,-1 },
  {44,"CQWWRTTY"   ,CW_PH_DUPE_NG,1,MULTI_TYPE_CQWWRTTY,&multi_cqzones,-1,-1,NULL,-1,-1 },
  {15,"ARRLDX"     ,CW_PH_DUPE_NG,1,MULTI_TYPE_ARRLDX ,&multi_arrldx, -1,-1,NULL,-1,-1 },
  {20,"ARRL10m"    ,CW_PH_DUPE_OK,1,MULTI_TYPE_ARRL10M,&multi_arrl10m, -1,-1,NULL,-1,-1 },    
  { 1,"TAMAGAWA"   ,CW_PH_DUPE_OK,2,MULTI_TYPE_NORMAL,&multi_tama,-1,-1,       NULL,-1,-1 },
  { 2,"TOKYOUHF"   ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_tokyouhf,-1,-1,NULL,-1,-1 },
  { 4,"Saitama"    ,CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_saitama_int,-1,-1,NULL,-1,-1 },
  { 5,"KCJ"        ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_kcj,-1,-1,NULL,-1,-1 },
  { 6,"KantoUHF"   ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_kantou,-1,-1,NULL,-1,-1 },
  {10,"KanagawaInt",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_knint,-1,-1,NULL,-1,-1 },
  {11,"Yokohama"   ,CW_PH_DUPE_OK,3,MULTI_TYPE_NORMAL,&multi_yk,-1,-1,NULL,-1,-1 },
  {12,"UEC"        ,CW_PH_DUPE_NG,2,MULTI_TYPE_UEC   ,&multi_allja, -1,-1,NULL,-1,-1 },
  {13,"Tsurumigawa",CW_PH_DUPE_OK,2,MULTI_TYPE_NORMAL,&multi_tmtest, -1,-1,NULL,-1,-1 },
  {16,"HSWAS"      ,CW_PH_DUPE_OK,1,MULTI_TYPE_GL_NUMBERS,&multi_hswas, -1,-1,NULL,-1,-1 },
  {17,"Yamanashi"  ,CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_yntest, -1,-1,NULL,-1,-1 },
  {21,"MusashinoL" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_musashino_line,-1,-1,NULL,-1,-1 },
  {22,"KCWA"       ,CW_PH_DUPE_NG,1,MULTI_TYPE_KCWA,&multi_kcj,-1,-1,NULL,-1,-1 },
  {23,"TOKAIQSO"   ,CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_tki,-1,-1,NULL,-1,-1 },
  {24,"UEC_VUS"    ,CW_PH_DUPE_OK,2,MULTI_TYPE_NORMAL,&multi_allja, -1,-1,&multi_acag,8,13 },    // * OK to QSO in AM,FM,SSB,CW
  //  {20,"MusashinoL" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_musashino_line, -1,-1,NULL,-1,-1 },
  {14,"Ja8Int"     ,CW_PH_DUPE_NG,1,MULTI_TYPE_NOCHK_LASTCHR,&multi_allja, -1,-1,NULL,-1,-1 },
  {28,"Ja8Out"     ,CW_PH_DUPE_NG,1,MULTI_TYPE_NOCHK_LASTCHR,&multi_ja8out, -1,-1,NULL,-1,-1 },
  { 9,"ACAGnochk"  ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR_NOMULTICHK  ,NULL,-1,-1,NULL,-1,-1 },
  {25,"AAtest"     ,CW_PH_DUPE_NG,1,MULTI_TYPE_AA,&multi_aatest,-1,-1,  NULL,-1,-1 },
  {27,"ShimaneAllJAOut",CW_PH_DUPE_OK,1,(MULTI_TYPE_KENGAI | (32<<8)),&multi_acag,-1,-1,  NULL,-1,-1 }, // 32 is SHimane Ken number    
  {26,"ShimaneAllJAInt",CW_PH_DUPE_OK,1,(MULTI_TYPE_KENNAI | (32<<8)),&multi_acag,-1,-1,  NULL,-1,-1 }, // 32 is SHimane Ken number
  {29,"Tochigi",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_acag,-1,-1,  NULL,-1,-1 }, 
  {31,"OkhotskOut" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_okhotskout,-1,-1,NULL,-1,-1 },      
  {32,"OkhotskInt" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_okhotskint,-1,-1,NULL,-1,-1 },
  {33,"JA5Out" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_ja5out,-1,-1,NULL,-1,-1 },      
  {34,"JA5Int" ,CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_ja5int,-1,-1,NULL,-1,-1 },
  {35,"Shiga",CW_PH_DUPE_NG,1,(MULTI_TYPE_KENNAI | (23<<8)),&multi_acag,-1,-1,  NULL,-1,-1 }, // 23 is Shiga Ken number
  {36,"AomoriInt",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_aomori_int,-1,-1,  NULL,-1,-1 }, 
  {41,"AomoriOut",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_aomori_out,-1,-1,  NULL,-1,-1 }, 
  {38,"IburiHidakaOut",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_iburihidakaout,-1,-1,  NULL,-1,-1 }, 
  {37,"IburiHidakaInt",CW_PH_DUPE_NG,1,MULTI_TYPE_NORMAL,&multi_iburihidakaint,-1,-1,  NULL,-1,-1 }, 
  {39,"KagoshimaOut",CW_PH_DUPE_OK,1,(MULTI_TYPE_KENGAI_KJ | (46<<8)),&multi_acag,-1,-1,  NULL,-1,-1 }, // 46 is Kagoshima Ken number
  {40,"KagoshimaInt",CW_PH_DUPE_OK,1,(MULTI_TYPE_KENNAI_KJ | (46<<8)),&multi_acag,-1,-1,  NULL,-1,-1 }, // 46 is Kagoshima Ken number
  {42,"GigaContest",CW_PH_DUPE_OK,1,MULTI_TYPE_NORMAL,&multi_giga0area,-1,-1,  NULL,-1,-1 }, 
  { -1,""         ,0   ,0,0,NULL,-1,-1,NULL,-1,-1  }
};
//  { 0,"NOMULTI"   ,CW_PH_DUPE_NG,1,0,&multi_test_line,-1,-1,NULL,-1,-1 }, 


namespace {

struct contest_stats_slot {
  bool valid;
  // True only after a full MAKEDUPE reconstruction for this contest.
  // Live dual-contest updates alone must not make a slot look complete.
  bool rebuilt;
  uint8_t contest_id;
  struct score score_data;
  uint8_t multi_worked_bits[N_BAND][(N_MULTI + 7) / 8];
};
static contest_stats_slot contest_stats[2] = {};

// Reused workspaces for built-in previous-contest multiplier checks.  The old
// implementation allocated, built and freed both tables for every QSO, which
// made a dual-contest MAKEDUPE unnecessarily expensive.
static struct multi_list *previous_multi_saved = NULL;
static struct multi_list *previous_multi_cached = NULL;
static int previous_multi_cached_id = -1;

static int contest_stats_find(uint8_t id) {
  for (int i = 0; i < 2; ++i)
    if (contest_stats[i].valid && contest_stats[i].contest_id == id) return i;
  return -1;
}
static int contest_stats_get(uint8_t id) {
  int i = contest_stats_find(id);
  if (i >= 0) return i;
  for (i = 0; i < 2; ++i) if (!contest_stats[i].valid) break;
  if (i >= 2) {
    // Do not assume slot 0 is the active contest.  After startup it commonly
    // still contains NOMULTI, which caused slot 1 to alternate between Main
    // and Sub and made the multiplier count disappear on every switch.
    // Protect the actually active contest and evict the other/older slot.
    uint8_t active_id = 0;
    if (plogw != NULL) {
      if (is_user_md_contest_name(plogw->contest_name + 2))
        active_id = user_md_runtime_id(plogw->contest_name + 2, false);
      else if (plogw->contest_id >= 0 &&
               plogw->contest_id < USER_MD_RUNTIME_ID_FIRST)
        active_id = (uint8_t)plogw->contest_id;
    }
    i = (contest_stats[0].contest_id == active_id) ? 1 : 0;
    if (plogw && plogw->ostream)
      plogw->ostream->printf(
          "CONTEST STATS: replace slot=%d old=%u new=%u active=%u\n",
          i, (unsigned int)contest_stats[i].contest_id,
          (unsigned int)id, (unsigned int)active_id);
  }
  memset(&contest_stats[i], 0, sizeof(contest_stats[i]));
  contest_stats[i].valid = true;
  contest_stats[i].contest_id = id;
  return i;
}

struct contest_selection {
  int id;
  char name[sizeof(plogw->contest_name) - 2];
};

static contest_selection active_contest = {0, ""};
static contest_selection previous_contest = {0, ""};
static bool active_contest_valid = false;
static bool previous_contest_valid = false;
static bool suppress_contest_history = false;
static bool contest_history_loaded = false;
static contest_selection saved_active = {0, ""};
static contest_selection saved_previous = {0, ""};
static bool saved_active_valid = false;
static bool saved_previous_valid = false;
static char pending_pair_main[sizeof(plogw->contest_name) - 2] = "";
static bool explicit_contest_pair = false;

static void contest_entry_set_single_impl(const char *name) {
  explicit_contest_pair = false;
  strlcpy(plogw->contest_entry + 2, name ? name : "",
          sizeof(plogw->contest_entry) - 2);
  plogw->contest_entry[1] = strlen(plogw->contest_entry + 2);
}

static void contest_entry_set_pair(const char *main_name,
                                   const char *sub_name) {
  explicit_contest_pair = true;
  plogw->contest_entry[2] = '\0';
  strlcpy(plogw->contest_entry + 2, main_name,
          sizeof(plogw->contest_entry) - 2);
  strlcat(plogw->contest_entry + 2, ",",
          sizeof(plogw->contest_entry) - 2);
  strlcat(plogw->contest_entry + 2, sub_name,
          sizeof(plogw->contest_entry) - 2);
  plogw->contest_entry[1] = strlen(plogw->contest_entry + 2);
}

static bool persisted_contest_is_available(const contest_selection &s) {
  if (!s.name[0]) return false;
  if (!is_user_md_contest_name(s.name)) return true;
  char canonical[sizeof(s.name)];
  if (!canonicalize_user_md_contest_name(s.name, canonical,
                                         sizeof(canonical))) return false;
  char filename[24];
  snprintf(filename, sizeof(filename), "/%s.MD", canonical + 4);
  return SD.exists(filename);
}

static void load_contest_history() {
  if (contest_history_loaded) return;
  contest_history_loaded = true;
  File f = SD.open("/CONTEST.TXT", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    contest_selection *dst = NULL;
    int prefix = 0;
    if (line.startsWith("#CONTEST_ACTIVE=")) {
      dst = &saved_active; prefix = 16;
    } else if (line.startsWith("#CONTEST_PREVIOUS=")) {
      dst = &saved_previous; prefix = 18;
    }
    if (!dst) continue;
    String value = line.substring(prefix);
    int comma = value.indexOf(',');
    if (comma <= 0) continue;
    dst->id = value.substring(0, comma).toInt();
    strlcpy(dst->name, value.substring(comma + 1).c_str(), sizeof(dst->name));
    if (is_user_md_contest_name(dst->name)) {
      char canonical[sizeof(dst->name)];
      if (canonicalize_user_md_contest_name(dst->name, canonical,
                                            sizeof(canonical)))
        strlcpy(dst->name, canonical, sizeof(dst->name));
    }
    if (dst == &saved_active) saved_active_valid = persisted_contest_is_available(*dst);
    else saved_previous_valid = persisted_contest_is_available(*dst);
  }
  f.close();
}

static void save_contest_history() {
  if (!active_contest_valid) return;
  const char *tmp_name = "/CONTEST.NEW";
  const char *bak_name = "/CONTEST.BAK";
  if (SD.exists(tmp_name)) SD.remove(tmp_name);
  File dst = SD.open(tmp_name, FILE_WRITE);
  if (!dst) return;

  // Preserve every setting except old history records.  History used to be
  // append-only, so replace all accumulated copies with one current pair.
  File src = SD.open("/CONTEST.TXT", FILE_READ);
  if (src) {
    while (src.available()) {
      String line = src.readStringUntil('\n');
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      if (line.startsWith("#CONTEST_ACTIVE=") ||
          line.startsWith("#CONTEST_PREVIOUS=")) continue;
      dst.println(line);
    }
    src.close();
  }
  dst.printf("#CONTEST_ACTIVE=%d,%s\n", active_contest.id, active_contest.name);
  if (previous_contest_valid)
    dst.printf("#CONTEST_PREVIOUS=%d,%s\n",
               previous_contest.id, previous_contest.name);
  dst.flush();
  dst.close();

  if (SD.exists(bak_name)) SD.remove(bak_name);
  if (SD.exists("/CONTEST.TXT") &&
      !SD.rename("/CONTEST.TXT", bak_name)) {
    if (SD.exists(tmp_name)) SD.remove(tmp_name);
    return;
  }
  if (!SD.rename(tmp_name, "/CONTEST.TXT")) {
    if (SD.exists(bak_name)) SD.rename(bak_name, "/CONTEST.TXT");
    return;
  }
  if (SD.exists(bak_name)) SD.remove(bak_name);
}

static bool same_contest(const contest_selection &a, const contest_selection &b) {
  return a.id == b.id && strcasecmp(a.name, b.name) == 0;
}

static uint8_t contest_dupe_id_for_name_impl(const char *name, int built_in_id) {
  if (is_user_md_contest_name(name)) return user_md_runtime_id(name, false);
  return (built_in_id >= 0 && built_in_id < USER_MD_RUNTIME_ID_FIRST)
             ? (uint8_t)built_in_id : 0;
}

static uint8_t current_contest_dupe_id_impl() {
  return active_contest_valid
      ? contest_dupe_id_for_name_impl(active_contest.name, active_contest.id) : 0;
}

static uint8_t previous_contest_dupe_id_impl() {
  return previous_contest_valid
      ? contest_dupe_id_for_name_impl(previous_contest.name, previous_contest.id) : 0;
}

static int contest_dupe_mask_for_name_impl(const char *name, int built_in_id,
                                           int fallback_mask) {
  if (is_user_md_contest_name(name)) {
    int mask;
    if (get_contest_runtime_dupe_mask(name, &mask)) return mask;
    return fallback_mask;
  }
  for (int i = 0; i < N_CONTEST && contest_defs[i].id != -1; ++i)
    if (contest_defs[i].id == built_in_id) return contest_defs[i].mask;
  return fallback_mask;
}

static void contest_name_for_id(int id, char *dst, size_t dst_size) {
  if (dst == NULL || dst_size == 0) return;
  dst[0] = '\0';

  if (id == USER_MD_CONTEST_ID &&
      is_user_md_contest_name(plogw->contest_name + 2)) {
    strlcpy(dst, plogw->contest_name + 2, dst_size);
    return;
  }

  for (int i = 0; i < N_CONTEST; i++) {
    if (contest_defs[i].id == -1) break;
    if (contest_defs[i].id == id) {
      strlcpy(dst, contest_defs[i].name, dst_size);
      return;
    }
  }
}

static void note_contest_selection(int id, const char *name) {
  load_contest_history();
  contest_selection next;
  next.id = id;
  const char *source_name = name != NULL ? name : "";
  char canonical[sizeof(next.name)];
  if (is_user_md_contest_name(source_name) &&
      canonicalize_user_md_contest_name(source_name, canonical,
                                        sizeof(canonical)))
    source_name = canonical;
  strlcpy(next.name, source_name, sizeof(next.name));

  if (!active_contest_valid) {
    active_contest = next;
    active_contest_valid = true;
    // settings.txt contains only one contest.  Recover the other side of the
    // active/previous pair from the last successful operating session.
    if (saved_active_valid && same_contest(saved_active, next) &&
        saved_previous_valid) {
      previous_contest = saved_previous;
      previous_contest_valid = true;
    } else if (saved_previous_valid && same_contest(saved_previous, next) &&
               saved_active_valid) {
      previous_contest = saved_active;
      previous_contest_valid = true;
    }
    set_dupechk_contest_id(current_contest_dupe_id_impl());
    save_contest_history();
    return;
  }
  if (same_contest(active_contest, next)) return;

  if (!suppress_contest_history) {
    previous_contest = active_contest;
    previous_contest_valid = true;
  }
  active_contest = next;
  set_dupechk_contest_id(current_contest_dupe_id_impl());
  save_contest_history();
}

}  // namespace

void contest_entry_set_single(const char *name) {
  contest_entry_set_single_impl(name);
}

void contest_commit_single_mode() {
  // A contest name entered without a comma is an explicit request for
  // single-contest operation.  set_contest_id() normally remembers the old
  // active contest as "previous"; discard it here so it cannot silently
  // return as SUB after restart or participate in dual-contest processing.
  previous_contest.id = 0;
  previous_contest.name[0] = '\0';
  previous_contest_valid = false;
  saved_previous.id = 0;
  saved_previous.name[0] = '\0';
  saved_previous_valid = false;
  pending_pair_main[0] = '\0';
  explicit_contest_pair = false;
  if (active_contest_valid)
    contest_entry_set_single_impl(active_contest.name);
  save_contest_history();
}

bool restore_saved_contest_selection() {
  load_contest_history();
  if (!saved_active_valid) return false;

  // settings.txt stores only contest_id.  That is insufficient for User MD
  // contests because every User contest shares USER_MD_CONTEST_ID.  Restore
  // the exact canonical name before set_contest_id() runs.
  plogw->contest_id = saved_active.id;
  strlcpy(plogw->contest_name + 2, saved_active.name,
          sizeof(plogw->contest_name) - 2);
  plogw->contest_name[1] = strlen(plogw->contest_name + 2);

  if (saved_previous_valid) {
    contest_entry_set_pair(saved_active.name, saved_previous.name);
  } else {
    contest_entry_set_single_impl(saved_active.name);
  }

  if (plogw->ostream)
    plogw->ostream->printf(
        "CONTEST RESTORE: main=<%s> sub=<%s>\n",
        saved_active.name,
        saved_previous_valid ? saved_previous.name : "");

  // Keep both User MD multiplier tables resident after a dual-User restart.
  // Load SUB first and let process_pending_contest_pair() select MAIN after
  // the first asynchronous load completes.  The MAKEDUPE request remains
  // pending until the final MAIN table is active, so the log is scanned once.
  if (saved_previous_valid &&
      is_user_md_contest_name(saved_active.name) &&
      is_user_md_contest_name(saved_previous.name)) {
    if (select_contest_pair(saved_active.name, saved_previous.name))
      return true;

    // Fall back to the saved MAIN contest if the pair preload could not be
    // started. settings.cpp will use the ordinary set_contest_id() path.
    plogw->contest_id = saved_active.id;
    strlcpy(plogw->contest_name + 2, saved_active.name,
            sizeof(plogw->contest_name) - 2);
  }
  return true;
}

uint8_t contest_dupe_id_for_name(const char *name, int built_in_id) {
  return contest_dupe_id_for_name_impl(name, built_in_id);
}
uint8_t current_contest_dupe_id() { return current_contest_dupe_id_impl(); }
uint8_t previous_contest_dupe_id() { return previous_contest_dupe_id_impl(); }
int contest_dupe_mask_for_name(const char *name, int built_in_id,
                               int fallback_mask) {
  return contest_dupe_mask_for_name_impl(name, built_in_id, fallback_mask);
}
int previous_contest_dupe_mask() {
  return previous_contest_valid
      ? contest_dupe_mask_for_name_impl(previous_contest.name,
                                        previous_contest.id, plogw->mask)
      : plogw->mask;
}

int contest_definition_count() {
  int count = 0;
  while (count < N_CONTEST && contest_defs[count].id != -1) ++count;
  return count;
}

int contest_definition_id(int index) {
  if (index < 0 || index >= contest_definition_count()) return -1;
  return contest_defs[index].id;
}

const char *contest_definition_name(int index) {
  if (index < 0 || index >= contest_definition_count()) return "";
  return contest_defs[index].name;
}

int contest_definition_mask(int index) {
  if (index < 0 || index >= contest_definition_count()) return 0;
  return contest_defs[index].mask;
}

static bool resolve_contest_name(const char *input, char *name,
                                 size_t name_size, int *id) {
  if (!input || !*input || !name || name_size == 0 || !id) return false;
  const size_t input_len = strlen(input);
  for (int i = 0; i < N_CONTEST && contest_defs[i].id != -1; ++i) {
    if (strncasecmp(contest_defs[i].name, input, input_len) == 0) {
      strlcpy(name, contest_defs[i].name, name_size);
      *id = contest_defs[i].id;
      return true;
    }
  }

  char candidate[LEN_CONTEST_NAME + 1];
  if (is_user_md_contest_name(input))
    strlcpy(candidate, input, sizeof(candidate));
  else
    snprintf(candidate, sizeof(candidate), "User%s", input);
  if (!canonicalize_user_md_contest_name(candidate, name, name_size))
    return false;
  *id = USER_MD_CONTEST_ID;
  return true;
}

static bool select_resolved_contest(const char *name, int id) {
  plogw->contest_id = id;
  strlcpy(plogw->contest_name + 2, name, sizeof(plogw->contest_name) - 2);
  set_contest_id();
  return strcasecmp(plogw->contest_name + 2, name) == 0;
}

bool select_contest_pair(const char *main_input, const char *sub_input) {
  char main_name[sizeof(plogw->contest_name) - 2];
  char sub_name[sizeof(plogw->contest_name) - 2];
  int main_id = -1, sub_id = -1;
  if (!resolve_contest_name(main_input, main_name, sizeof(main_name), &main_id) ||
      !resolve_contest_name(sub_input, sub_name, sizeof(sub_name), &sub_id))
    return false;

  contest_entry_set_pair(main_name, sub_name);

  pending_pair_main[0] = '\0';
  if (!select_resolved_contest(sub_name, sub_id)) return false;
  if (user_md_contest_loading()) {
    strlcpy(pending_pair_main, main_name, sizeof(pending_pair_main));
    return true;
  }
  return select_resolved_contest(main_name, main_id);
}

void process_pending_contest_pair() {
  if (!pending_pair_main[0] || user_md_contest_loading()) return;
  char main_name[sizeof(pending_pair_main)];
  strlcpy(main_name, pending_pair_main, sizeof(main_name));
  pending_pair_main[0] = '\0';
  int id = -1;
  char resolved[sizeof(main_name)];
  if (!resolve_contest_name(main_name, resolved, sizeof(resolved), &id) ||
      !select_resolved_contest(resolved, id)) {
    upd_display_info_flash("Dual contest\nMAIN select failed");
    info_disp.timer = 2000;
  }
}


int is_international_contest()
{
  switch(plogw->contest_id) {
  case 0: // no multi
  case 3: // cqww
  case 44: // cqww rtty
  case 15:// arrl1x
  case 20:// arrl10m
  case 25: // AA
    return 1;
  default:
    return 0;
  }
}
//const char *contest_names[N_CONTEST+1] = {"NOMULTI", "TAMAGAWA", "TOKYOUHF","CQWW", "Saitama-Int", "KCJ", "KantoUHF","AllJA","JA No PWR","ACAG(no multi)","KanagawaInt","Yokohama","UEC contest","Tsurumigawa","JA8(int)contest","ARRL int'l","HSWAScontest","YN contest", "ACAG(multi chk)","FD","MusashinoLine",""};
//const int contest_ids[N_CONTEST+1] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,-1};

void search_contest_id_from_name()
{
  int len;
  // search names in the database and set contest_id of the first matched entry
  len=strlen(plogw->contest_name+2);
  if (len<1) return;
  for (int i=0;i<N_CONTEST;i++) {
    if (contest_defs[i].id==-1) {
      console->print("reached end of contest list i=");
      console->print(i);
      console->print(" N_CONTEST=");
      console->print(N_CONTEST);
      break;
    }
    if (strncasecmp(contest_defs[i].name,plogw->contest_name+2,len)==0) {
      // set matched contest and set_contest_id()
      plogw->contest_id = contest_defs[i].id;
      set_contest_id();
      return;
    }
  }
  plogw->ostream->println("contest_name not found");  
}


void set_contest_id() {
  char target_name[sizeof(plogw->contest_name) - 2];
  contest_name_for_id(plogw->contest_id, target_name, sizeof(target_name));

  // Preserve any Alt-C/runtime edits before leaving the active contest.
  // CONTEST.TXT remains the persistent source for F1/F2/F3/F5/Sent EXCH.
  if (active_contest_valid && target_name[0] &&
      (active_contest.id != plogw->contest_id ||
       strcasecmp(active_contest.name, target_name) != 0)) {
    if (!save_contest_runtime_preset(active_contest.name) && plogw->ostream) {
      plogw->ostream->printf("Contest preset save failed: %s\n",
                             active_contest.name);
    }
  }

  if (plogw->contest_id == USER_MD_CONTEST_ID &&
      is_user_md_contest_name(plogw->contest_name + 2)) {
    if (start_user_md_contest(plogw->contest_name + 2)) {
      apply_contest_runtime_preset(target_name);
      note_contest_selection(plogw->contest_id, target_name);
    }
    return;
  }
  // set contest information based on contest_id referreing to the contest_defs 
  // find entry in contest_defs
  for (int i=0;i<N_CONTEST;i++) {
    if (contest_defs[i].id==-1) {
      break;
    }
    if (contest_defs[i].id==plogw->contest_id) {
      // this is the contest entry
      // set name
      strcpy(plogw->contest_name+2,contest_defs[i].name);
      
      plogw->mask=contest_defs[i].mask;
      sync_dupechk_mask_subcpu(plogw->mask);
      plogw->multi_type=contest_defs[i].multi_type;
      plogw->cw_pts=contest_defs[i].cw_pts;
      // set multi
      if (contest_defs[i].multi1!=NULL) {
	init_multi(contest_defs[i].multi1,contest_defs[i].multi1_start_band,contest_defs[i].multi1_stop_band);
      }
      if (contest_defs[i].multi2!=NULL) {
	init_multi(contest_defs[i].multi2,contest_defs[i].multi2_start_band,contest_defs[i].multi2_stop_band);
      }
      apply_contest_runtime_preset(plogw->contest_name + 2);
      note_contest_selection(plogw->contest_id, plogw->contest_name + 2);
      upd_display_info_contest_settings(so2r.radio_selected());
      if (!suppress_contest_history) request_makedupe_rebuild();
      return;
    }
  }
  plogw->contest_id=0;
  plogw->ostream->println("contest_id not found -> NOMULTI");
}

bool previous_contest_info(int *id, char *name, size_t name_size) {
  if (!previous_contest_valid) return false;
  if (id) *id = previous_contest.id;
  if (name && name_size) strlcpy(name, previous_contest.name, name_size);
  return true;
}

bool previous_contest_sent_exch(char *out, size_t out_size) {
  if (out && out_size) out[0] = '\0';
  if (!previous_contest_valid || !out || out_size == 0) return false;
  return get_contest_runtime_sent_exch(previous_contest.name, out, out_size);
}

int previous_contest_multi_check(const char *exch, int bandid) {
  if (!previous_contest_valid || exch == NULL || *exch == '\0') return -1;

  if (is_user_md_contest_name(previous_contest.name))
    return user_md_multi_check_for(previous_contest.name, exch, bandid);

  const contest_definition *def = NULL;
  for (int i = 0; i < N_CONTEST && contest_defs[i].id != -1; ++i) {
    if (contest_defs[i].id == previous_contest.id) {
      def = &contest_defs[i];
      break;
    }
  }

  // User-defined MD contests need their loaded runtime table; do not disturb
  // the active contest to load another one during a RUN QSO.
  if (def == NULL) return -1;

  // multi_list is a little over 3 kB with the current N_BAND/N_MULTI values.
  // Do not copy it onto the Arduino main-task stack. Normal operation often
  // leaves less than 1 kB of stack headroom. Prefer PSRAM, then ordinary heap.
  if (previous_multi_saved == NULL || previous_multi_cached == NULL) {
    uint32_t caps = f_spiram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                             : MALLOC_CAP_8BIT;
    if (previous_multi_saved == NULL)
      previous_multi_saved = (struct multi_list *)heap_caps_malloc(
          sizeof(*previous_multi_saved), caps);
    if (previous_multi_cached == NULL)
      previous_multi_cached = (struct multi_list *)heap_caps_malloc(
          sizeof(*previous_multi_cached), caps);
  }
  if (previous_multi_saved == NULL || previous_multi_cached == NULL) {
    if (plogw->ostream)
      plogw->ostream->println(
          "DUAL MULTI: cannot allocate previous-contest workspace");
    return -1;
  }

  memcpy(previous_multi_saved, &multi_list, sizeof(*previous_multi_saved));
  const int saved_multi_type = plogw->multi_type;
  const int saved_contest_id = plogw->contest_id;

  if (previous_multi_cached_id != def->id) {
    memset(&multi_list, 0, sizeof(multi_list));
    plogw->contest_id = def->id;
    plogw->multi_type = def->multi_type;
    if (def->multi1 != NULL)
      init_multi(def->multi1, def->multi1_start_band, def->multi1_stop_band);
    if (def->multi2 != NULL)
      init_multi(def->multi2, def->multi2_start_band, def->multi2_stop_band);
    memcpy(previous_multi_cached, &multi_list, sizeof(*previous_multi_cached));
    previous_multi_cached_id = def->id;
  } else {
    memcpy(&multi_list, previous_multi_cached, sizeof(multi_list));
    plogw->contest_id = def->id;
    plogw->multi_type = def->multi_type;
  }

  char tmp[LEN_EXCH + 1];
  strlcpy(tmp, exch, sizeof(tmp));
  const int result = multi_check(tmp, bandid);

  memcpy(&multi_list, previous_multi_saved, sizeof(*previous_multi_saved));
  plogw->multi_type = saved_multi_type;
  plogw->contest_id = saved_contest_id;

  return result;
}

void contest_stats_begin_rebuild() {
  memset(contest_stats, 0, sizeof(contest_stats));
  if (active_contest_valid) contest_stats_get(current_contest_dupe_id_impl());
  if (previous_contest_valid) {
    const uint8_t previous_id = previous_contest_dupe_id();
    if (contest_stats_find(previous_id) < 0) contest_stats_get(previous_id);
  }
}

void contest_stats_record_rebuild(uint8_t contest_id, unsigned char bandmode,
                                  const char *recv_exch) {
  const uint8_t active_id = current_contest_dupe_id_impl();
  const bool is_active = contest_id == active_id;
  const bool is_previous = previous_contest_valid &&
                           contest_id == previous_contest_dupe_id();
  // The shared DUPE pool may contain older contests too, but MAIN keeps score
  // snapshots only for the active/previous pair.
  if (!is_active && !is_previous) return;
  const int bandid = bandmode / 4;
  const int modetype = bandmode % 4;
  if (bandid < 1 || bandid >= N_BAND) return;

  int slot = contest_stats_get(contest_id);
  contest_stats[slot].score_data.worked[
      modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;

  int multi = -1;
  char tmp[LEN_EXCH + 1];
  strlcpy(tmp, recv_exch ? recv_exch : "", sizeof(tmp));
  if (is_active)
    multi = multi_check(tmp, bandid);
  else if (is_previous)
    multi = previous_contest_multi_check(tmp, bandid);

  if (multi >= 0 && multi < N_MULTI) {
    uint8_t *cell = &contest_stats[slot].multi_worked_bits[bandid - 1][multi >> 3];
    const uint8_t mask = (uint8_t)(1U << (multi & 7));
    if ((*cell & mask) == 0) contest_stats[slot].score_data.nmulti[bandid - 1]++;
    *cell |= mask;
  }
}

void contest_stats_finish_rebuild(bool valid) {
  if (!valid) {
    memset(contest_stats, 0, sizeof(contest_stats));
    init_score();
    clear_multi_worked();
    return;
  }
  for (int i = 0; i < 2; ++i)
    if (contest_stats[i].valid) contest_stats[i].rebuilt = true;
  contest_stats_restore_current();
}

void contest_stats_capture_current() {
  if (!active_contest_valid) return;
  const uint8_t id = current_contest_dupe_id_impl();
  int slot = contest_stats_get(id);
  contest_stats[slot].score_data = score;
  memcpy(contest_stats[slot].multi_worked_bits, multi_list.multi_worked_bits,
         sizeof(contest_stats[slot].multi_worked_bits));
  // Do not change rebuilt here.  This function is also called after each live
  // QSO; a partially accumulated secondary-contest slot is not a MAKEDUPE
  // reconstruction.
}

void contest_stats_capture_rebuilt_current() {
  contest_stats_capture_current();
  if (!active_contest_valid) return;
  int slot = contest_stats_find(current_contest_dupe_id_impl());
  if (slot >= 0) contest_stats[slot].rebuilt = true;
}

bool contest_stats_restore_current() {
  if (!active_contest_valid) return false;
  int slot = contest_stats_find(current_contest_dupe_id_impl());
  if (slot < 0) {
    // A contest not rebuilt yet starts with clean statistics. MAKEDUPE will
    // populate and capture it; the other slot remains intact.
    init_score();
    memset(multi_list.multi_worked_bits, 0, sizeof(multi_list.multi_worked_bits));
    return false;
  }
  score = contest_stats[slot].score_data;
  memcpy(multi_list.multi_worked_bits, contest_stats[slot].multi_worked_bits,
         sizeof(multi_list.multi_worked_bits));
  return contest_stats[slot].rebuilt;
}

void contest_stats_record_secondary(uint8_t contest_id, int modetype, int bandid,
                                    int multi, bool is_dupe) {
  if (bandid < 1 || bandid >= N_BAND) return;
  int slot = contest_stats_get(contest_id);
  if (!is_dupe)
    contest_stats[slot].score_data.worked[
        modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;
  if (!is_dupe && multi >= 0 && multi < N_MULTI) {
    uint8_t *cell = &contest_stats[slot].multi_worked_bits[bandid - 1][multi >> 3];
    uint8_t mask = (uint8_t)(1U << (multi & 7));
    if ((*cell & mask) == 0) contest_stats[slot].score_data.nmulti[bandid - 1]++;
    *cell |= mask;
  }
}

bool alternate_contest() {
  plogw->ostream->printf(
			 "alternate_contest: active=%d previous=%d current=%d %s\n",
			 active_contest_valid,
			 previous_contest_valid,
			 plogw->contest_id,
			 plogw->contest_name + 2);
    
  if (user_md_contest_loading()) {
    upd_display_info_flash("Contest switch\nUser MD is loading");
    info_disp.timer = 2000;
    return false;
  }
  if (!active_contest_valid || !previous_contest_valid) {
    upd_display_info_flash("Contest alternate\nNo previous contest");
    info_disp.timer = 2000;
    return false;
  }

  const contest_selection old_active = active_contest;
  const contest_selection target = previous_contest;
  contest_stats_capture_current();

  suppress_contest_history = true;
  plogw->contest_id = target.id;
  strlcpy(plogw->contest_name + 2, target.name,
          sizeof(plogw->contest_name) - 2);
  set_contest_id();
  suppress_contest_history = false;

  if (!same_contest(active_contest, target)) {
    return false;
  }
  previous_contest = old_active;
  previous_contest_valid = true;
  if (explicit_contest_pair)
    contest_entry_set_pair(active_contest.name, previous_contest.name);
  const bool target_stats_rebuilt = contest_stats_restore_current();
  if (!target_stats_rebuilt) {
    // A slot may already contain QSOs accumulated while this contest was the
    // secondary contest.  Those are only partial statistics; always rebuild
    // from QSO.TXT the first time the contest becomes active.
    request_makedupe_rebuild();
  }
  save_contest_history();

  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "Contest switched\n%s", active_contest.name);
  upd_display_info_flash(dp->lcdbuf);
  info_disp.timer = 2000;
  plogw->ostream->printf("Contest alternate: %s\n", active_contest.name);
  return true;
}
