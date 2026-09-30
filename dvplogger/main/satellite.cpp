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
#include "SD.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "satellite.h"
#include "cat.h"
#include "settings.h"
#include "display.h"
#include "so2r.h"
#include "log.h"
#include <HTTPClient.h>
#include <AsyncTCP.h>
#include "esp_heap_caps.h"
#include "Plan13.h"
#include "timekeep.h"
HTTPClient http;
#include <maidenhead.h>
/// sattracking based on https://www.amsat.org/amsat/articles/g3ruh/111.html

char *tlefilename = "/tle.txt";
File tlefile;

// TLE parsing is deliberately separated from HTTP download.  Web/AsyncTCP
// responses can still own several KB immediately after a request completes;
// starting orbit/TLE work in that window caused very low heap watermarks.
static volatile bool sat_tle_parse_pending = false;
static uint32_t sat_tle_parse_not_before_ms = 0;
static uint32_t sat_tle_parse_wait_log_ms = 0;

static bool sat_tle_parse_heap_ready() {
  const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t async_q = asyncTCPQueueMessagesWaiting();

  // These are admission thresholds, not HW1-specific limits.  On larger
  // hardware they are naturally satisfied immediately.  On a busy small
  // heap they serialize TLE parsing behind outstanding Web traffic.
  return free_now >= 28672 && largest >= 16384 && async_q == 0;
}

void request_sat_tle_parse(uint32_t delay_ms) {
  const uint32_t requested = millis() + delay_ms;
  // Coalesce all callers into one parse job.  Never postpone an already
  // scheduled earlier parse merely because another caller asks for one.
  if (!sat_tle_parse_pending || (int32_t)(requested - sat_tle_parse_not_before_ms) < 0)
    sat_tle_parse_not_before_ms = requested;
  sat_tle_parse_pending = true;
}

// index of sat_info[] sorted by  satellite aos
int satidx_sort[N_SATELLITES];

static const char *satdbfilename = "/satdb.txt";
extern const struct sat_info2 sat_info2[N_SATELLITES];

// offset_freq is only the small transponder alignment correction shown as
// Ofs:.  It must never move the configured downlink passband into another RF
// band.  This also detects the 289-MHz corruption that can result when a
// wrong-band MAIN/SUB snapshot is accidentally used as a manual re-anchor.
static int sat_hz_band(int hz) {
  if (hz <= 0) return 0;
  return freq2bandid((unsigned int)(hz / FREQ_UNIT));
}

static int sat_default_offset_hz(const char *name) {
  if (!name) return 0;
  for (int j = 0; j < N_SATELLITES; ++j) {
    if (sat_info2[j].name[0] == '\0') break;
    if (strcmp(sat_info2[j].name, name) == 0)
      return (int)(sat_info2[j].offset_freq * 1000.0);
  }
  return 0;
}

static bool sat_offset_keeps_configured_band(int i, int offset_hz) {
  if (i < 0 || i >= N_SATELLITES) return false;
  const int b0 = sat_hz_band(sat_info[i].dn_f0);
  const int b1 = sat_hz_band(sat_info[i].dn_f1);
  if (b0 == 0 || b1 == 0 || b0 != b1) return false;
  return sat_hz_band(sat_info[i].dn_f0 + offset_hz) == b0 &&
         sat_hz_band(sat_info[i].dn_f1 + offset_hz) == b1;
}

static void sat_repair_corrupt_offset_if_needed(int i) {
  if (i < 0 || i >= N_SATELLITES) return;
  // Satellites such as AO-27 may have TLE/AOS information but no configured
  // linear transponder in sat_info[].  offset_freq=0 is valid there; there is
  // no downlink passband against which a corruption guard can be evaluated.
  if (sat_info[i].dn_f0 <= 0 || sat_info[i].dn_f1 <= 0) return;
  if (sat_offset_keeps_configured_band(i, sat_info[i].offset_freq)) return;
  const int bad = sat_info[i].offset_freq;
  const int def = sat_default_offset_hz(sat_info[i].name);
  sat_info[i].offset_freq = def;
  plogw->ostream->printf("SAT OFS CORRUPT %s %d -> %d (default; wrong RF band)\n",
                        sat_info[i].name, bad, def);
}

static int first_empty_sat_slot() {
  for (int i = 0; i < N_SATELLITES; ++i) {
    if (sat_info[i].name[0] == '\0') return i;
  }
  return -1;
}

static void set_sat_definition(int i, const char *name,
                               int up0, int up1, const char *upmode,
                               int dn0, int dn1, const char *dnmode,
                               int beacon, int offset) {
  if (i < 0 || i >= N_SATELLITES) return;
  strlcpy(sat_info[i].name, name ? name : "", sizeof(sat_info[i].name));
  sat_info[i].up_f0 = up0;
  sat_info[i].up_f1 = up1;
  sat_info[i].dn_f0 = dn0;
  sat_info[i].dn_f1 = dn1;
  strlcpy(sat_info[i].up_mode, upmode ? upmode : "", sizeof(sat_info[i].up_mode));
  strlcpy(sat_info[i].dn_mode, dnmode ? dnmode : "", sizeof(sat_info[i].dn_mode));
  sat_info[i].bc_f0 = beacon;
  sat_info[i].offset_freq = offset;
}

static void load_default_satellite_database() {
  // Preserve the historical satellite-name list, including satellites that
  // have TLE/AOS data but no configured transponder.
  for (int i = 0; i < N_SATELLITES; ++i) {
    if (sat_names[i] == nullptr || sat_names[i][0] == '\0') break;
    set_sat_definition(i, sat_names[i], 0, 0, "", 0, 0, "", 0, 0);
  }

  // Overlay the built-in transponder defaults.
  for (int j = 0; j < N_SATELLITES; ++j) {
    if (sat_info2[j].name[0] == '\0') break;
    int i = find_satname((char *)sat_info2[j].name);
    if (i < 0) {
      i = first_empty_sat_slot();
      if (i < 0) break;
    }
    set_sat_definition(i, sat_info2[j].name,
                       (int)(sat_info2[j].up_f0 * 1000000.0),
                       (int)(sat_info2[j].up_f1 * 1000000.0),
                       sat_info2[j].up_mode,
                       (int)(sat_info2[j].dn_f0 * 1000000.0),
                       (int)(sat_info2[j].dn_f1 * 1000000.0),
                       sat_info2[j].dn_mode,
                       (int)(sat_info2[j].bc_f0 * 1000000.0),
                       (int)(sat_info2[j].offset_freq * 1000.0));
  }

  // Legacy compatibility: older firmware stored only per-satellite frequency
  // offsets in /satinfo.txt ("name offset").  /satdb.txt supersedes it and
  // stores the complete satellite/transponder definition.  Check existence
  // before FILE_READ so a missing legacy file does not produce a VFS error.
  if (SD.exists("/satinfo.txt")) {
    File legacy = SD.open("/satinfo.txt", FILE_READ);
    if (legacy) {
      while (readline(&legacy, buf, 0x0d0a, 128) != 0) {
        char *name = strtok(buf, " ");
        char *ofs = strtok(NULL, " ");
        if (!name || !ofs) continue;
        int i = find_satname(name);
        if (i >= 0) sat_info[i].offset_freq = atoi(ofs);
      }
      legacy.close();
    }
  }
}

void load_satinfo() {
  // Always start from the firmware defaults, then overlay /satdb.txt.
  // This keeps newly-added built-in satellites available even when an older
  // user database already exists on the SD card.
  load_default_satellite_database();

  File db = SD.open(satdbfilename, FILE_READ);
  if (!db) {
    plogw->ostream->println("Creating default satellite database /satdb.txt");
    save_satinfo();
    return;
  }

  // Format:
  // name<TAB>up0_hz<TAB>up1_hz<TAB>up_mode<TAB>dn0_hz<TAB>dn1_hz
  // <TAB>dn_mode<TAB>beacon_hz<TAB>offset_hz
  while (readline(&db, buf, 0x0d0a, 192) != 0) {
    char *saveptr = nullptr;
    char *name = strtok_r(buf, "\t", &saveptr);
    char *up0s = strtok_r(nullptr, "\t", &saveptr);
    char *up1s = strtok_r(nullptr, "\t", &saveptr);
    char *upmode = strtok_r(nullptr, "\t", &saveptr);
    char *dn0s = strtok_r(nullptr, "\t", &saveptr);
    char *dn1s = strtok_r(nullptr, "\t", &saveptr);
    char *dnmode = strtok_r(nullptr, "\t", &saveptr);
    char *bcs = strtok_r(nullptr, "\t", &saveptr);
    char *ofss = strtok_r(nullptr, "\t", &saveptr);
    if (!name || !up0s || !up1s || !upmode || !dn0s || !dn1s ||
        !dnmode || !bcs || !ofss) continue;

    int i = find_satname(name);
    if (i < 0) i = first_empty_sat_slot();
    if (i < 0) break;

    set_sat_definition(i, name,
                       atoi(up0s), atoi(up1s), upmode,
                       atoi(dn0s), atoi(dn1s), dnmode,
                       atoi(bcs), atoi(ofss));
  }
  db.close();
}



// name up_low up_high  up_mode down_low down_high down_mode beacon(MHz) offset(kHz)
// 複数のトラポンが搭載されている衛星は "sat_name,L" のように記述することでトラポン周波数の設定を区別する。
// 軌道情報(sat_info[])は、複数の同じsat_name のエントリーに記録することが必要となる。
// 決まりを決めただけでまだ実装していない FO-118 はとりあえずリニアトラポンだけ記述 23/1/3
const struct sat_info2 sat_info2[N_SATELLITES] = {
  { "FO-29", 145.900, 146.000, "LSB", 435.800, 435.900, "USB", 435.795, 2.4 },
  {"AO-73", 435.130, 435.150, "LSB", 145.950, 145.970, "USB", 0.0, 1.5},
  { "AO-07", 145.850, 145.950, "USB", 29.400, 29.500, "USB", 29.502, 0.0 },
  { "XW-2A", 435.030, 435.050, "LSB", 145.665, 145.685, "USB", 145.660, -1.3 },
  {"XW-2B", 435.090, 435.110, "LSB", 145.730, 145.750, "USB", 145.725, 0.0},
  {"XW-2C", 435.150, 435.170, "LSB", 145.795, 145.815, "USB", 145.790, 0.0},
  {"XW-2D", 435.210, 435.230, "LSB", 145.860, 145.880, "USB", 145.855, 0.0},
  // {"XW-2F", 435.330, 435.350, "LSB", 145.980, 146.000, "USB", 145.975, 0.0},
  {"MESAT1", 145.910, 145.940, "LSB", 435.810, 435.840, "USB", 435.800, 0.0},  
  { "RS-44", 145.935, 145.995, "LSB", 435.610, 435.670, "USB", 435.605, -0.45 },
  { "EO-88", 435.015, 435.045, "LSB", 145.960, 145.990, "USB", 0.0, 0.1 },
  { "CAS-4A", 435.210, 435.230, "LSB", 145.860, 145.880, "USB", 145.855, -1.1 },
  { "CAS-4B", 435.270, 435.290, "LSB", 145.915, 145.935, "USB", 145.910, -1.5 },
  { "JO-97", 435.100, 435.120, "LSB", 145.855, 145.875, "USB", 145.840, -1.8 },
  { "FO-99", 145.900, 145.930, "LSB", 435.880, 435.910, "USB", 437.075, -1.0 },
  { "HO-113", 145.855, 145.885, "LSB", 435.195, 435.165, "USB", 435.575, 0.8 },
  { "ISS", 145.990, 145.990, "FM", 437.800, 437.800, "FM", 0.0, 0.0 },
  { "IO-117", 435.300, 435.320, "USB", 435.300, 435.320, "USB", 0, 0.0 },
  { "FO-118", 145.805, 145.835, "LSB", 435.525, 435.555, "USB", 435.570, 0.0 },
  { "CAS-10", 145.855, 145.885, "LSB", 435.195, 435.165, "USB", 435.575, 0.0 },
  { "", 0.0, 0.0, "", 0.0, 0.0, "", 0.0, 0.0 }  // end
};

void set_sat_index(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {
    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println("Not found");
    }
    return;
  } else {
    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println(plogw->sat_idx_selected);
    }
    plogw->sat_idx_selected = i;
  }
}


// adjust
void adjust_sat_offset(int dfreq) {
  if (plogw->sat) {
    int i;
    i = plogw->sat_idx_selected;
    if (i < 0) return;
    sat_info[i].offset_freq += dfreq;
    if (!plogw->f_console_emu) {
      plogw->ostream->print("Sat freq offset=");
      plogw->ostream->println(sat_info[i].offset_freq);
    }
    save_satinfo();
    set_sat_freq_calc();
  }
}

void set_sat_info2(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {

    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println("Not found");
    }
    return;
  }

  for (int j = 0; j < N_SATELLITES; j++) {
    if (sat_info2[j].name[0] == '\0') break;
    if (strcmp(sat_info2[j].name, sat_name) == 0) {
      // set j th of sat_info2
      sat_info[i].up_f0 = int(sat_info2[j].up_f0 * 1000000.);
      sat_info[i].up_f1 = int(sat_info2[j].up_f1 * 1000000.);
      sat_info[i].dn_f0 = int(sat_info2[j].dn_f0 * 1000000.);
      sat_info[i].dn_f1 = int(sat_info2[j].dn_f1 * 1000000.);
      sat_info[i].bc_f0 = int(sat_info2[j].bc_f0 * 1000000.);
      sat_info[i].offset_freq = int(sat_info2[j].offset_freq * 1000.);
      strcpy(sat_info[i].up_mode, sat_info2[j].up_mode);
      strcpy(sat_info[i].dn_mode, sat_info2[j].dn_mode);

      return;
    }
  }
}

void set_sat_info_calc() {
  int i;
  i = plogw->sat_idx_selected;
  if (i < 0) return;

  if (verbose & 8) {
    plogw->ostream->print("set_sat_info_calc():");
    plogw->ostream->println(plogw->sat_name_set);
  }
  p13.setElements(
    sat_info[i].YEAR,
    sat_info[i].EPOCH,
    sat_info[i].INCLINATION,
    sat_info[i].RAAN,
    sat_info[i].ECCENTRICITY * ONEPPTM,
    sat_info[i].ARGUMENT_PEDIGREE,
    sat_info[i].MEAN_ANOMALY,
    sat_info[i].MEAN_MOTION,
    sat_info[i].TIME_MOTION_D,
    sat_info[i].EPOCH_ORBIT,
    180);
}



#define TX 1
#define RX 0


static bool sat_rig_name_starts_with(const struct radio *radio, const char *prefix) {
  if (!radio || !radio->rig_spec || !prefix) return false;
  return strncmp(radio->rig_spec->name, prefix, strlen(prefix)) == 0;
}

static bool sat_freq_in_range(unsigned int f, int f0, int f1) {
  if (f == 0 || f0 <= 0 || f1 <= 0) return false;
  const unsigned int lo = (unsigned int)(f0 < f1 ? f0 : f1);
  const unsigned int hi = (unsigned int)(f0 < f1 ? f1 : f0);
  const unsigned int margin = 25000U;
  return (f + margin >= lo) && (f <= hi + margin);
}

static void sat_auto_reason(char *reason, size_t reason_len, const char *text) {
  if (!reason || reason_len == 0) return;
  strlcpy(reason, text ? text : "", reason_len);
}

static bool sat_radio0_is_ic9700() {
  const struct radio *r0 = &radio_list[0];
  return r0->enabled &&
         (sat_rig_name_starts_with(r0, "IC-9700") ||
          (r0->rig_spec && r0->rig_spec->rig_type == RIG_TYPE_ICOM_IC9700));
}

void sat_frequency_monitor_reset(uint32_t settle_ms);
static void sat_frequency_sequencer_cancel_writes();

bool sat_apply_vfo_mode_to_rig() {
  if (!plogw->sat || !sat_radio0_is_ic9700()) return false;
  struct radio *radio = &radio_list[0];
  if (plogw->sat_vfo_mode != SAT_VFO_SINGLE_A_RX &&
      plogw->sat_vfo_mode != SAT_VFO_SINGLE_A_TX) return false;

  const bool ratb = (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX);

  // A layout change invalidates any queued/in-flight SAT frequency write.
  // New targets will be queued by the following Doppler/CENTER/BEACON update.
  sat_frequency_sequencer_cancel_writes();

  // These two IC-9700 modes are mutually exclusive in practice.  Do not
  // merely request SAT=ON while Dualwatch is still active: first turn the
  // old layout off, then enable the requested layout.
  if (ratb) {
    // R_A_T_B: native Satellite mode => MAIN=RX/downlink, SUB=TX/uplink.
    send_head_civ(radio);
    add_civ_buf((byte)0x16); add_civ_buf((byte)0x59); add_civ_buf((byte)0x00);
    send_tail_civ(radio);
    send_head_civ(radio);
    add_civ_buf((byte)0x16); add_civ_buf((byte)0x5a); add_civ_buf((byte)0x01);
    send_tail_civ(radio);
  } else {
    // R_B_T_A: normal Dualwatch => MAIN=TX/uplink, SUB=RX/downlink.
    send_head_civ(radio);
    add_civ_buf((byte)0x16); add_civ_buf((byte)0x5a); add_civ_buf((byte)0x00);
    send_tail_civ(radio);
    send_head_civ(radio);
    add_civ_buf((byte)0x16); add_civ_buf((byte)0x59); add_civ_buf((byte)0x01);
    send_tail_civ(radio);
  }

  // Do not try to assign RF bands here.  In IC-9700 Satellite mode the
  // operating sides are selected directly: MAIN=RX/downlink, SUB=TX/uplink.
  // Frequency writes use 07 D0/D1 followed by 05 in set_vfo_frequency_rig().

  // Leave the front-panel operating selection on MAIN after changing layout.
  send_head_civ(radio);
  add_civ_buf((byte)0x07); add_civ_buf((byte)0xd0);
  send_tail_civ(radio);

  // Abort an in-flight satellite read before changing frequencies.
  // CENTER/BEACON write both sides immediately; no delayed write queue.
  sat_frequency_monitor_reset(0U);

  if (verbose & 8) {
    plogw->ostream->println(ratb
      ? "SAT IC9700 APPLY RATB: SAT mode MAIN=RX/downlink SUB=TX/uplink"
      : "SAT IC9700 APPLY RBTA: DUAL mode MAIN=TX/uplink SUB=RX/downlink");
  }
  return true;
}

void sat_vfo_mode_label(char *buf, size_t len) {
  if (!buf || len == 0) return;
  if (sat_radio0_is_ic9700()) {
    if (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX) {
      strlcpy(buf, "IC-9700 SAT: RX=MAIN / TX=SUB (R_A_T_B)", len);
      return;
    }
    if (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX) {
      strlcpy(buf, "IC-9700 DUAL: RX=SUB / TX=MAIN (R_B_T_A)", len);
      return;
    }
  }
  const char *text = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX ? "Single: TX=A (R_B_T_A)" :
                     plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ? "Single: RX=A (R_A_T_B)" :
                     plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0 ? "TX=Radio0 / RX=Radio1" :
                     plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_1 ? "TX=Radio1 / RX=Radio0" : "?";
  strlcpy(buf, text, len);
}

uint32_t sat_current_radio_mask() {
  if (!plogw->sat) return 0;

  // Derive this from the CURRENT VFO configuration every time.  Do not cache
  // the mask when SAT mode starts: Alt-R/Web/auto selection may change the
  // TX/RX radio assignment while the satellite session remains active.
  switch (plogw->sat_vfo_mode) {
    case SAT_VFO_SINGLE_A_TX:
    case SAT_VFO_SINGLE_A_RX:
      return (1UL << 0);
    case SAT_VFO_MULTI_TX_0:
    case SAT_VFO_MULTI_TX_1:
      return (1UL << 0) | (1UL << 1);
    default:
      return 0;
  }
}

void sat_vfo_mode_label_compact(char *buf, size_t len) {
  if (!buf || len == 0) return;
  if (sat_radio0_is_ic9700()) {
    if (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX) {
      strlcpy(buf, "S:M/S", len);  // Satellite: RX=MAIN / TX=SUB
      return;
    }
    if (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX) {
      strlcpy(buf, "D:S/M", len);  // Dual: RX=SUB / TX=MAIN
      return;
    }
  }
  const char *text = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ? "RATB" :
                     plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX ? "RBTA" :
                     plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0 ? "R1T0" :
                     plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_1 ? "R0T1" : "?";
  strlcpy(buf, text, len);
}

int auto_select_sat_vfo_mode(char *reason, size_t reason_len) {
  sat_auto_reason(reason, reason_len, "No change");

  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES || sat_info[i].name[0] == '\0') {
    sat_auto_reason(reason, reason_len, "No satellite selected");
    return -1;
  }

  struct radio *r0 = &radio_list[0];
  struct radio *r1 = &radio_list[1];
  const bool e0 = r0->enabled;
  const bool e1 = r1->enabled;
  const bool ic705_0 = e0 && sat_rig_name_starts_with(r0, "IC-705");
  const bool ic705_1 = e1 && sat_rig_name_starts_with(r1, "IC-705");
  const bool ic9700_0 = e0 && sat_rig_name_starts_with(r0, "IC-9700");

  if (e0 && !e1 &&
      (ic9700_0 || (r0->rig_spec && r0->rig_spec->rig_type == RIG_TYPE_ICOM_IC9700))) {
    // IC-9700 Satellite mode has fixed roles: MAIN=RX/downlink, SUB=TX/uplink.
    // Represent that as R_A_T_B (SAT_VFO_SINGLE_A_RX).  R_B_T_A remains the
    // normal dual-receive arrangement (MAIN/A=TX, SUB/B=RX).
    plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
    so2r.change_focused_radio(0);
    sat_auto_reason(reason, reason_len, "Single IC-9700 SAT: MAIN=RX/downlink, SUB=TX/uplink");
    return plogw->sat_vfo_mode;
  }

  if (e0 && !e1 &&
      (ic705_0 || (r0->rig_spec && r0->rig_spec->rig_type == RIG_TYPE_ICOM_IC705))) {
    plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
    so2r.change_focused_radio(0);
    sat_auto_reason(reason, reason_len, "Single IC-705: selected/VFO A=RX, other VFO=TX");
    return plogw->sat_vfo_mode;
  }

  if (e0 && e1) {
    const bool r0_up = sat_freq_in_range(r0->freq, sat_info[i].up_f0, sat_info[i].up_f1);
    const bool r0_dn = sat_freq_in_range(r0->freq, sat_info[i].dn_f0, sat_info[i].dn_f1);
    const bool r1_up = sat_freq_in_range(r1->freq, sat_info[i].up_f0, sat_info[i].up_f1);
    const bool r1_dn = sat_freq_in_range(r1->freq, sat_info[i].dn_f0, sat_info[i].dn_f1);

    if (r0_up && r1_dn && !(r0_dn && r1_up)) {
      plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_0;
      so2r.change_focused_radio(0);
      sat_auto_reason(reason, reason_len, "2TRX: Radio0 frequency=uplink, Radio1=downlink");
      return plogw->sat_vfo_mode;
    }
    if (r1_up && r0_dn && !(r1_dn && r0_up)) {
      plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_1;
      so2r.change_focused_radio(1);
      sat_auto_reason(reason, reason_len, "2TRX: Radio1 frequency=uplink, Radio0=downlink");
      return plogw->sat_vfo_mode;
    }

    const int up_band = freq2bandid(sat_info[i].up_f0);
    const int dn_band = freq2bandid(sat_info[i].dn_f0);
    const bool band0_up = up_band > 0 && r0->bandid == up_band;
    const bool band1_up = up_band > 0 && r1->bandid == up_band;
    const bool band0_dn = dn_band > 0 && r0->bandid == dn_band;
    const bool band1_dn = dn_band > 0 && r1->bandid == dn_band;
    const bool dual705 = ic705_0 && ic705_1;

    if (band0_up && band1_dn) {
      plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_0;
      so2r.change_focused_radio(0);
      sat_auto_reason(reason, reason_len,
                      dual705 ? "Two IC-705s: Radio0 uplink / Radio1 downlink by band"
                              : "2TRX: Radio0 uplink / Radio1 downlink by band");
      return plogw->sat_vfo_mode;
    }
    if (band1_up && band0_dn) {
      plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_1;
      so2r.change_focused_radio(1);
      sat_auto_reason(reason, reason_len,
                      dual705 ? "Two IC-705s: Radio1 uplink / Radio0 downlink by band"
                              : "2TRX: Radio1 uplink / Radio0 downlink by band");
      return plogw->sat_vfo_mode;
    }

    if (dual705) {
      sat_auto_reason(reason, reason_len,
                      "Two IC-705s detected; TX/RX direction ambiguous, mode unchanged");
      return -1;
    }

    sat_auto_reason(reason, reason_len,
                    "Two rigs active; frequencies/bands do not identify TX/RX, mode unchanged");
    return -1;
  }

  if (e0 && !e1) {
    plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
    so2r.change_focused_radio(0);
    sat_auto_reason(reason, reason_len, "Single generic rig: VFO A=RX fallback");
    return plogw->sat_vfo_mode;
  }

  if (!e0 && e1) {
    sat_auto_reason(reason, reason_len,
                    "Only Radio1 active; current single-TRX modes require Radio0");
    return -1;
  }

  sat_auto_reason(reason, reason_len, "No active rig");
  return -1;
}

// Satellite CAT frequency monitor -------------------------------------------------
// A frequency report must be classified as RX/downlink or TX/uplink before it
// can be used as a tuning input.  IC-9700 Satellite mode needs an explicit
// MAIN/SUB select before command 03, so remember which side the outstanding
// query belongs to.  Other rigs use the same re-anchor routine once their side
// is known from sat_vfo_mode/radio index.
static int sat_query_side = -1;
static struct radio *sat_query_radio = nullptr;
static uint32_t sat_query_started_ms = 0;
static uint8_t sat_monitor_phase = 0;
static uint32_t sat_monitor_next_ms = 0;
// IC-9700 read transaction: 07 D0/D1 must be allowed to take effect before
// command 03.  0=idle, 1=waiting after VFO select, 2=waiting for 03 response.
static uint8_t sat_query_stage = 0;
static uint32_t sat_query_action_ms = 0;
static const uint32_t SAT_IC9700_SELECT_SETTLE_MS = 40U;
static const int SAT_DIAL_MAX_JUMP_HZ = 5000000;

// IC-9700 may report a frequency a few Hz away from the value just written
// because a Doppler update and MAIN/SUB readback are not atomic.  Keep a
// short-lived record of our own writes so those readbacks are never mistaken
// for operator tuning.
static const int SAT_DIAL_DEADBAND_HZ = 20;
static const uint32_t SAT_OWN_WRITE_GUARD_MS = 1500U;
static int sat_own_write_hz[2] = {0, 0};
static uint32_t sat_own_write_ms[2] = {0, 0};

// IC-9700 operator tuning is classified from one complete one-second
// MAIN/SUB snapshot.  This allows RX-only, TX-only and linked RX+TX tuning
// without treating the two reports as two independent dial operations.
static int sat_snapshot_prev_hz[2] = {0, 0};
static bool sat_snapshot_prev_valid = false;
static int sat_snapshot_hz[2] = {0, 0};
static bool sat_snapshot_seen[2] = {false, false};

// Native IC-9700 SATELLITE mode manual-dial settling.  Once either side is
// observed moving, DVPlogger stops all frequency SETs and only samples the
// complete MAIN/SUB pair.  Commit after the same valid pair has been observed
// for two consecutive follow-up snapshots.  This prevents our companion/
// Doppler writes from fighting the front-panel dial while it is still moving.
static bool sat_dial_settling = false;
static int sat_dial_start_hz[2] = {0, 0};
static int sat_dial_last_hz[2] = {0, 0};
static bool sat_dial_moved[2] = {false, false};
static uint8_t sat_dial_stable_count = 0;
static const uint8_t SAT_DIAL_STABLE_SNAPSHOTS = 2;

// Doppler CAT writes are event driven.  Remember the Doppler component at
// the last DVPlogger SET, not the last 500-ms calculation.  A new SET is
// needed only after the Doppler component has moved by this amount.
static const int SAT_DOPPLER_WRITE_THRESHOLD_HZ = 10;
static int sat_last_write_doppler_hz[2] = {0, 0};
static bool sat_last_write_doppler_valid[2] = {false, false};

static int sat_current_doppler_hz(int side) {
  if (side == SAT_RX)
    return (int)((double)plogw->dn_f - plogw->satdn_f);
  return (int)((double)plogw->up_f - plogw->satup_f);
}

static void sat_reanchor_doppler_write_reference() {
  sat_last_write_doppler_hz[SAT_RX] = sat_current_doppler_hz(SAT_RX);
  sat_last_write_doppler_hz[SAT_TX] = sat_current_doppler_hz(SAT_TX);
  sat_last_write_doppler_valid[SAT_RX] = true;
  sat_last_write_doppler_valid[SAT_TX] = true;
}


// SAT CAT output is serialized by sat_frequency_monitor_process(), which is
// called only from the 100-ms interval service.  The 500-ms orbit/Doppler
// calculation and UI CENTER/BEACON handlers only update these requests.
static int sat_write_pending_hz[2] = {0, 0};
static bool sat_write_pending[2] = {false, false};
static bool sat_write_force[2] = {false, false};

// IC-9700 write transaction.  MAIN/SUB selection and command 05 must never be
// interleaved with the monitor's 07 D0/D1 + 03 transaction.
// 0=idle, 1=wait after select, 2=wait after 05 before restore.
static uint8_t sat_write_stage = 0;
static int sat_write_side = -1;
static int sat_write_hz = 0;
static struct radio *sat_write_radio = nullptr;
static uint32_t sat_write_action_ms = 0;
static bool sat_ic9700_swap_pending = false;
static const uint32_t SAT_IC9700_WRITE_SETTLE_MS = 40U;

static void sat_queue_frequency_write(int side, int hz, bool force) {
  if (side != SAT_RX && side != SAT_TX) return;
  // During native SATELLITE front-panel tuning the rig owns both frequencies.
  // Do not accumulate stale Doppler/companion writes that would be emitted as
  // soon as the dial stops; the final stable pair becomes the new anchor.
  if (sat_dial_settling && plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX) {
    if (!force) {
      if (verbose & 8)
        plogw->ostream->printf("SAT DIAL settling: suppress queue %s=%d force=0\n",
                              side == SAT_RX ? "RX" : "TX", hz);
      return;
    }

    // CENTER/BEACON are explicit operator commands.  They take ownership back
    // from front-panel dial settling immediately; otherwise a force command
    // issued while settling would be silently discarded.
    sat_dial_settling = false;
    sat_dial_stable_count = 0;
    sat_write_pending[SAT_RX] = sat_write_pending[SAT_TX] = false;
    sat_write_force[SAT_RX] = sat_write_force[SAT_TX] = false;
    if (verbose & 8)
      plogw->ostream->printf("SAT DIAL settling cancelled by FORCE %s=%d\n",
                            side == SAT_RX ? "RX" : "TX", hz);
  }
  sat_write_pending_hz[side] = hz;
  sat_write_pending[side] = true;
  if (force) sat_write_force[side] = true;
  if (verbose & 8)
    plogw->ostream->printf("SAT SEQ queue %s=%d force=%d\n",
                          side == SAT_RX ? "RX" : "TX", hz, force ? 1 : 0);
}

static void sat_frequency_sequencer_cancel_writes() {
  sat_write_pending[SAT_RX] = sat_write_pending[SAT_TX] = false;
  sat_write_force[SAT_RX] = sat_write_force[SAT_TX] = false;
  sat_write_stage = 0; sat_write_side = -1; sat_write_hz = 0;
  sat_write_radio = nullptr; sat_write_action_ms = 0;
  sat_ic9700_swap_pending = false;
}

static bool sat_is_ic9700(const struct radio *radio);

// Physical IC-9700 MAIN/SUB frequencies learned from the explicit 07 D0/D1
// + 03 monitor.  Keep these separate from SAT_RX/SAT_TX: when the RF bands
// are reversed, the semantic RX/TX labels are precisely what we are trying
// to repair.  This also works for any two of the IC-9700's 144/430/1200 MHz
// bands; no V/U-only assumption is made here.
static int sat_ic9700_main_hz = 0;
static int sat_ic9700_sub_hz = 0;
static bool sat_ic9700_main_seen = false;
static bool sat_ic9700_sub_seen = false;
// Permit at most one automatic MAIN/SUB RF-band swap after startup/layout setup.
// Normal one-second tracking snapshots must never swap A/B on their own.
static bool sat_ic9700_autoswap_armed = true;

static bool sat_ic9700_supported_band(int bandid) {
  return bandid == 8 || bandid == 9 || bandid == 10; // 144, 430, 1200 MHz
}

static void sat_ic9700_ensure_band_layout(struct radio *radio) {
  if (!radio || !sat_is_ic9700(radio) || !plogw->sat ||
      !sat_ic9700_main_seen || !sat_ic9700_sub_seen) return;

  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES) return;

  const bool ratb = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
  const bool rbta = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX;
  if (!ratb && !rbta) return;

  const int dn_band = freq2bandid((unsigned int)(sat_info[i].dn_f0 / FREQ_UNIT));
  const int up_band = freq2bandid((unsigned int)(sat_info[i].up_f0 / FREQ_UNIT));
  if (!sat_ic9700_supported_band(dn_band) ||
      !sat_ic9700_supported_band(up_band) || dn_band == up_band) {
    if (verbose & 8)
      plogw->ostream->printf("SAT IC9700 BAND layout skip: target up=%d dn=%d (need two distinct 144/430/1200 bands)\n",
                            up_band, dn_band);
    return;
  }

  // Native SAT (R_A_T_B): MAIN=RX/downlink, SUB=TX/uplink.
  // DUAL (R_B_T_A):       MAIN=TX/uplink,   SUB=RX/downlink.
  const int want_main = ratb ? dn_band : up_band;
  const int want_sub  = ratb ? up_band : dn_band;
  const int have_main = freq2bandid((unsigned int)(sat_ic9700_main_hz / FREQ_UNIT));
  const int have_sub  = freq2bandid((unsigned int)(sat_ic9700_sub_hz / FREQ_UNIT));

  if (have_main == want_main && have_sub == want_sub) {
    sat_ic9700_autoswap_armed = false;
    if (verbose & 8)
      plogw->ostream->printf("SAT IC9700 BAND layout OK: MAIN band=%d SUB band=%d\n",
                            have_main, have_sub);
    return;
  }

  // 07 B0 exchanges MAIN and SUB.  Use it only when the two currently read
  // bands are exactly the desired pair in reverse order.  This is important
  // for 1200-MHz satellites too: if a third/unexpected band is present, a
  // blind swap cannot fix it and could make the state worse.
  if (have_main == want_sub && have_sub == want_main) {
    if (!sat_ic9700_autoswap_armed) {
      if (verbose & 8)
        plogw->ostream->printf("SAT IC9700 BAND layout reversed during tracking: MAIN=%d SUB=%d; auto 07 B0 suppressed\n",
                              have_main, have_sub);
      return;
    }
    sat_ic9700_autoswap_armed = false;
    if (verbose & 8)
      plogw->ostream->printf("SAT IC9700 BAND layout reversed: MAIN=%d SUB=%d -> 07 B0 -> MAIN=%d SUB=%d\n",
                            have_main, have_sub, want_main, want_sub);
    // Do not transmit from the response parser.  The 100-ms SAT
    // sequencer owns all CAT output and will issue exactly one 07 B0, then
    // invalidate the physical readback so the next monitor burst verifies it.
    sat_ic9700_swap_pending = true;
    return;
  }

  if (verbose & 8)
    plogw->ostream->printf("SAT IC9700 BAND layout unresolved: have MAIN=%d SUB=%d, want MAIN=%d SUB=%d; no blind 07 B0\n",
                          have_main, have_sub, want_main, want_sub);
}

static bool sat_is_ic9700(const struct radio *radio) {
  return radio && radio->rig_spec &&
         (radio->rig_spec->rig_type == RIG_TYPE_ICOM_IC9700 ||
          sat_rig_name_starts_with(radio, "IC-9700"));
}

static int sat_side_for_radio(const struct radio *radio) {
  if (!radio) return -1;
  switch (plogw->sat_vfo_mode) {
    case SAT_VFO_SINGLE_A_RX: return SAT_RX;
    case SAT_VFO_SINGLE_A_TX: return SAT_TX;
    case SAT_VFO_MULTI_TX_0: return radio->rig_idx == 0 ? SAT_TX : SAT_RX;
    case SAT_VFO_MULTI_TX_1: return radio->rig_idx == 1 ? SAT_TX : SAT_RX;
    default: return -1;
  }
}

static int sat_absdiff(int a, int b) {
  int d = a - b;
  return d < 0 ? -d : d;
}

static bool sat_frequency_report_is_own_or_invalid(int side, int freq_hz) {
  const int target = side == SAT_RX ? plogw->dn_f : plogw->up_f;
  const int last_cmd = side == SAT_RX ? plogw->dn_f_prev : plogw->up_f_prev;
  const int tol0 = plogw->freq_tolerance > 0 ? plogw->freq_tolerance : 1;
  const int tol = tol0 > SAT_DIAL_DEADBAND_HZ ? tol0 : SAT_DIAL_DEADBAND_HZ;
  const uint32_t now = millis();
  const bool own_recent = sat_own_write_hz[side] > 0 &&
      (uint32_t)(now - sat_own_write_ms[side]) <= SAT_OWN_WRITE_GUARD_MS &&
      sat_absdiff(freq_hz, sat_own_write_hz[side]) <= tol;
  if (own_recent) return true;
  if (target > 0 && sat_absdiff(freq_hz, target) > SAT_DIAL_MAX_JUMP_HZ) {
    if (verbose & 8)
      plogw->ostream->printf("SAT QUERY wrong-band %s %d target=%d jump=%d ignored\n",
                            side == SAT_RX ? "RX" : "TX", freq_hz, target,
                            sat_absdiff(freq_hz, target));
    return true;
  }
  (void)last_cmd;
  return false;
}

static void sat_apply_manual_snapshot(bool rx_changed, bool tx_changed,
                                      int rx_hz, int tx_hz) {
  if (!rx_changed && !tx_changed) return;
  if (plogw->sat_freq_tracking_mode == SAT_NO_TRACK) return;
  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES) return;

  // Validate the physical RF bands against the satellite definition, not
  // against plogw->up_f/dn_f.  The latter may already be corrupt and was the
  // reason a 435-MHz SUB readback could be accepted as an RS-44 uplink.
  const int want_rx_band = sat_hz_band(sat_info[i].dn_f0);
  const int want_tx_band = sat_hz_band(sat_info[i].up_f0);
  const int have_rx_band = sat_hz_band(rx_hz);
  const int have_tx_band = sat_hz_band(tx_hz);
  if (want_rx_band == 0 || want_tx_band == 0 ||
      have_rx_band != want_rx_band || have_tx_band != want_tx_band) {
    if (verbose & 8)
      plogw->ostream->printf("SAT DIAL snapshot rejected wrong-band RX=%d(b%d want b%d) TX=%d(b%d want b%d)\n",
                            rx_hz, have_rx_band, want_rx_band,
                            tx_hz, have_tx_band, want_tx_band);
    return;
  }

  sat_repair_corrupt_offset_if_needed(i);
  const double doppler_factor = (1.0 - p13.RR / 299792.0);
  if (doppler_factor == 0.0) return;

  // One common transponder offset (OLED Ofs:) is solved from the final pair.
  // For a one-sided adjustment the unchanged side is the previous snapshot;
  // for linked Satellite tuning both new values are used together.
  const double satdn_now = (double)rx_hz / doppler_factor;
  const double satup_now = (double)tx_hz * doppler_factor;
  const double new_offset_d = satdn_now + satup_now -
      (double)sat_info[i].dn_f0 - (double)sat_info[i].up_f1;
  const int old_offset = sat_info[i].offset_freq;
  const int new_offset = (int)(new_offset_d >= 0.0 ? new_offset_d + 0.5 : new_offset_d - 0.5);

  // Last line of defence: a manual re-anchor must not move the configured
  // downlink passband into another RF band.
  if (!sat_offset_keeps_configured_band(i, new_offset)) {
    if (verbose & 8)
      plogw->ostream->printf("SAT DIAL Ofs rejected %d -> %d: would cross RF band\n",
                            old_offset, new_offset);
    return;
  }

  sat_info[i].offset_freq = new_offset;
  plogw->dn_f = rx_hz;
  plogw->up_f = tx_hz;
  plogw->dn_f_prev = rx_hz;
  plogw->up_f_prev = tx_hz;
  plogw->satdn_f = satdn_now;
  plogw->satup_f = satup_now;

  // A manual tune establishes a new tracking anchor.  Reset the Doppler SET
  // reference so the next 500-ms calculation cannot immediately undo it.
  sat_reanchor_doppler_write_reference();

  if (verbose & 8)
    plogw->ostream->printf("SAT DIAL snapshot %s%s RX=%d TX=%d Ofs %d->%d\n",
                          rx_changed ? "RX" : "", tx_changed ? (rx_changed ? "+TX" : "TX") : "",
                          rx_hz, tx_hz, old_offset, new_offset);
}

static bool sat_snapshot_pair_has_expected_bands(int rx, int tx) {
  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES) return false;
  return sat_hz_band(rx) == sat_hz_band(sat_info[i].dn_f0) &&
         sat_hz_band(tx) == sat_hz_band(sat_info[i].up_f0);
}

static void sat_commit_native_dial_pair(int rx, int tx, bool rx_moved, bool tx_moved) {
  if (rx_moved != tx_moved) {
    // Only one physical side moved: this is the deliberate Ofs calibration
    // gesture.  Solve the common transponder offset from the final stable pair.
    sat_apply_manual_snapshot(rx_moved, tx_moved, rx, tx);
    return;
  }
  if (!rx_moved || !tx_moved) return;

  // Both sides moved: normal IC-9700 linked NOR/REV tuning.  Preserve Ofs and
  // adopt the final stable pair as the new tracking anchor.
  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES || !sat_snapshot_pair_has_expected_bands(rx, tx)) return;
  const double doppler_factor = (1.0 - p13.RR / 299792.0);
  if (doppler_factor == 0.0) return;
  plogw->dn_f = rx;
  plogw->up_f = tx;
  plogw->satdn_f = (double)rx / doppler_factor;
  plogw->satup_f = (double)tx * doppler_factor;
  plogw->dn_f_prev = rx;
  plogw->up_f_prev = tx;
  sat_reanchor_doppler_write_reference();
  if (verbose & 8)
    plogw->ostream->printf("SAT DIAL settle commit linked RX=%d TX=%d Ofs=%d unchanged\n",
                          rx, tx, sat_info[i].offset_freq);
}

static void sat_finish_ic9700_snapshot() {
  if (!sat_snapshot_seen[SAT_RX] || !sat_snapshot_seen[SAT_TX]) return;
  const int rx = sat_snapshot_hz[SAT_RX];
  const int tx = sat_snapshot_hz[SAT_TX];
  const int tol0 = plogw->freq_tolerance > 0 ? plogw->freq_tolerance : 1;
  const int tol = tol0 > SAT_DIAL_DEADBAND_HZ ? tol0 : SAT_DIAL_DEADBAND_HZ;

  if (!sat_snapshot_prev_valid) {
    sat_snapshot_prev_hz[SAT_RX] = rx;
    sat_snapshot_prev_hz[SAT_TX] = tx;
    sat_snapshot_prev_valid = true;
    return;
  }

  // Native R_A_T_B SATELLITE mode: once front-panel motion is seen, stop
  // writing frequencies until BOTH MAIN and SUB have settled.  Wrong-band or
  // mixed-time snapshots never advance the stable counter.
  const bool native_sat = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
  if (native_sat && sat_dial_settling) {
    if (!sat_snapshot_pair_has_expected_bands(rx, tx)) {
      sat_dial_stable_count = 0;
      if (verbose & 8)
        plogw->ostream->printf("SAT DIAL settling invalid pair RX=%d TX=%d; stable reset\n", rx, tx);
      return;
    }

    if (sat_absdiff(rx, sat_dial_start_hz[SAT_RX]) > tol) sat_dial_moved[SAT_RX] = true;
    if (sat_absdiff(tx, sat_dial_start_hz[SAT_TX]) > tol) sat_dial_moved[SAT_TX] = true;

    const bool same_rx = sat_absdiff(rx, sat_dial_last_hz[SAT_RX]) <= tol;
    const bool same_tx = sat_absdiff(tx, sat_dial_last_hz[SAT_TX]) <= tol;
    if (same_rx && same_tx) {
      if (sat_dial_stable_count < 255) ++sat_dial_stable_count;
    } else {
      sat_dial_stable_count = 0;
    }
    sat_dial_last_hz[SAT_RX] = rx;
    sat_dial_last_hz[SAT_TX] = tx;

    if (verbose & 8)
      plogw->ostream->printf("SAT DIAL settling RX=%d TX=%d moved=%d/%d stable=%u/%u\n",
                            rx, tx, sat_dial_moved[SAT_RX] ? 1 : 0,
                            sat_dial_moved[SAT_TX] ? 1 : 0,
                            (unsigned)sat_dial_stable_count,
                            (unsigned)SAT_DIAL_STABLE_SNAPSHOTS);

    if (sat_dial_stable_count >= SAT_DIAL_STABLE_SNAPSHOTS) {
      const bool rx_moved = sat_dial_moved[SAT_RX];
      const bool tx_moved = sat_dial_moved[SAT_TX];
      sat_dial_settling = false; // allow writes again only after commit
      sat_dial_stable_count = 0;
      sat_commit_native_dial_pair(rx, tx, rx_moved, tx_moved);
      sat_snapshot_prev_hz[SAT_RX] = rx;
      sat_snapshot_prev_hz[SAT_TX] = tx;
      // Discard any stale write request generated before settling began.
      sat_write_pending[SAT_RX] = sat_write_pending[SAT_TX] = false;
      sat_write_force[SAT_RX] = sat_write_force[SAT_TX] = false;
      if (verbose & 8)
        plogw->ostream->printf("SAT DIAL settling done RX=%d TX=%d moved=%d/%d\n",
                              rx, tx, rx_moved ? 1 : 0, tx_moved ? 1 : 0);
    }
    return;
  }

  bool rx_changed = sat_absdiff(rx, sat_snapshot_prev_hz[SAT_RX]) > tol;
  bool tx_changed = sat_absdiff(tx, sat_snapshot_prev_hz[SAT_TX]) > tol;

  // Suppress only the side that matches our recent SET.  The other side may
  // still be a genuine operator adjustment in the same one-second cycle.
  if (rx_changed && sat_frequency_report_is_own_or_invalid(SAT_RX, rx)) rx_changed = false;
  if (tx_changed && sat_frequency_report_is_own_or_invalid(SAT_TX, tx)) tx_changed = false;

  if (native_sat && (rx_changed || tx_changed)) {
    if (!sat_snapshot_pair_has_expected_bands(rx, tx)) {
      if (verbose & 8)
        plogw->ostream->printf("SAT DIAL start rejected invalid pair RX=%d TX=%d\n", rx, tx);
      return;
    }
    sat_dial_settling = true;
    sat_dial_start_hz[SAT_RX] = sat_snapshot_prev_hz[SAT_RX];
    sat_dial_start_hz[SAT_TX] = sat_snapshot_prev_hz[SAT_TX];
    sat_dial_last_hz[SAT_RX] = rx;
    sat_dial_last_hz[SAT_TX] = tx;
    sat_dial_moved[SAT_RX] = sat_absdiff(rx, sat_dial_start_hz[SAT_RX]) > tol;
    sat_dial_moved[SAT_TX] = sat_absdiff(tx, sat_dial_start_hz[SAT_TX]) > tol;
    sat_dial_stable_count = 0;
    // A monitor snapshot is only completed while no staged write owns the
    // bus, so it is safe to cancel queued (not in-flight) tracking requests.
    sat_write_pending[SAT_RX] = sat_write_pending[SAT_TX] = false;
    sat_write_force[SAT_RX] = sat_write_force[SAT_TX] = false;
    if (verbose & 8)
      plogw->ostream->printf("SAT DIAL settling begin start RX=%d TX=%d now RX=%d TX=%d moved=%d/%d\n",
                            sat_dial_start_hz[SAT_RX], sat_dial_start_hz[SAT_TX],
                            rx, tx, sat_dial_moved[SAT_RX] ? 1 : 0,
                            sat_dial_moved[SAT_TX] ? 1 : 0);
    return;
  }

  if (rx_changed || tx_changed) {
    // IC-9700 DUAL (R_B_T_A) does not link MAIN/SUB tuning.  Keep its existing
    // immediate companion-side behavior; settling is native SATELLITE only.
    const bool ic9700_dual = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX;
    if (ic9700_dual && (rx_changed != tx_changed)) {
      const double doppler_factor = (1.0 - p13.RR / 299792.0);
      if (doppler_factor != 0.0) {
        const int saved_tracking = plogw->sat_freq_tracking_mode;
        if (rx_changed) {
          plogw->dn_f = rx;
          plogw->satdn_f = (double)rx / doppler_factor;
          plogw->sat_freq_tracking_mode = SAT_RX_FIX;
          set_sat_freq_calc();
          sat_queue_frequency_write(SAT_TX, plogw->up_f, false);
          if (verbose & 8)
            plogw->ostream->printf("SAT DUAL DIAL RX=%d -> follow TX=%d Ofs=%d\n",
                                  rx, plogw->up_f, sat_info[plogw->sat_idx_selected].offset_freq);
        } else {
          plogw->up_f = tx;
          plogw->satup_f = (double)tx * doppler_factor;
          plogw->sat_freq_tracking_mode = SAT_TX_FIX;
          set_sat_freq_calc();
          sat_queue_frequency_write(SAT_RX, plogw->dn_f, false);
          if (verbose & 8)
            plogw->ostream->printf("SAT DUAL DIAL TX=%d -> follow RX=%d Ofs=%d\n",
                                  tx, plogw->dn_f, sat_info[plogw->sat_idx_selected].offset_freq);
        }
        plogw->sat_freq_tracking_mode = saved_tracking;
        sat_reanchor_doppler_write_reference();
      }
    } else if (ic9700_dual && verbose & 8) {
      plogw->ostream->printf("SAT DUAL DIAL RX+TX changed RX=%d TX=%d: Ofs unchanged\n", rx, tx);
    }
  }

  sat_snapshot_prev_hz[SAT_RX] = rx;
  sat_snapshot_prev_hz[SAT_TX] = tx;
}

bool sat_accept_frequency_report(struct radio *radio, int side, int freq_hz) {
  if (!plogw->sat || !radio || freq_hz <= 0 || (side != SAT_RX && side != SAT_TX))
    return false;

  // IC-9700 is handled as a paired snapshot by sat_accept_icom_frequency_report().
  if (sat_is_ic9700(radio) &&
      (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX))
    return true;

  if (sat_frequency_report_is_own_or_invalid(side, freq_hz)) return true;
  const int other = side == SAT_RX ? plogw->up_f : plogw->dn_f;
  sat_apply_manual_snapshot(side == SAT_RX, side == SAT_TX,
                            side == SAT_RX ? freq_hz : other,
                            side == SAT_TX ? freq_hz : other);
  return true;
}

bool sat_accept_generic_frequency_report(struct radio *radio, int freq_units) {
  if (!plogw->sat) return false;
  const int side = sat_side_for_radio(radio);
  if (side < 0) return false;
  return sat_accept_frequency_report(radio, side, freq_units * FREQ_UNIT);
}

static void sat_ic9700_select_and_query(struct radio *radio, int side);

static void sat_ic9700_select_main(struct radio *radio) {
  if (!radio) return;
  send_head_civ(radio);
  add_civ_buf((byte)0x07);
  add_civ_buf((byte)0xd0);
  send_tail_civ(radio);
  if (verbose & 8) plogw->ostream->println("SAT QUERY IC9700 burst done; MAIN restored");
}

bool sat_accept_icom_frequency_report(struct radio *radio, int freq_units) {
  if (!plogw->sat || sat_query_radio != radio || sat_query_side < 0 ||
      sat_query_stage != 2) return false;
  const int side = sat_query_side;
  const int freq_hz = freq_units * FREQ_UNIT;
  // Record the physical side that was actually selected for this query before
  // interpreting it as RX/TX.  This remains valid even when the bands are
  // reversed and the semantic readback is therefore rejected as wrong-band.
  if (sat_is_ic9700(radio)) {
    const bool ratb = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
    const bool queried_main = ratb ? (side == SAT_RX) : (side == SAT_TX);
    if (queried_main) {
      sat_ic9700_main_hz = freq_hz;
      sat_ic9700_main_seen = true;
    } else {
      sat_ic9700_sub_hz = freq_hz;
      sat_ic9700_sub_seen = true;
    }
  }
  sat_query_side = -1;
  sat_query_radio = nullptr;
  sat_query_started_ms = 0;
  sat_query_stage = 0;

  bool consumed = true;
  if (sat_is_ic9700(radio) &&
      (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX)) {
    sat_snapshot_hz[side] = freq_hz;
    sat_snapshot_seen[side] = true;
    if (side == SAT_TX) sat_finish_ic9700_snapshot();
  } else {
    consumed = sat_accept_frequency_report(radio, side, freq_hz);
  }

  // IC-9700: make the once-per-second monitor a short burst rather than
  // leaving the rig alternating MAIN/SUB throughout the second.  As soon as
  // the MAIN/RX frequency response arrives, read SUB/TX immediately; after
  // the SUB/TX response, restore MAIN and leave the rig alone until the next
  // one-second burst.
  if (sat_is_ic9700(radio) &&
      (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX)) {
    if (side == SAT_RX) {
      // Schedule the second half of the burst.  Do not transmit from the CAT
      // receive/parser path; the 100-ms interval sequencer owns all output.
      sat_query_side = SAT_TX;
      sat_query_radio = radio;
      sat_query_stage = 3; // queued select+query
      sat_query_action_ms = millis();
    } else {
      // Both physical MAIN and SUB are known.  Layout correction is queued;
      // MAIN restore is likewise performed by the interval sequencer.
      sat_ic9700_ensure_band_layout(radio);
      sat_query_side = SAT_RX;
      sat_query_radio = radio;
      sat_query_stage = 4; // queued MAIN restore/end burst
      sat_query_action_ms = millis();
    }
  }
  return consumed;
}

static void sat_ic9700_select_and_query(struct radio *radio, int side) {
  // R_A_T_B uses the IC-9700 native Satellite mapping MAIN=RX, SUB=TX.
  // R_B_T_A deliberately retains the normal dual mapping MAIN=TX, SUB=RX.
  const bool ratb = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
  const bool select_main = ratb ? (side == SAT_RX) : (side == SAT_TX);
  send_head_civ(radio);
  add_civ_buf((byte)0x07);
  add_civ_buf((byte)(select_main ? 0xd0 : 0xd1));
  send_tail_civ(radio);

  // Do not send 03 back-to-back with 07 D0/D1.  On the IC-9700 the old VFO
  // can still be selected at that instant, which is exactly how a 145-MHz
  // value was observed as the 435-MHz RX side.  The monitor service sends 03
  // after a short non-blocking settle interval.
  sat_query_side = side;
  sat_query_radio = radio;
  sat_query_started_ms = millis();
  sat_query_action_ms = millis() + SAT_IC9700_SELECT_SETTLE_MS;
  sat_query_stage = 1;
  if (verbose & 8)
    plogw->ostream->printf("SAT QUERY IC9700 select %s %s; 03 deferred\n",
                          side == SAT_RX ? "RX" : "TX",
                          select_main ? "MAIN" : "SUB");
}

void sat_frequency_monitor_reset(uint32_t settle_ms) {
  sat_query_side = -1;
  sat_query_radio = nullptr;
  sat_query_started_ms = 0;
  sat_query_stage = 0;
  sat_query_action_ms = 0;
  sat_monitor_next_ms = millis() + settle_ms;
  // A response to an abandoned satellite 03 must not block the next burst.
  if (radio_list[0].enabled && sat_is_ic9700(&radio_list[0]))
    radio_list[0].f_civ_response_expected = 0;
  if (verbose & 8)
    plogw->ostream->printf("SAT QUERY reset; settle=%lu ms\n", (unsigned long)settle_ms);
}


static bool sat_service_ic9700_write() {
  if (sat_write_stage == 0) return false;
  if ((int32_t)(millis() - sat_write_action_ms) < 0) return true;
  struct radio *r = sat_write_radio;
  if (!r || !r->enabled) {
    sat_write_stage = 0; sat_write_side = -1; sat_write_radio = nullptr;
    return true;
  }

  if (sat_write_stage == 1) {
    // Selection has settled.  If the 500-ms calculator updated this side
    // while SELECT was settling, consume the newest target rather than
    // transmitting the stale value captured at transaction start.
    if (sat_write_side >= 0 && sat_write_pending[sat_write_side]) {
      sat_write_hz = sat_write_pending_hz[sat_write_side];
      sat_write_pending[sat_write_side] = false;
      sat_write_force[sat_write_side] = false;
    }
    int f = sat_write_hz;
    send_head_civ(r);
    add_civ_buf((byte)0x05);
    add_civ_buf((byte)dec2bcd(f % 100)); f /= 100;
    add_civ_buf((byte)dec2bcd(f % 100)); f /= 100;
    add_civ_buf((byte)dec2bcd(f % 100)); f /= 100;
    add_civ_buf((byte)dec2bcd(f % 100)); f /= 100;
    add_civ_buf((byte)dec2bcd(f % 100));
    send_tail_civ(r);
    sat_own_write_hz[sat_write_side] = sat_write_hz;
    sat_own_write_ms[sat_write_side] = millis();
    sat_write_stage = 2;
    sat_write_action_ms = millis() + SAT_IC9700_WRITE_SETTLE_MS;
    if (verbose & 8)
      plogw->ostream->printf("SAT SEQ IC9700 05 %s=%d\n",
                            sat_write_side == SAT_RX ? "RX" : "TX", sat_write_hz);
    return true;
  }

  // Leave the front panel on MAIN after every complete transaction.
  send_head_civ(r); add_civ_buf((byte)0x07); add_civ_buf((byte)0xd0); send_tail_civ(r);
  if (sat_write_side == SAT_RX) plogw->dn_f_prev = sat_write_hz;
  else plogw->up_f_prev = sat_write_hz;
  sat_last_write_doppler_hz[sat_write_side] = sat_current_doppler_hz(sat_write_side);
  sat_last_write_doppler_valid[sat_write_side] = true;
  if (verbose & 8) plogw->ostream->println("SAT SEQ IC9700 write done; MAIN restored");
  sat_write_stage = 0; sat_write_side = -1; sat_write_hz = 0; sat_write_radio = nullptr;
  return true;
}

static bool sat_start_pending_write() {
  int side = -1;
  // TX first preserves the historical pair-write order; next interval will
  // service RX.  Only one CAT transaction is ever started per interval tick.
  if (sat_write_pending[SAT_TX]) side = SAT_TX;
  else if (sat_write_pending[SAT_RX]) side = SAT_RX;
  if (side < 0) return false;

  const int hz = sat_write_pending_hz[side];
  sat_write_pending[side] = false;
  sat_write_force[side] = false;

  struct radio *r = nullptr;
  int vfo = 0;
  switch (plogw->sat_vfo_mode) {
    case SAT_VFO_MULTI_TX_0: r = &radio_list[side == SAT_TX ? 0 : 1]; vfo = 0; break;
    case SAT_VFO_MULTI_TX_1: r = &radio_list[side == SAT_TX ? 1 : 0]; vfo = 0; break;
    case SAT_VFO_SINGLE_A_TX: r = &radio_list[0]; vfo = side == SAT_TX ? 0 : 1; break;
    case SAT_VFO_SINGLE_A_RX: r = &radio_list[0]; vfo = side == SAT_TX ? 1 : 0; break;
    default: return false;
  }
  if (!r || !r->enabled || !r->rig_spec) return true;

  if (sat_is_ic9700(r) &&
      (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX)) {
    // Begin a staged select -> wait -> 05 -> wait -> restore transaction.
    send_head_civ(r); add_civ_buf((byte)0x07);
    add_civ_buf((byte)(vfo ? 0xd1 : 0xd0)); send_tail_civ(r);
    sat_write_stage = 1; sat_write_side = side; sat_write_hz = hz;
    sat_write_radio = r; sat_write_action_ms = millis() + SAT_IC9700_WRITE_SETTLE_MS;
    if (verbose & 8)
      plogw->ostream->printf("SAT SEQ IC9700 select %s for %s=%d; 05 deferred\n",
                            vfo ? "SUB" : "MAIN", side == SAT_RX ? "RX" : "TX", hz);
    return true;
  }

  // Other rigs have an atomic/single-command SAT frequency write.  They still
  // pass through this common interval sequencer, so Doppler/UI code never
  // transmits CAT directly.
  set_vfo_frequency_rig(hz, vfo, r);
  if (side == SAT_RX) plogw->dn_f_prev = hz; else plogw->up_f_prev = hz;
  sat_last_write_doppler_hz[side] = sat_current_doppler_hz(side);
  sat_last_write_doppler_valid[side] = true;
  return true;
}

bool sat_frequency_monitor_process() {
  if (!plogw->sat) {
    sat_query_side = -1;
    sat_query_radio = nullptr;
    sat_query_started_ms = 0;
    sat_query_stage = 0;
    sat_monitor_next_ms = 0;
    sat_write_pending[SAT_RX] = sat_write_pending[SAT_TX] = false;
    sat_write_force[SAT_RX] = sat_write_force[SAT_TX] = false;
    sat_write_stage = 0; sat_write_side = -1; sat_write_radio = nullptr;
    sat_ic9700_swap_pending = false;
    sat_snapshot_prev_valid = false;
    sat_snapshot_seen[SAT_RX] = sat_snapshot_seen[SAT_TX] = false;
    sat_dial_settling = false;
    sat_dial_stable_count = 0;
    sat_dial_moved[SAT_RX] = sat_dial_moved[SAT_TX] = false;
    sat_last_write_doppler_valid[SAT_RX] = sat_last_write_doppler_valid[SAT_TX] = false;
    return false;
  }

  // A staged write owns the bus until select/write/restore is complete.
  if (sat_service_ic9700_write()) return true;

  // Parser callbacks only queue follow-up IC-9700 actions.  Execute them here.
  if (sat_query_stage == 3 && sat_query_radio) {
    struct radio *qr = sat_query_radio;
    sat_query_stage = 0; sat_query_side = -1; sat_query_radio = nullptr;
    sat_ic9700_select_and_query(qr, SAT_TX);
    return true;
  }
  if (sat_query_stage == 4 && sat_query_radio) {
    struct radio *qr = sat_query_radio;
    sat_query_stage = 0; sat_query_side = -1; sat_query_radio = nullptr;
    if (sat_ic9700_swap_pending) {
      send_head_civ(qr); add_civ_buf((byte)0x07); add_civ_buf((byte)0xb0); send_tail_civ(qr);
      sat_ic9700_swap_pending = false;
      sat_ic9700_main_seen = sat_ic9700_sub_seen = false;
      plogw->up_f_prev = plogw->dn_f_prev = 0;
      // Keep pending target writes, but verify the new physical layout first.
      sat_query_side = SAT_RX; sat_query_radio = qr; sat_query_stage = 5;
      sat_query_action_ms = millis() + 100U;
      sat_monitor_next_ms = millis() + 200U;
      if (verbose & 8) plogw->ostream->println("SAT SEQ IC9700 07 B0; restore/readback deferred");
      return true;
    }
    sat_ic9700_select_main(qr);
    return true;
  }
  if (sat_query_stage == 5 && sat_query_radio) {
    if ((int32_t)(millis() - sat_query_action_ms) < 0) return true;
    struct radio *qr = sat_query_radio;
    sat_query_stage = 0; sat_query_side = -1; sat_query_radio = nullptr;
    sat_ic9700_select_main(qr);
    return true;
  }

  // Complete the IC-9700 select -> wait -> 03 transaction without blocking.
  // ACKs to 07 D0/D1 are deliberately ignored; only the following 03 response
  // is allowed to consume the side tag.
  if (sat_query_side >= 0) {
    if (sat_query_stage == 1 && (int32_t)(millis() - sat_query_action_ms) >= 0) {
      struct radio *qr = sat_query_radio;
      if (!qr) {
        sat_query_side = -1;
        sat_query_stage = 0;
        return true;
      }
      send_head_civ(qr);
      add_civ_buf((byte)0x03);
      send_tail_civ(qr);
      qr->f_civ_response_expected = 1;
      qr->civ_response_timer = 50;
      sat_query_stage = 2;
      sat_query_started_ms = millis();
      if (verbose & 8)
        plogw->ostream->printf("SAT QUERY IC9700 03 %s\n",
                              sat_query_side == SAT_RX ? "RX" : "TX");
      return true;
    }
    if (sat_query_stage == 1) return true;
    if ((uint32_t)(millis() - sat_query_started_ms) < 300U) return true;
    if (verbose & 8) plogw->ostream->println("SAT QUERY timeout; side tag cleared");
    sat_query_side = -1;
    sat_query_radio = nullptr;
    sat_query_started_ms = 0;
    sat_query_stage = 0;
  }

  // Frequency writes from Doppler/CENTER/BEACON are serviced here, never
  // from the 500-ms calculator or UI callbacks.  IC-9700 waits until a monitor
  // burst has established/verified the physical MAIN/SUB layout.
  if (sat_write_pending[SAT_RX] || sat_write_pending[SAT_TX]) {
    struct radio *r0w = &radio_list[0];
    const bool need_layout = r0w->enabled && sat_is_ic9700(r0w) &&
        (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
         plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX) &&
        (!sat_ic9700_main_seen || !sat_ic9700_sub_seen || sat_ic9700_swap_pending);
    if (!need_layout) return sat_start_pending_write();
  }

  // IC-9700 Satellite mode: once per second, perform one short burst:
  // MAIN/RX query -> SUB/TX query -> restore MAIN.  The RX response launches
  // the TX query immediately, so MAIN/SUB activity is concentrated into a
  // short interval and the rig remains on MAIN for the rest of the second.
  // Return true between bursts so the ordinary poller cannot add extra
  // MAIN/SUB frequency queries.
  struct radio *r0 = &radio_list[0];
  if ((plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX) && r0->enabled &&
      sat_is_ic9700(r0)) {
    if ((int32_t)(millis() - sat_monitor_next_ms) < 0) return true;
    if (r0->f_civ_response_expected) return true;
    sat_snapshot_seen[SAT_RX] = sat_snapshot_seen[SAT_TX] = false;
    sat_ic9700_select_and_query(r0, SAT_RX);
    sat_monitor_next_ms = millis() + 1000U;
    return true;
  }

  // For two-radio satellite operation, alternate RX and TX radios at the same
  // 200-ms/side rate.  Single-rig non-9700 operation can only observe the
  // currently selected VFO through the generic query, but still benefits from
  // the common dial re-anchor logic in set_frequency().
  if (plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0 ||
      plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_1) {
    const int side = (sat_monitor_phase++ & 1) ? SAT_TX : SAT_RX;
    int ridx;
    if (plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0)
      ridx = side == SAT_TX ? 0 : 1;
    else
      ridx = side == SAT_TX ? 1 : 0;
    struct radio *r = &radio_list[ridx];
    if (!r->enabled || r->f_civ_response_expected) return true;
    send_freq_query_civ(r);
    return true;
  }

  return false;
}

static int sat_operating_kind() {
  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES) return 0;
  const char *up = sat_info[i].up_mode;
  const char *dn = sat_info[i].dn_mode;
  if ((up && strcmp(up, "FM") == 0) || (dn && strcmp(dn, "FM") == 0)) return 2;
  // Any configured non-FM transponder mode is treated as a linear
  // transponder.  The database's LSB/USB describes transponder sense; the
  // IC-9700 operator convention here is RX=USB and TX=USB/CW.
  if ((up && up[0]) || (dn && dn[0])) return 1;
  return 0;
}

bool sat_apply_default_opmode() {
  if (!plogw->sat) return false;
  struct radio *radio = so2r.radio_selected();
  if (!radio || !radio->enabled) return false;
  const int i = plogw->sat_idx_selected;
  if (i < 0 || i >= N_SATELLITES) return false;

  // The satellite database is authoritative for the normal uplink mode.
  // set_sat_opmode() independently obtains the RX/downlink mode from the
  // database (CW uplink is the one exception: RX is forced to USB).
  const char *up = sat_info[i].up_mode;
  if (up && up[0]) {
    set_sat_opmode(radio, (char *)up);
    if (verbose & 8)
      plogw->ostream->printf("SAT MODE auto: TX=%s RX=db(%s)\n",
                            up, sat_info[i].dn_mode);
    return true;
  }
  if (verbose & 8) plogw->ostream->println("SAT MODE auto: unknown; set UP/DOWN mode in satellite DB");
  return false;
}

bool sat_alt_m_opmode() {
  if (!plogw->sat) return false;
  struct radio *radio = so2r.radio_selected();
  if (!radio || !radio->enabled) return true;
  const int kind = sat_operating_kind();
  if (kind == 2) {
    set_sat_opmode(radio, (char *)"FM");
    if (verbose & 8) plogw->ostream->println("SAT Alt-M: FM satellite; keep RX/TX FM");
    return true;
  }
  if (kind == 1) {
    const int i = plogw->sat_idx_selected;
    const char *db_up = (i >= 0 && i < N_SATELLITES && sat_info[i].up_mode[0])
                            ? sat_info[i].up_mode : "USB";
    const bool tx_is_cw = strcmp(radio->opmode, "CW") == 0 ||
                          strcmp(radio->opmode, "CW-R") == 0;
    const char *next = tx_is_cw ? db_up : "CW";
    set_sat_opmode(radio, (char *)next);
    if (verbose & 8)
      plogw->ostream->printf("SAT Alt-M: TX=%s RX=%s\n", next,
                            strcmp(next, "CW") == 0 || strcmp(next, "CW-R") == 0
                                ? "USB" : sat_info[i].dn_mode);
    return true;
  }
  // No mode information in the satellite DB: leave legacy Alt-M available so
  // the operator can still work an unclassified entry.
  return false;
}

void set_vfo_frequency_rig(int freq, int vfo, struct radio *radio)
// vfo 0 for main 1 for sub (unselected) vfo selection
{
  if (!radio->enabled) {
    if (verbose & 8) {
      plogw->ostream->print("Rig #");
      plogw->ostream->print(radio->rig_idx);
      plogw->ostream->println("is not enabled. not set freq.");
    }
    return;
  }
  // Remember IC-9700 satellite writes before `freq` is consumed by the BCD
  // encoder below.  This is used only to suppress our own immediate readback.
  if (plogw->sat && sat_is_ic9700(radio) &&
      (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ||
       plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX)) {
    const bool ratb = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
    const int side = ratb ? (vfo == 0 ? SAT_RX : SAT_TX)
                          : (vfo == 0 ? SAT_TX : SAT_RX);
    sat_own_write_hz[side] = freq;
    sat_own_write_ms[side] = millis();
  }

  if (verbose & 8) {
    plogw->ostream->print("set_vfo_frequency_rig(): vfo ");
    plogw->ostream->print(vfo);
    plogw->ostream->print(":");
    plogw->ostream->print("RIG idx:");
    plogw->ostream->print(radio->rig_idx);
    plogw->ostream->print(" freq ");
    plogw->ostream->print(freq);
  }

  switch (radio->rig_spec->rig_type) {
    case 0:  // IC-705 VFO sel/unselected to set RX/TX frequency
      if (verbose & 8) plogw->ostream->print(" IC-705 ( sel RX unsel TX )");
      send_head_civ(radio);
      add_civ_buf((byte)0x25);  // Frequency !
      switch (vfo) {
        case 1:
          if (verbose & 8) plogw->ostream->print(" flip vfo selection");
          add_civ_buf((byte)(1 - vfo));  // flip vfo selection
          break;
        case 0:
          if (verbose & 8) plogw->ostream->print(" normal vfo selection");
          add_civ_buf((byte)vfo);
          break;
      }
      //      civport->write((byte)vfo);
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10 Hz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 100kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10MHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1GHz
      send_tail_civ(radio);
      break;

    case 1: { // IC-9700: select MAIN/SUB with 07, then set selected frequency with 05
      const bool native_sat = plogw->sat && plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX;
      const int requested_freq = freq;

      // Select the required IC-9700 side and write its operating frequency.
      // Native Satellite mode uses MAIN=RX/downlink and SUB=TX/uplink; no
      // 07 D2 band-assignment command is involved.
      if (verbose & 8)
        plogw->ostream->print(native_sat
                                 ? " IC-9700 SAT main RX sub TX"
                                 : " IC-9700 normal main TX sub RX");

      // The IC-9700 path used 07 D0/D1 + 05 successfully before the v3
      // experiment with command 25.  Restore that proven write path.  The
      // read/query side still has the deferred 03 and wrong-band protection,
      // so a stale readback cannot re-anchor the satellite frequencies.
      if (verbose & 8) {
        plogw->ostream->printf("\nSAT CIV WRITE SELECT vfo=%d side=%s bytes=FE FE %02X E0 07 %02X FD\n",
                               vfo, vfo == 1 ? "SUB" : "MAIN",
                               radio->rig_spec->civaddr, vfo == 1 ? 0xd1 : 0xd0);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select MAIN/SUB
      if (vfo == 1) {
        if (verbose & 8) plogw->ostream->print(native_sat ? " select sub (TX)" : " select sub (RX)");
        add_civ_buf((byte)0xd1);
      } else {
        if (verbose & 8) plogw->ostream->print(native_sat ? " select main (RX)" : " select main (TX)");
        add_civ_buf((byte)0xd0);
      }
      send_tail_civ(radio);

      // Set the frequency of the selected MAIN/SUB side.
      if (verbose & 8) {
        int fdiag = requested_freq;
        byte b0 = dec2bcd(fdiag % 100); fdiag /= 100;
        byte b1 = dec2bcd(fdiag % 100); fdiag /= 100;
        byte b2 = dec2bcd(fdiag % 100); fdiag /= 100;
        byte b3 = dec2bcd(fdiag % 100); fdiag /= 100;
        byte b4 = dec2bcd(fdiag % 100);
        plogw->ostream->printf("SAT CIV WRITE FREQ vfo=%d side=%s hz=%d bytes=FE FE %02X E0 05 %02X %02X %02X %02X %02X FD\n",
                               vfo, vfo == 1 ? "SUB" : "MAIN", requested_freq,
                               radio->rig_spec->civaddr, b0, b1, b2, b3, b4);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x05);
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;
      add_civ_buf((byte)dec2bcd(freq % 100));
      send_tail_civ(radio);

      // Leave the operator on MAIN in native Satellite mode.  For R_B_T_A
      // (normal DUAL) retain the historical SUB selection.
      if (verbose & 8) {
        const byte restore = native_sat ? 0xd0 : 0xd1;
        plogw->ostream->printf("SAT CIV WRITE RESTORE side=%s bytes=FE FE %02X E0 07 %02X FD\n",
                               native_sat ? "MAIN" : "SUB",
                               radio->rig_spec->civaddr, restore);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x07);
      add_civ_buf((byte)(native_sat ? 0xd0 : 0xd1));
      send_tail_civ(radio);
      break;
    }

    case RIG_TYPE_YAESU:       // FT-991A/FTDX10 Yaesu CAT
    case RIG_TYPE_YAESU_FTX1:  // FTX-1 Yaesu CAT
    case RIG_TYPE_KENWOOD:     // QCX-mini / Kenwood CAT
      // currently just send frequency for these rig types
      // no VFO selection. 22/8/25
      int type;
      type = radio->rig_spec->cat_type;
      char buf[30];
      switch (type) {
        case 1:
          if (verbose & 8) plogw->ostream->print(" Yaesu ");
          if (freq < 0) return;
          sprintf(buf, "FA%09d;", freq);
          send_cat_cmd(radio, buf);
          return;
        case 2:
          // kenwood TS480 uses 11 digit
          if (verbose & 8) plogw->ostream->print(" Kenwood ");
          if (freq < 0) return;
          sprintf(buf, "FA%011d;", freq);
          send_cat_cmd(radio, buf);
          return;
        case 3:
          return;
      }
      break;


    case 4: { // manual rig: no CAT, but keep DVPlogger's internal frequency/LCD in sync
      // Satellite frequencies are in Hz; radio->freq uses FREQ_UNIT-Hz units.
      // Unlike CAT rigs, a manual rig has no later rig response that would
      // refresh the normal radio display, so redraw it only when the displayed
      // frequency actually changes.
      const unsigned int old_freq = radio->freq;
      set_frequency_rig_radio((unsigned int)(freq / FREQ_UNIT), radio);
      if (radio->freq != old_freq) upd_display();
      if (verbose & 8) plogw->ostream->print(" manual rig internal freq/LCD updated.");
      break;
    }
  }
  if (verbose & 8) plogw->ostream->println("");
}

// set_sat_freq_calc で計算した周波数をリグに適宜セットする
void set_sat_freq_to_rig_vfo(int freq, int tx)
// Public SAT frequency request entry.  Never transmit CAT here: all rigs are
// serialized by sat_frequency_monitor_process() in the 100-ms interval.
{
  sat_queue_frequency_write(tx ? SAT_TX : SAT_RX, freq, false);
}



void set_sat_freq_up() {
  const int d = sat_current_doppler_hz(SAT_TX);
  if (!sat_last_write_doppler_valid[SAT_TX] ||
      sat_absdiff(d, sat_last_write_doppler_hz[SAT_TX]) >= SAT_DOPPLER_WRITE_THRESHOLD_HZ)
    sat_queue_frequency_write(SAT_TX, plogw->up_f, false);
}

void set_sat_freq_dn() {
  const int d = sat_current_doppler_hz(SAT_RX);
  if (!sat_last_write_doppler_valid[SAT_RX] ||
      sat_absdiff(d, sat_last_write_doppler_hz[SAT_RX]) >= SAT_DOPPLER_WRITE_THRESHOLD_HZ)
    sat_queue_frequency_write(SAT_RX, plogw->dn_f, false);
}

// CENTER/BEACON are explicit operator commands, but CAT output is still owned
// by the 100-ms SAT sequencer.  Force both targets into its queue.
static void sat_force_freq_pair_to_rig() {
  sat_queue_frequency_write(SAT_TX, plogw->up_f, true);
  sat_queue_frequency_write(SAT_RX, plogw->dn_f, true);
  // Force a fresh IC-9700 physical-layout read before either write.
  if (radio_list[0].enabled && sat_is_ic9700(&radio_list[0])) {
    sat_ic9700_main_seen = sat_ic9700_sub_seen = false;
    sat_monitor_next_ms = millis();
  }
  if (verbose & 8)
    plogw->ostream->printf("SAT FORCE queued TX=%d RX=%d\n", plogw->up_f, plogw->dn_f);
}


void set_sat_freq_to_rig() {
  switch (plogw->sat_freq_tracking_mode) {
    case SAT_TX_FIX:  // send RX frequency
      set_sat_freq_dn();
      break;
    case SAT_RX_FIX:  // send TX frequency
      set_sat_freq_up();
      break;
    case SAT_SAT_FIX:  // send TX frequency to main  and RX frequcy to SUB RIG
      set_sat_freq_up();
      set_sat_freq_dn();
      break;
  }
}


// up/down frequency calculation
void set_sat_freq_calc() {
  int i;
  i = plogw->sat_idx_selected;
  if (i < 0) return;

  // A corrupt saved Ofs must be repaired before SAT_RX_FIX/SAT_TX_FIX/
  // SAT_SAT_FIX use it to derive the opposite side.
  sat_repair_corrupt_offset_if_needed(i);

  // check plogw->freq to set up_f, satup_f, and dn_f dependnt on sat_vfo_mode and sat_freq_tracking_mode

  // suppress frequency control during nextaos calcuration because p13.RR may be of other satellites
  if (plogw->f_nextaos != 0) return;

  //The program calculates range-rate RR (line 4190) in km/s. If a satellite transmits a frequency FT, then this is received as a frequency FR:
  //FR = FT * (1 - RR/299792). [Note: 299792 is the speed of light in km/s]
  double doppler_factor;
  doppler_factor = (1.0 - p13.RR / 299792.0);
  double ofs;
  double fsat0;  // frequency at satellite


  // calc beacon_frequency for calculation
  if (sat_info[i].bc_f0 != 0) {
    plogw->beacon_f = sat_info[i].bc_f0 * doppler_factor;
  } else {
    plogw->beacon_f = 0;
  }
  if (verbose & 8) {
    plogw->ostream->print("Beacon freq:");
    plogw->ostream->println(plogw->beacon_f);
  }

  //
  if (verbose & 8) plogw->ostream->print("sat_freq_tracking_mode:");
  switch (plogw->sat_freq_tracking_mode) {
    case SAT_RX_FIX:  // RX fixed (TX frequency calc and set )
      if (verbose & 8) plogw->ostream->println("SAT_RX_FIX");

      //
      //  relationship between uplink downlink frequencies
      // ofs0:offset_freq
      //  dn_f0+ofs0 ----ofs ----satdn_f             dn_f1+ofs0
      //	  up_f0        	           (satup_f)----ofs ---up_f1

      fsat0 = plogw->dn_f / doppler_factor;
      // check fsat0 with downlink frequency
      plogw->satdn_f = fsat0;
      if (fsat0 > (sat_info[i].dn_f1 + sat_info[i].offset_freq) || fsat0 < (sat_info[i].dn_f0 + sat_info[i].offset_freq)) {
        if (verbose & 8) {
          plogw->ostream->print("downlink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
      }
      ofs = fsat0 - (sat_info[i].dn_f0 + sat_info[i].offset_freq);
      // Satellite-side uplink frequency (before ground-to-satellite Doppler).
      plogw->satup_f = sat_info[i].up_f1 - ofs;
      // usually uplink frequency is inverse so offset from upper edge of downlink band
      plogw->up_f = plogw->satup_f / doppler_factor;
      break;

    case SAT_TX_FIX:  // TX fixed (RX frequency controlled)
      if (verbose & 8) plogw->ostream->println("SAT_TX_FIX");



      fsat0 = plogw->up_f * doppler_factor;
      plogw->satup_f = fsat0;
      if (fsat0 > sat_info[i].up_f1 || fsat0 < sat_info[i].up_f0) {
        if (verbose & 8) {
          plogw->ostream->print("uplink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
        plogw->f_inband_txp = 0;
      } else {
        plogw->f_inband_txp = 1;
      }
      ofs = sat_info[i].up_f1 - fsat0;
      // usually uplink frequency is inverse so offset from upper edge of downlink band
      // offset is down link offset added by satellite transmission
      plogw->satdn_f = sat_info[i].dn_f0 + sat_info[i].offset_freq + ofs;
      plogw->dn_f = plogw->satdn_f * doppler_factor;
      break;

    case SAT_SAT_FIX:  // satellite fixed  (both TX RX frequency controlled )
      if (verbose & 8) plogw->ostream->println("SAT_SAT_FIX");

      // in this mode, sat uplink frequency may not be calculated from current vfo setting reliably. So, satup_f may be set only by Alt-key command to force set frequency, or in different mode


      fsat0 = plogw->satdn_f;
      if (fsat0 > (sat_info[i].dn_f1 + sat_info[i].offset_freq) || fsat0 < (sat_info[i].dn_f0 + sat_info[i].offset_freq)) {
        if (verbose & 8) {
          plogw->ostream->print("downlink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
        plogw->f_inband_txp = 0;
      } else {
        plogw->f_inband_txp = 1;
      }
      ofs = fsat0 - (sat_info[i].dn_f0 + sat_info[i].offset_freq);
      plogw->satup_f = sat_info[i].up_f1 - ofs;
      plogw->up_f = plogw->satup_f / doppler_factor;
      plogw->dn_f = plogw->satdn_f * doppler_factor;

      break;
  }

  // set up , down frequencies to rig(s)
  set_sat_freq_to_rig();
  if (verbose & 8) {
    plogw->ostream->print("Up ");
    plogw->ostream->print(plogw->up_f);
    plogw->ostream->print(" Dn ");
    plogw->ostream->println(plogw->dn_f);
  }
}

void set_sat_center_frequency() {
  if (verbose & 8) plogw->ostream->println("set sat center frequency");
  // Center and Beacon must use exactly the same IC-9700 layout path.
  sat_apply_vfo_mode_to_rig();
  int i;
  i = plogw->sat_idx_selected;
  if (i >= 0) {
    int memo;
    memo = plogw->sat_freq_tracking_mode;  // temprally memorize
    plogw->sat_freq_tracking_mode = 2;     // sat fixed mode
    plogw->satdn_f = (sat_info[i].dn_f0 / 2 + sat_info[i].dn_f1 / 2);
    if (verbose & 8) plogw->ostream->print("-> satdn_f = ");
    plogw->ostream->println(plogw->satdn_f);
    set_sat_freq_calc();
    sat_force_freq_pair_to_rig();
    plogw->sat_freq_tracking_mode = memo;
  }
}

void set_sat_beacon_frequency() {
  if (verbose & 8) plogw->ostream->println("set sat beacon frequency");
  sat_apply_vfo_mode_to_rig();
  int i;
  i = plogw->sat_idx_selected;
  if (i >= 0) {
    if (sat_info[i].bc_f0 != 0) {
      int memo;
      memo = plogw->sat_freq_tracking_mode;  // temprally memorize
      plogw->sat_freq_tracking_mode = 2;     // sat fixed mode
      plogw->satdn_f = sat_info[i].bc_f0;
      set_sat_freq_calc();
      sat_force_freq_pair_to_rig();
      plogw->sat_freq_tracking_mode = memo;
    }
  }
}

void set_location_gl_calc(char *locator) {
  // set location from grid locator
  double lon, lat;
  lon = mh2lon(locator);
  lat = mh2lat(locator);
  // set logwindow the location
  plogw->longitude=lon;
  plogw->latitude=lat;
  // set to p13
  p13.setLocation(lon, lat, 50);
  if (verbose & 8) {
    plogw->ostream->print("Lat:");
    plogw->ostream->print(lat);
    plogw->ostream->print(" Lon:");
    plogw->ostream->println(lon);
  }
}

//    p13.setFrequency(435300000, 145920000);//AO-51  frequency
//    p13.setLocation(-64.375, 45.8958, 20); // Sackville, NB
//    p13.setTime(2009, 10, 1, 19, 5, 0); //Oct 1, 2009 19:05:00 UTC


// satellite display update

void freq2str(char *s, int freq) {
  int tmp1, tmp2;
  tmp2 = (freq % 1000) / 10;
  tmp1 = freq / 1000;
  tmp1 = tmp1 % 1000;
  sprintf(s, "%3d.%03d.%02d", freq / 1000000, tmp1, tmp2);
}

void upd_display_sat() {
  //  console->print("upd_display_sat timer=");
  //  console->println(info_disp.timer);
  if (info_disp.timer > 0) return;
  const uint32_t satlcd_t0_us = micros();
  select_left_display();  
  char sbuf[20], sbuf1[20];
  sprintf(dp->lcdbuf, "%-6s %-6s az%03d%c", plogw->sat_name_set, plogw->grid_locator_set, plogw->rotator_az, plogw->f_rotator_track ? 'T' : ' ');
  display_printStr(dp->lcdbuf, 10);

  dtostrf(p13.AZ, 3, 0, sbuf);
  dtostrf(p13.EL, 3, 0, sbuf1);
  char vfo_label[8];
  sat_vfo_mode_label_compact(vfo_label, sizeof(vfo_label));
  sprintf(dp->lcdbuf, "AZ:%3s EL:%3s %s", sbuf, sbuf1, vfo_label);
  display_printStr(dp->lcdbuf, 11);

  freq2str(sbuf1, plogw->up_f);
  // sat_info endpoints are corresponding uplink/downlink transponder edges.
  // Equal endpoints mean a fixed-frequency (e.g. FM) channel, for which
  // NOR/REV is not meaningful.  Otherwise the relative edge directions
  // directly tell us whether the transponder is normal or inverting.
  const int satlcd_idx_tx = plogw->sat_idx_selected;
  const char *sat_sense = "";
  if (satlcd_idx_tx >= 0 && satlcd_idx_tx < N_SATELLITES) {
    const int64_t du = (int64_t)sat_info[satlcd_idx_tx].up_f1 - sat_info[satlcd_idx_tx].up_f0;
    const int64_t dd = (int64_t)sat_info[satlcd_idx_tx].dn_f1 - sat_info[satlcd_idx_tx].dn_f0;
    if (du != 0 && dd != 0) sat_sense = ((du > 0) == (dd > 0)) ? "REV" : "NOR";
  }
  if (sat_sense[0])
    sprintf(dp->lcdbuf, "TX%c:%s %s", plogw->sat_freq_tracking_mode == SAT_TX_FIX ? '*' : ' ', sbuf1, sat_sense);
  else
    sprintf(dp->lcdbuf, "TX%c:%s", plogw->sat_freq_tracking_mode == SAT_TX_FIX ? '*' : ' ', sbuf1);
  display_printStr(dp->lcdbuf, 12);

  freq2str(sbuf, plogw->dn_f);
  sprintf(dp->lcdbuf, "RX%c:%s", plogw->sat_freq_tracking_mode == SAT_RX_FIX ? '*' : ' ', sbuf);
  display_printStr(dp->lcdbuf, 13);

  // Keep the traditional satellite-side frequency row.  Beacon information,
  // when available, shares the final row with Ofs so the six-row layout is
  // unchanged.
  freq2str(sbuf, plogw->satdn_f);
  sprintf(dp->lcdbuf, "S %c:%s%c", plogw->sat_freq_tracking_mode == SAT_SAT_FIX ? '*' : ' ', sbuf, plogw->f_inband_txp == 1 ? ' ' : '!');
  display_printStr(dp->lcdbuf, 14);

  const int satlcd_idx = plogw->sat_idx_selected;
  int ofs_hz = 0;
  if (satlcd_idx >= 0 && satlcd_idx < N_SATELLITES) {
    ofs_hz = sat_info[satlcd_idx].offset_freq;
  }

  // Ofs is shown in 0.01 kHz (10 Hz) units with an explicit sign.
  const char ofs_sign = (ofs_hz >= 0) ? '+' : '-';
  int ofs_abs_hz = (ofs_hz >= 0) ? ofs_hz : -ofs_hz;
  int ofs_001khz = (ofs_abs_hz + 5) / 10;  // round to nearest 0.01 kHz
  int ofs_whole = ofs_001khz / 100;
  int ofs_frac = ofs_001khz % 100;

  if (satlcd_idx >= 0 && satlcd_idx < N_SATELLITES && sat_info[satlcd_idx].bc_f0 > 0) {
    // Nominal beacon frequency, displayed to kHz like TX/RX/S.  Do not use
    // Doppler-corrected plogw->beacon_f here.
    const int beacon_khz = (sat_info[satlcd_idx].bc_f0 + 500) / 1000;
    sprintf(dp->lcdbuf, "Bn :%d.%03d O:%c%d.%02dk",
            beacon_khz / 1000, beacon_khz % 1000, ofs_sign, ofs_whole, ofs_frac);
  } else {
    sprintf(dp->lcdbuf, "O:%c%d.%02dk", ofs_sign, ofs_whole, ofs_frac);
  }
  display_printStr(dp->lcdbuf, 15);

  //u8g2_l.sendBuffer();  // transfer internal memory to the display
  left_display_sendBuffer();

  // Measure the complete satellite LCD redraw, including the OLED buffer
  // transfer.  This is intentionally printed once per actual LCD update so
  // that it can be compared directly with INTERVAL SLOW diagnostics.
  const uint32_t satlcd_dt_us = micros() - satlcd_t0_us;
  if (verbose & VERBOSE_PERF) {
    plogw->ostream->print("[SATLCD] upd_display_sat ");
    plogw->ostream->print((unsigned long)satlcd_dt_us);
    plogw->ostream->println(" us");
  }
}

void print_sat_info_by_index(int i) {

  if (verbose & 8) {
    plogw->ostream->print("Name:");
    plogw->ostream->println(sat_info[i].name);
    plogw->ostream->print(" YEAR ");
    plogw->ostream->println(sat_info[i].YEAR);
    plogw->ostream->print(" EPOCH ");
    plogw->ostream->println(sat_info[i].EPOCH);
    plogw->ostream->print(" INCLINATION ");
    plogw->ostream->println(sat_info[i].INCLINATION);
    plogw->ostream->print(" RAAN ");
    plogw->ostream->println(sat_info[i].RAAN);
    plogw->ostream->print(" ECCENTRICITY ");
    plogw->ostream->println(sat_info[i].ECCENTRICITY);
    plogw->ostream->print(" ARGUMENT_PEDIGREE ");
    plogw->ostream->println(sat_info[i].ARGUMENT_PEDIGREE);
    plogw->ostream->print(" MEAN_ANOMALY ");
    plogw->ostream->println(sat_info[i].MEAN_ANOMALY);
    plogw->ostream->print(" MEAN_MOTION ");
    plogw->ostream->println(sat_info[i].MEAN_MOTION);
    plogw->ostream->print(" TIME_MOTION_D ");
    plogw->ostream->println(sat_info[i].TIME_MOTION_D);
    plogw->ostream->print(" EPOCH_ORBIT ");
    plogw->ostream->println(sat_info[i].EPOCH_ORBIT);

    plogw->ostream->print(" Up Freq ");
    plogw->ostream->print(sat_info[i].up_f0);
    plogw->ostream->print(" - ");
    plogw->ostream->print(sat_info[i].up_f1);
    plogw->ostream->print(" ");
    plogw->ostream->println(sat_info[i].up_mode);
    plogw->ostream->print(" Dn Freq ");
    plogw->ostream->print(sat_info[i].dn_f0);
    plogw->ostream->print(" - ");
    plogw->ostream->print(sat_info[i].dn_f1);
    plogw->ostream->print(" ");
    plogw->ostream->println(sat_info[i].dn_mode);

    plogw->ostream->print(" Beacon ");
    plogw->ostream->println(sat_info[i].bc_f0);
  }
}

void print_sat_info(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {

    if (!plogw->f_console_emu) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println(" Not found");
    }
    return;
  }
  print_sat_info_by_index(i);
}




int compare_satinfo_aos(const void *a, const void *b) {
  DateTime timea, timeb;
  timea = sat_info[*(int *)a].nextaos;
  timeb = sat_info[*(int *)b].nextaos;
  return (timea.unixtime() - timeb.unixtime());
}

int compare_datetime(DateTime a, DateTime b) {
  return (a.unixtime() - b.unixtime());
}


// display list of aos los maxel
void print_sat_info_aos() {
  // create list of index in satidx_sort
  int nsat;
  nsat = 0;
  for (int i = 0; i < N_SATELLITES; i++) {
    if (sat_info[i].YEAR == 0) continue;
    satidx_sort[nsat] = i;
    nsat++;
  }
  int j;
  if (!plogw->f_console_emu) {
    plogw->ostream->print("nsat=");
    plogw->ostream->println(nsat);
  }
  qsort(satidx_sort, nsat, sizeof(int), compare_satinfo_aos);
  char buf[30];
  strcpy(dp->lcdbuf, "");
  int count;
  count = 0;
  for (int i = 0; i < nsat; i++) {
    j = satidx_sort[i];
    sprintf(buf, "%-7s", sat_info[j].name);
    if (!plogw->f_console_emu) plogw->ostream->print(buf);

    if ((count >= plogw->nextaos_showidx) && (count <= plogw->nextaos_showidx + 6)) {
      // print to LCD
      strcat(dp->lcdbuf, buf);
      sprintf(buf, "%02d:%02d-%02d:%02d ", sat_info[j].nextaos.hour(),
              sat_info[j].nextaos.minute(),
              sat_info[j].nextlos.hour(),
              sat_info[j].nextlos.minute());

      strcat(dp->lcdbuf, buf);
      dtostrf(sat_info[j].maxel, 2, 0, buf);
      strcat(dp->lcdbuf, buf);
      strcat(dp->lcdbuf, "\n");
    }
    if (!plogw->f_console_emu) {
      plogw->ostream->print("AOS ");
      print_datetime(sat_info[j].nextaos);
      plogw->ostream->print(" LOS ");
      print_datetime(sat_info[j].nextlos);
      plogw->ostream->print(" EL ");
      plogw->ostream->print(sat_info[j].maxel);
      plogw->ostream->println("");
    }
    count++;
  }
  upd_display_info_flash(dp->lcdbuf);
}



void init_sat() {
  // TLE I/O uses bounded local buffers.  No persistent heap allocation is
  // needed here; this avoids heap fragmentation while HTTP/AsyncTCP is active.
}


void allocate_sat() {
  // Kept for API compatibility with older callers.  TLE buffers are now local
  // to getTLE()/readtlefile(), so there is nothing to allocate.
}

// Kept for API compatibility.  Do not force satellite mode off merely because
// a TLE transfer/parser temporary buffer has gone away.
void release_sat() {
}

int find_satname(char *satname) {
  if (!satname) return -1;
  for (int i = 0; i < N_SATELLITES; ++i) {
    if (sat_info[i].name[0] == '\0') continue;
    if (strcmp(satname, sat_info[i].name) == 0) return i;
  }
  return -1;
}

void readtlefile() {
  // TLE lines are 69 characters.  Keep only name + line1; line2 can be
  // parsed directly from the input buffer.  This cuts parser stack usage and
  // avoids carrying three 128-byte temporary strings at once.
  char line[96] = {0};
  char tle1_line[72] = {0};
  char satname_line[64] = {0};
  bool have_sat_definition = false;
  for (int i = 0; i < N_SATELLITES; ++i) {
    if (sat_info[i].name[0] != '\0') {
      have_sat_definition = true;
      break;
    }
  }
  if (!have_sat_definition) load_satinfo();

  plogw->ostream->printf("opening tlefile %s\n",tlefilename);  
  tlefile = SD.open(tlefilename, FILE_READ);
  if (!tlefile) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("opening TLE file");
      plogw->ostream->println(tlefilename);
      plogw->ostream->print(" failed");
    }
    return;
  }
  sprintf(dp->lcdbuf, "Read TLE file \n");
  upd_display_info_flash(dp->lcdbuf);

  if (!plogw->f_console_emu) {
    plogw->ostream->print("printing contents of ");
    plogw->ostream->println(tlefilename);
  }
  
  int count;
  int stat;
  int tle_records = 0;
  int tle_matched = 0;
  stat = 0;
  count = 0;
  //  allocate_sat();
  while (tlefile.available()) {
    // read line and store data into memory
    //    char c;
    //    c = tlefile.read();
    //    plogw->ostream->print(c);
    int c = tlefile.readBytesUntil(0x0a, (uint8_t *)line, sizeof(line) - 1);
    if (c > 0) {
      count++;      
      //plogw->ostream->print("c="); plogw->ostream->println(c);
      line[c] = '\0';
      // replace stray 0x0d
      for (int i = 0; i < c; i++) {
        if (line[i] == 0x0d) { line[i] = '\0'; break; }
      }

      // the first line is unix time of the tle file
      if (count==1) {
	sscanf(line,"%ld",&plogw->tle_unixtime);
	plogw->ostream->print("tle file unixtime=");
	plogw->ostream->println(plogw->tle_unixtime);
	continue;
      }
      switch (stat) {
        case 0:  // name
          strlcpy(satname_line, line, sizeof(satname_line));
          stat = 1;
          break;
        case 1:  // TLE1
          if (line[0] == '1') {
            strlcpy(tle1_line, line, sizeof(tle1_line));
            stat = 2;
          } else {
            stat = 0;
          }
          break;
        case 2:  // TLE2
          if (line[0] == '2') {

            // check contents
            if (verbose & 8) {
              plogw->ostream->println("Satellite entry");
              plogw->ostream->write((uint8_t *)satname_line, strlen(satname_line));
              plogw->ostream->println("");
              plogw->ostream->write((uint8_t *)tle1_line, strlen(tle1_line));
              plogw->ostream->println("");
              plogw->ostream->write((uint8_t *)line, strlen(line));
              plogw->ostream->println("");
            }
            int i;
            i = find_satname(satname_line);
            tle_records++;
            if (i >= 0) tle_matched++;
            if (verbose & 8) {
              plogw->ostream->printf("TLE MATCH %-32s : %s",
                                    satname_line, (i >= 0) ? "MATCH" : "NO MATCH");
              if (i >= 0) plogw->ostream->printf(" idx=%d", i);
              plogw->ostream->println();
            }
            if (i != -1) {
              // Avoid a full OLED buffer flush for every matching satellite.
              // The parser can run while network callbacks are active; repeated
              // display work here only lengthens that critical window.

              // this is the satellite to read into sat_info database
              if (verbose & 8) {
                plogw->ostream->print("Satellite info:");
                plogw->ostream->println(satname_line);
              }
              strcpy(sat_info[i].name, satname_line);
              char buf[20];
              buf[0] = '\0';
              strncat(buf, tle1_line + 18, 2);
              sat_info[i].YEAR = atoi(buf) + 2000;
              buf[0] = '\0';
              strncat(buf, tle1_line + 20, 12);
              sat_info[i].EPOCH = atof(buf);
              buf[0] = '\0';
              strncat(buf, line + 8, 8);
              sat_info[i].INCLINATION = atof(buf);
              buf[0] = '\0';
              strncat(buf, line + 17, 8);
              sat_info[i].RAAN = atof(buf);
              buf[0] = '0';
              buf[1] = '.';
              buf[2] = '\0';
              strncat(buf + 2, line + 26, 7);
              sat_info[i].ECCENTRICITY = atof(buf);

              buf[0] = '\0';
              strncat(buf, line + 34, 8);
              sat_info[i].ARGUMENT_PEDIGREE = atof(buf);
              buf[0] = '\0';
              strncat(buf, line + 43, 8);
              sat_info[i].MEAN_ANOMALY = atof(buf);

              buf[0] = '\0';
              strncat(buf, line + 52, 11);
              sat_info[i].MEAN_MOTION = atof(buf);

              buf[0] = '\0';
              strncat(buf, tle1_line + 33, 10);
              sat_info[i].TIME_MOTION_D = atof(buf);

              buf[0] = '\0';
              strncat(buf, line + 63, 5);
              sat_info[i].EPOCH_ORBIT = atoi(buf);


              //ISS (ZARYA)
              //          1         2         3         4         5         6
              //0123456789012345678901234567890123456789012345678901234567890123456789
              //1 25544U 98067A   19181.39493521 -.00156611  00000-0 -26708-2 0  9993
              //k lllllm bbyyyp   AABBBBBBBBBBBB IIIIIIIIII yyyyyyyy pppppppp b rrrrg

              //          1         2         3         4         5         6
              //0123456789012345678901234567890123456789012345678901234567890123456789
              //2 25544  51.6396 295.7455 0007987  98.4040   9.2643 15.51194651177305
              //b rrrrr CCCCCCCC DDDDDDDD EEEEEEE FFFFFFFF GGGGGGGG HHHHHHHHHHHJJJJJb


              //String  BIRD=               "ISS (ZARYA)";
              //#define YEAR                2019            // 20AA
              //#define EPOCH               181.39493521    // BBBBBBBBBBBB
              //#define INCLINATION         51.6396         // CCCCCCCC
              //#define RAAN                295.7455        // DDDDDDDD
              //#define ECCENTRICITY        0.0007987       // 0.EEEEEE
              //#define ARGUMENT_PEDIGREE   98.404          // FFFFFFFF
              //#define MEAN_ANOMALY        9.2643          // GGGGGGGG
              //#define MEAN_MOTION         15.51194651     // HHHHHHHHHHH
              //#define TIME_MOTION_D       -0.00156611     // IIIIIIIIII
              //#define EPOCH_ORBIT         17730           // JJJJJ
              //#define ONEPPM              1.0e-6
              //#define ONEPPTM             1.0e-7
              //              p13.setElements(YEAR, EPOCH, INCLINATION, RAAN, ECCENTRICITY * ONEPPTM, ARGUMENT_PEDIGREE,
              //                              MEAN_ANOMALY, MEAN_MOTION, TIME_MOTION_D, EPOCH_ORBIT + ONEPPM, 0);

              set_sat_info2(satname_line);
              print_sat_info(satname_line);

              stat = 0;
            } else {
              // sprintf(dp->lcdbuf,"Read TLE file \nSat.\n%s\nSkip.",satname_line);	upd_display_info_flash(dp->lcdbuf);
              if (verbose & 8) {
                plogw->ostream->print(satname_line);
                plogw->ostream->println(":not found");
              }
              stat = 0;
            }
            break;
	  }
          //PrintHex<uint8_t>(c, 0x80);plogw->ostream->print(" ");

          //if (count>=32) {
          //  plogw->ostream->println("");
          //  count=0;
          //}
      }
    }
  }
  tlefile.close();
  plogw->ostream->printf("TLE parse: records=%d matched=%d unmatched=%d\n",
                        tle_records, tle_matched, tle_records - tle_matched);
  strcpy(dp->lcdbuf, "Read TLE file \nFinished");
  upd_display_info_flash(dp->lcdbuf);
  if (!plogw->f_console_emu) plogw->ostream->println("\nend printing.");
}

// get tle information from network and store to tle file in SD memory.

void getTLE() {
  sat_tle_last_result = -1;
  plogw->ostream->println("getTLE()");

  if (f_sat_updated) return;
  if (wifi_status != 1) {
    request_sat_tle_parse(0);
    return;
  }

  // Download into a temporary file and replace tle.txt only after at least one
  // complete name/TLE1/TLE2 record has been received.  The HTTP body is parsed
  // incrementally; it is never accumulated in a String or heap buffer.
  const char *tmpfn = "/tle.new";
  const char *bakfn = "/tle.bak";
  if (SD.exists(tmpfn)) SD.remove(tmpfn);
  File out = SD.open(tmpfn, FILE_WRITE);
  if (!out) {
    plogw->ostream->println("opening temporary TLE file failed");
    return;
  }

  sprintf(dp->lcdbuf, "Connect TLE \nServer");
  upd_display_info_flash(dp->lcdbuf);
  if (!plogw->f_console_emu) plogw->ostream->print("[HTTP] GET begin...\n");

  // The AMSAT endpoint currently replies without Content-Length.  With the
  // default HTTP/1.1 keep-alive connection, http.connected() can remain true
  // after the complete body has arrived, so the old loop waited for its 10 s
  // idle timeout.  HTTP/1.0 requests connection-close framing and gives us an
  // unambiguous EOF without buffering the response.
  http.useHTTP10(true);
  http.setReuse(false);
  http.begin(sat_tle_url);
  if (!plogw->f_console_emu) plogw->ostream->print("[HTTP] GET...\n");
  int httpCode = http.GET();
  int valid_records = 0;
  bool download_ok = false;

  if (httpCode > 0) {
    if (!plogw->f_console_emu) plogw->ostream->printf("[HTTP] GET... code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient *stream = http.getStreamPtr();
      int len = http.getSize();
      if (!plogw->f_console_emu) plogw->ostream->printf("[HTTP] Content-Length=%d\n", len);

      out.println(my_rtc.unixtime());
      char line[160];
      char satname[128] = {0};
      char tle1[80] = {0};
      size_t line_len = 0;
      bool line_overflow = false;
      unsigned long last_rx = millis();
      unsigned long received = 0;

      bool rx_timeout = false;
      while (len != 0 && (http.connected() || stream->available())) {
        int ch = stream->read();
        if (ch < 0) {
          // For unknown-length responses EOF is the TCP close requested by
          // HTTP/1.0.  Keep a timeout only as a genuine stalled-transfer guard.
          if (!http.connected() && stream->available() == 0) break;
          if (millis() - last_rx > 10000UL) {
            rx_timeout = true;
            plogw->ostream->println("[HTTP] TLE receive timeout (stalled)");
            break;
          }
          delay(1);
          continue;
        }
        last_rx = millis();
        received++;
        if (len > 0) len--;

        if (ch == '\r') continue;
        if (ch != '\n') {
          if (line_len + 1 < sizeof(line)) line[line_len++] = (char)ch;
          else line_overflow = true;
          continue;
        }

        line[line_len] = '\0';
        if (!line_overflow && line_len > 0) {
          if (line[0] == '1') {
            if (line_len == 69 && satname[0]) strlcpy(tle1, line, sizeof(tle1));
            else tle1[0] = '\0';
          } else if (line[0] == '2') {
            if (line_len == 69 && satname[0] && tle1[0]) {
              out.println(satname);
              out.println(tle1);
              out.println(line);
              valid_records++;
              if ((valid_records & 7) == 0) {
                sprintf(dp->lcdbuf, "TLE received\nSat %d", valid_records);
                upd_display_info_flash(dp->lcdbuf);
              }
            }
            tle1[0] = '\0';
          } else {
            // A normal TLE name line.  Headers/comments are harmless: they are
            // replaced by the next name before a valid line 1/line 2 pair.
            strlcpy(satname, line, sizeof(satname));
            tle1[0] = '\0';
          }
        } else if (line_overflow && !plogw->f_console_emu) {
          plogw->ostream->println("[HTTP] overlong TLE line ignored");
        }
        line_len = 0;
        line_overflow = false;
      }

      // Handle a final non-newline-terminated line only for diagnostics; a
      // complete TLE record normally ends with line 2 + newline.
      out.flush();
      download_ok = (valid_records > 0);
      if (!plogw->f_console_emu)
        plogw->ostream->printf("[HTTP] TLE bytes=%lu records=%d eof=%s\n",
                              received, valid_records, rx_timeout ? "timeout" : "normal");
    }
  } else {
    plogw->ostream->printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  // Release HTTP/TCP resources before touching the satellite database.  This is
  // important on small heaps and also reduces overlap with AsyncTCP Web traffic.
  http.end();
  out.close();

  if (download_ok) {
    if (SD.exists(bakfn)) SD.remove(bakfn);
    bool had_old = SD.exists(tlefilename);
    bool old_saved = !had_old || SD.rename(tlefilename, bakfn);
    if (old_saved && SD.rename(tmpfn, tlefilename)) {
      if (SD.exists(bakfn)) SD.remove(bakfn);
      sat_tle_last_result = HTTP_CODE_OK;
      f_sat_updated = 1;
    } else {
      if (SD.exists(tmpfn)) SD.remove(tmpfn);
      if (had_old && SD.exists(bakfn) && !SD.exists(tlefilename)) SD.rename(bakfn, tlefilename);
      plogw->ostream->println("TLE file replace failed");
    }
  } else {
    if (SD.exists(tmpfn)) SD.remove(tmpfn);
    plogw->ostream->println("TLE download contained no complete records; old file kept");
  }

  plogw->ostream->println("end of getTLE");
  // Do not parse immediately after HTTPClient teardown.  Let lwIP/AsyncTCP and
  // any Web response callbacks return their buffers first.
  request_sat_tle_parse();
}



void plan13_test() {
  p13.setFrequency(435300000, 145920000);  //AO-51  frequency
  p13.setLocation(-64.375, 45.8958, 20);   // Sackville, NB
  p13.setTime(2009, 10, 1, 19, 5, 0);      //Oct 1, 2009 19:05:00 UTC
  p13.setElements(2009, 232.55636497, 98.0531, 238.4104, 83652 * 1.0e-7, 290.6047,
                  68.6188, 14.406497342, -0.00000001, 27022, 180.0);  //fairly recent keps for AO-51 //readElements();



  //ISS (ZARYA)
  //          1         2         3         4         5         6
  //0123456789012345678901234567890123456789012345678901234567890123456789
  //1 25544U 98067A   19181.39493521 -.00156611  00000-0 -26708-2 0  9993
  //k lllllm bbyyyp   AABBBBBBBBBBBB IIIIIIIIII yyyyyyyy pppppppp b rrrrg

  //          1         2         3         4         5         6
  //0123456789012345678901234567890123456789012345678901234567890123456789
  //2 25544  51.6396 295.7455 0007987  98.4040   9.2643 15.51194651177305
  //b rrrrr CCCCCCCC DDDDDDDD EEEEEEE FFFFFFFF GGGGGGGG HHHHHHHHHHHJJJJJb


  //void setElements(double YE_in, double TE_in, double IN_in, double
  //         RA_in, double EC_in, double WP_in, double MA_in, double MM_in,
  //    double M2_in, double RV_in, double ALON_in );


  p13.initSat();
  p13.satvec();
  p13.rangevec();
  p13.printdata();
  plogw->ostream->println();
  plogw->ostream->println("Should be:");
  plogw->ostream->println("AZ:57.07 EL: 4.05 RX 435301728 TX 145919440");
}

void save_satinfo() {
  const char *tmpfn = "/satdb.tmp";
  const char *bakfn = "/satdb.bak";

  SD.remove(tmpfn);
  File db = SD.open(tmpfn, FILE_WRITE);
  if (!db) {
    plogw->ostream->println("ERROR: cannot create /satdb.tmp");
    return;
  }

  for (int i = 0; i < N_SATELLITES; ++i) {
    if (sat_info[i].name[0] == '\0') continue;
    db.print(sat_info[i].name); db.print('\t');
    db.print(sat_info[i].up_f0); db.print('\t');
    db.print(sat_info[i].up_f1); db.print('\t');
    db.print(sat_info[i].up_mode); db.print('\t');
    db.print(sat_info[i].dn_f0); db.print('\t');
    db.print(sat_info[i].dn_f1); db.print('\t');
    db.print(sat_info[i].dn_mode); db.print('\t');
    db.print(sat_info[i].bc_f0); db.print('\t');
    db.println(sat_info[i].offset_freq);
  }
  db.flush();
  db.close();

  SD.remove(bakfn);
  if (SD.exists(satdbfilename)) {
    if (!SD.rename(satdbfilename, bakfn)) {
      plogw->ostream->println("ERROR: cannot backup /satdb.txt");
      SD.remove(tmpfn);
      return;
    }
  }
  if (!SD.rename(tmpfn, satdbfilename)) {
    plogw->ostream->println("ERROR: cannot install /satdb.txt");
    if (SD.exists(bakfn)) SD.rename(bakfn, satdbfilename);
    return;
  }
  SD.remove(bakfn);
}

void print_datetime(DateTime time) {
  char buf[30];
  sprintf(buf, "%02d/%02d %02d:%02d",
          time.month(),
          time.day(),
          time.hour(),
          time.minute());
  if (!plogw->f_console_emu) plogw->ostream->print(buf);
}

DateTime add_datetime(DateTime time, int seconds) {
  int year, month, day, hr, min, sec;

  year = time.year();
  month = time.month();
  day = time.day();
  hr = time.hour();
  min = time.minute();
  sec = time.second();

  sec += seconds;
  while (sec >= 60) {
    min++;
    sec -= 60;
  }
  while (min >= 60) {
    hr++;
    min -= 60;
  }
  while (hr >= 24) {
    day++;
    hr -= 24;
  }
  return DateTime(year, month, day, hr, min, sec);
}

void start_calc_nextaos() {
  //  plogw->ostream->println("NEXTAOS");
  //		plogw->nextaos_satidx=find_satname(radio->callsign+2+7);
  int i;
  for (i = 0; i < N_SATELLITES; i++) {
    if (*sat_info[i].name != '\0') break;
  }
  if (i < N_SATELLITES) {
    plogw->nextaos_satidx = i;
    plogw->f_nextaos = 1;
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("no satellite data");
    plogw->f_nextaos = 0;
  }
}

// satinfo[satidx] についてnextaos を調べ格納する。
void sat_find_nextaos_sequence() {
  int i;
  int satidx;
  satidx = plogw->nextaos_satidx;
  if (satidx == -1) return;
  char buf[20];

  switch (plogw->f_nextaos) {
    case 0:
      break;
    case 1:  //
      // start calculation
      set_sat_info_calc();
      // check if this sat_info has valid information

      if (sat_info[satidx].YEAR == 0) {
        if (!plogw->f_console_emu) {
          plogw->ostream->print("Not valid sat_info idx= ");
          plogw->ostream->print(satidx);
          plogw->ostream->println(" .. skip");
        }
        plogw->f_nextaos = 5;
        break;
      }
      if (verbose & 8) {
        plogw->ostream->print("nextaos calc sat information ");
        plogw->ostream->println(satidx);
      }
      print_sat_info_by_index(satidx);
      // check rtctime and los to see whether need for calculation
      //      if (compare_datetime(sat_info[satidx].nextlos, rtctime) < 0) {
      if (compare_datetime(sat_info[satidx].nextlos, my_rtc) < 0) {      
        //		  plogw->ostream->print("need to recalc AOS-LOS for satidx:");
        //		  plogw->ostream->println(satidx);
	//        sat_info[satidx].nextaos = rtctime;
	sat_info[satidx].nextaos = my_rtc;	
        plogw->nextaos_count = 0;
        plogw->f_nextaos = 2;
      } else {
        //		  plogw->ostream->print("no need to recalc AOS-LOS for satidx:");
        //		  plogw->ostream->println(satidx);
        plogw->f_nextaos = 5;
      }
      break;

    case 2:  // search aos

      sat_calc_position(satidx, sat_info[satidx].nextaos);
      // print result
      if (verbose & 8) {
        plogw->ostream->print("\nNEXTAOS satidx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextaos);
        //p13.printdata();

        plogw->ostream->print(" cnt=");
        plogw->ostream->print(plogw->nextaos_count);
        plogw->ostream->print(" AZ=");
        plogw->ostream->print(p13.AZ);

        plogw->ostream->print(" EL=");
        plogw->ostream->println(p13.EL);
      }

      if (p13.EL > 0) {
        // found aos
        plogw->f_nextaos = 3;
        plogw->nextaos_count = 0;
        sat_info[satidx].nextlos = sat_info[satidx].nextaos;
        sat_info[satidx].maxel = p13.EL;
      } else {
        // move to next candidate aos time
        if (p13.EL > -10) {
          sat_info[satidx].nextaos =
            add_datetime(sat_info[satidx].nextaos, 30);
        } else {
          sat_info[satidx].nextaos =
            add_datetime(sat_info[satidx].nextaos, 60 * 3);
        }

        plogw->nextaos_count++;
        if (plogw->nextaos_count > 900) {
          plogw->f_nextaos = 3;
        }
      }
      break;
    case 3:  // found aos and search los
      sat_calc_position(satidx, sat_info[satidx].nextlos);
      // print result
      if (verbose & 8) {
        plogw->ostream->print("\nNEXTLOS satidx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextlos);
        //p13.printdata();

        plogw->ostream->print(" cnt=");
        plogw->ostream->print(plogw->nextaos_count);
        plogw->ostream->print(" AZ=");
        plogw->ostream->print(p13.AZ);

        plogw->ostream->print(" EL=");
        plogw->ostream->println(p13.EL);
      }
      if (p13.EL < 0) {
        // found los
        plogw->f_nextaos = 4;
      } else {
        // move to next candidate aos time
        if (sat_info[satidx].maxel < p13.EL) {
          sat_info[satidx].maxel = p13.EL;
        }
        sat_info[satidx].nextlos =
          add_datetime(sat_info[satidx].nextlos, 30);

        plogw->nextaos_count++;
        if (plogw->nextaos_count > 900) {
          plogw->f_nextaos = 4;
        }
      }
      break;
    case 4:  // found nextlos
      if (!plogw->f_console_emu) {
        plogw->ostream->print("\nNEXTAOS idx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        plogw->ostream->print(sat_info[satidx].name);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextaos);
        plogw->ostream->print(" NEXTLOS ");
        print_datetime(sat_info[satidx].nextlos);
        plogw->ostream->println("");
      }
      plogw->f_nextaos = 5;
      break;
    case 5:  //
      // to the next satellite
      plogw->nextaos_satidx++;
      if (plogw->nextaos_satidx >= N_SATELLITES) {
        // no more satellite to calc
        plogw->f_nextaos = 0;
        plogw->nextaos_showidx = 0;
        print_sat_info_aos();  // print results

      } else {
        plogw->f_nextaos = 1;  // start calc for the next satellite
      }
      break;
  }

  // sat_calc_position() writes the shared p13 object.  The incremental AOS/LOS
  // search above evaluates future times and other satellites one step at a
  // time, while the OLED can be redrawn between those steps.  Always restore
  // p13 to the currently selected tracking satellite at the present time
  // before returning, so AZ/EL on the tracking screen never show a search
  // candidate's future position.
  const int track_idx = plogw->sat_idx_selected;
  if (plogw->f_nextaos != 0 && track_idx >= 0 && track_idx < N_SATELLITES &&
      sat_info[track_idx].YEAR != 0) {
    sat_calc_position(track_idx, my_rtc);
  }
}

void sat_calc_position(int satidx, DateTime time) {
  int i;
  i = satidx;
  p13.setElements(
    sat_info[i].YEAR,
    sat_info[i].EPOCH,
    sat_info[i].INCLINATION,
    sat_info[i].RAAN,
    sat_info[i].ECCENTRICITY * ONEPPTM,
    sat_info[i].ARGUMENT_PEDIGREE,
    sat_info[i].MEAN_ANOMALY,
    sat_info[i].MEAN_MOTION,
    sat_info[i].TIME_MOTION_D,
    sat_info[i].EPOCH_ORBIT,
    180);

  set_location_gl_calc(plogw->grid_locator_set);
  // JST->UTC conv just by -9 to hour
  p13.setTime(
    time.year(),
    time.month(),
    time.day(),
    time.hour() - 9,
    time.minute(),
    time.second());


  p13.initSat();
  p13.satvec();
  p13.rangevec();
}

void request_sat_tle_update() {
  if (!sat_tle_update_in_progress) { f_sat_updated = false; sat_tle_update_requested = true; }
}

void service_sat_tle_update() {
  if (sat_tle_update_requested && !sat_tle_update_in_progress) {
    sat_tle_update_requested = false;
    sat_tle_update_in_progress = true;
    getTLE();
    sat_tle_update_in_progress = false;
  }

  if (!sat_tle_parse_pending) return;
  const uint32_t now = millis();
  if ((int32_t)(now - sat_tle_parse_not_before_ms) < 0) return;

  if (!sat_tle_parse_heap_ready()) {
    if (now - sat_tle_parse_wait_log_ms >= 2000) {
      sat_tle_parse_wait_log_ms = now;
      const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      plogw->ostream->printf("TLE parse deferred: free=%u largest=%u async_q=%u\n",
                            (unsigned)free_now, (unsigned)largest,
                            (unsigned)asyncTCPQueueMessagesWaiting());
    }
    return;
  }

  sat_tle_parse_pending = false;
  plogw->ostream->printf("TLE parse start: free=%u largest=%u async_q=%u\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned)asyncTCPQueueMessagesWaiting());
  readtlefile();
  plogw->ostream->printf("TLE parse done: free=%u largest=%u\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void sat_process() {
  service_sat_tle_update();
  if (plogw->sat) {
    struct radio *radio;

    radio = radio_selected;

    // next-AOS search is serviced independently from main.cpp whenever a
    // job is active. Do not duplicate one search step here every 500 ms.
    set_sat_info_calc();
    set_location_gl_calc(plogw->grid_locator_set);

    // JST->UTC conv just by -9 to hour
    //    p13.setTime(rtctime.year(), rtctime.month(), rtctime.day(),
    //                rtctime.hour() - 9, rtctime.minute(), rtctime.second());
        p13.setTime(my_rtc.year(), my_rtc.month(),my_rtc.day(),
		    my_rtc.hour() - 9, my_rtc.minute(),my_rtc.second());

    p13.initSat();
    p13.satvec();
    p13.rangevec();

    // set rotator_target
    plogw->rotator_target_az = p13.AZ;

    set_sat_freq_calc();

    if (verbose & 8) {
      p13.printdata();
      plogw->ostream->println("");
    }
    // LCD redraw is intentionally serviced separately at 1 Hz from
    // processes.cpp.  Orbit/Doppler/rig tracking stays at 500 ms.
  }
}
