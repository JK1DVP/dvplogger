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
#include "bandmap.h"
#include "cat.h"
#include "display.h"
#include "ui.h"
#include "so2r.h"
#include "qso.h"
#include "zserver.h"
#include "satellite.h"
#include "misc.h"
#include "main.h"
#include "processes.h"
#include "cw_keying.h"
#include "esp32_flasher.h"
#include "mcp_interface.h"
#include "log.h"
#include "antenna.h"
#include "dupechk.h"
#include "mux_transport.h"
#include "callhist_remote.h"

enum QueryCIVType {Freq,Mode,Smeter,SWR,Ptt,Id,Preamp,Gps,Att,Power,RigAnt,ScopeLevel};
void send_query_civ(enum QueryCIVType type,struct radio *radio) {
  switch(type) {
  case Freq:		send_freq_query_civ(radio);break;//0
  case Mode:		send_mode_query_civ(radio);break;//1
  case Smeter:	    send_smeter_query_civ(radio);break;//2
  case SWR:          send_swr_query_civ(radio);break;
  case Ptt:		send_ptt_query_civ(radio);break;//3
  case Att:		send_att_query_civ(radio);break;//3    
  case Id:	      send_identification_query_civ(radio);break;  // 5
  case Preamp:		send_preamp_query_civ(radio);break;   //7 
  case Gps:		send_gps_query_civ(radio); break; 
  case Power:   send_power_query_civ(radio); break;
  case RigAnt:  send_rig_antenna_query(radio); break;
  case ScopeLevel: send_yaesu_scope_level_query(radio); break;
  }
}



static void process_subcpu_late_recovery()
{
  if (subcpu_online || !f_mux_transport) return;

  static uint32_t next_probe_ms = 0;
  const uint32_t now = millis();
  if (next_probe_ms != 0 && (int32_t)(now - next_probe_ms) < 0) return;
  next_probe_ms = now + 10000U;

  if (!callhist_subcpu_alive(120)) return;

  subcpu_online = true;
  const bool want_dupe_subcpu = dupechk_setting_wants_subcpu();

  if (want_dupe_subcpu) {
    console->println(
      "SUBCPU late recovery: response detected; DUPE rebuild scheduled");
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "SUBCPU FOUND\nRebuilding DUPE...");
    upd_display_info_flash(dp->lcdbuf);

    init_dupechk_maincpu();
    // Use the same deferred rebuild path as startup/contest changes.
    request_makedupe_rebuild();
  } else {
    console->printf(
      "SUBCPU late recovery: response detected; DUPE remains MAIN (setting=%d)\n",
      dupechk_at);
  }

  // SUBCPU recovery must not silently move CALLHIST.  callhist_at is the
  // already-resolved CALLHIST placement and is independent of DUPE.
  if (plogw->enable_callhist && callhist_at == 1) {
    bool fallback = false;
    int n = load_callhist_subcpu_or_main(callhistfn, &fallback);
    if (n > 0) {
      console->printf("SUBCPU RECOVERED: CALLHIST=%s entries=%d\n",
                      fallback ? "MAIN-PSRAM(fallback)" : "SUBCPU", n);
    } else {
      console->println("SUBCPU RECOVERED: CALLHIST disabled (no usable memory)");
    }
  } else if (plogw->enable_callhist) {
    console->println("SUBCPU RECOVERED: CALLHIST remains MAIN");
  } else {
    console->println("SUBCPU RECOVERED: CALLHIST=OFF");
  }

  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "SUBCPU RECOVERED\nDUPE: %s\nCALLHIST: %s",
           want_dupe_subcpu ? "rebuilding" : "MAIN",
           plogw->enable_callhist ? (callhist_at == 1 ? "SUBCPU" : "MAIN") : "OFF");
  upd_display_info_flash(dp->lcdbuf);
}

int interval_process_stat = 0;

// Lightweight interval_process() latency diagnostics.
// This deliberately does not use the normal profile banks because the normal
// profile report itself is printed from interval_process().  When one call is
// slower than INTERVAL_DIAG_THRESHOLD_US, print the slowest internal stage.
static const uint32_t INTERVAL_DIAG_THRESHOLD_US = 100000;

struct IntervalDiag {
  uint32_t start_us;
  uint32_t mark_us;
  uint32_t max_us;
  const char *max_stage;
};

static inline void interval_diag_begin(IntervalDiag *diag) {
  diag->start_us = micros();
  diag->mark_us = diag->start_us;
  diag->max_us = 0;
  diag->max_stage = "start";
}

static inline void interval_diag_mark(IntervalDiag *diag, const char *stage) {
  const uint32_t now = micros();
  const uint32_t elapsed = now - diag->mark_us;
  if (elapsed > diag->max_us) {
    diag->max_us = elapsed;
    diag->max_stage = stage;
  }
  diag->mark_us = now;
}

static inline void interval_diag_finish(IntervalDiag *diag) {
  const uint32_t total = micros() - diag->start_us;
  if (total >= INTERVAL_DIAG_THRESHOLD_US) {
    // Use the hardware serial port rather than console/telnet output so a
    // blocked network stream does not hide or amplify the diagnosis.
    console->printf("INTERVAL SLOW total=%lu us max=%lu us stage=%s stat=%d wifi=%d core=%d\n",
                  (unsigned long)total,
                  (unsigned long)diag->max_us,
                  diag->max_stage,
                  interval_process_stat,
                  wifi_enable,
                  xPortGetCoreID());
  }
}

void interval_process() {
  IntervalDiag interval_diag;
  interval_diag_begin(&interval_diag);
  struct radio *radio;
  int next_interval;
  next_interval = 100;
  service_icom_clock_sync();
  service_yaesu_scope();
  antenna_process();
  interval_diag_mark(&interval_diag, "antenna");
  if (f_mux_transport) mux_transport.recv_pkt();
  if (timeout_interval < millis()) {
    if (verbose & VERBOSE_SEQUENCE) {
      if (so2r.repeat_timer()!=0) {
	plogw->ostream->print("repeat timer=");
	//      plogw->ostream->print(plogw->repeat_func_timer);
	plogw->ostream->print(so2r.repeat_timer());
	plogw->ostream->print("stat ");
	plogw->ostream->println(so2r.sequence_stat());
      }
    }

    if (bandmap_disp.f_update && !dupechk_remote_query_pending()) {
      // Consume the legacy flag before translating it into the single
      // on-demand request path.
      bandmap_disp.f_update = 0;
      request_bandmap_update_on_demand();
      interval_diag_mark(&interval_diag, "bandmap_request");
    }
    if (!dupechk_remote_query_pending()) {
      if (f_mux_transport) mux_transport.recv_pkt();
      interval_diag_mark(&interval_diag, "pre_display_mux");

      const uint32_t display_info_started_us = micros();
      upd_display_info();// update info_display (when timer==0)
      const uint32_t display_info_elapsed_us = micros() - display_info_started_us;
      if (display_info_elapsed_us >= 80000U) {
        console->printf(
            "SLOWDETAIL display_info call=%lu us timer=%d show=%d stat=%d core=%d\n",
            (unsigned long)display_info_elapsed_us,
            info_disp.timer, info_disp.show_info, interval_process_stat,
            xPortGetCoreID());
      }
      interval_diag_mark(&interval_diag, "display_info_call");

      if (f_mux_transport) mux_transport.recv_pkt();
      interval_diag_mark(&interval_diag, "post_display_mux");
    } else {
      interval_diag_mark(&interval_diag, "display_skipped_remote_dupe");
    }
    
    /*
     * Unified weighted CAT/CI-V poll scheduler (100-ms slots).
     *
     * Yaesu ASCII uses a compact 4-slot cycle:
     *   0 IF
     *   1 TX -> response-driven SM0 (RX) / RM6 (TX)
     *   2 IF
     *   3 SLOW
     *
     * Thus IF runs at 5 Hz, TX+meter at 2.5 Hz, and slow/status requests
     * are admitted at 2.5 Hz aggregate.  TX and meter are one logical poll:
     * cat.cpp waits for the TX answer, parses it, then immediately queues the
     * correct meter without waiting for the next 100-ms interval slot.
     *
     * Other CAT/CI-V protocols retain the conservative 10-slot schedule.
     */
    static const QueryCIVType slow_common[] = {
      Id, Ptt, Mode, Smeter, Preamp, Ptt, Mode, Smeter, Att, Gps,
      Power, RigAnt, ScopeLevel
    };
    static const QueryCIVType slow_yaesu[] = {
      Id, Preamp, Att, Power, RigAnt, ScopeLevel
    };
    static uint8_t slow_common_pos[N_RADIO] = {0};
    static uint8_t slow_yaesu_pos[N_RADIO] = {0};

    // Satellite frequency monitoring has a tighter cadence than the normal
    // weighted poller.  IC-9700 MAIN/SUB and two-radio RX/TX are alternated,
    // giving each side a 200-ms observation period while Doppler calculation
    // remains on the existing 500-ms satellite timer.
    const bool sat_poll_used = sat_frequency_monitor_process();

    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      if (!radio->enabled || radio->rig_spec == NULL) continue;
      autotuner_service(radio);
      if (sat_poll_used) continue;

      const bool yaesu_ascii =
          radio->rig_spec->cat_type == CAT_TYPE_YAESU_NEW ||
          radio->rig_spec->cat_type == CAT_TYPE_YAESU_OLD;

      // Yaesu ASCII has its own one-query-at-a-time serializer.  Other
      // protocols retain the legacy response gate.
      if (!yaesu_ascii && radio->f_civ_response_expected) continue;

      if (radio->rig_spec->no_polling) {
        // Keep the existing 100-ms scheduler and slot layout unchanged.
        // NP:1 suppresses normal periodic traffic.  A program-originated SET
        // may arm one confirmation readback, which is sent only when the
        // corresponding existing interval slot arrives.
        if (yaesu_ascii) {
          // Yaesu IF; reports frequency and mode together.
          switch (interval_process_stat & 3) {
          case 0:
          case 2:
            if (radio->freq_readback_once_pending ||
                radio->mode_readback_once_pending) {
              send_query_civ(Freq, radio);
              radio->freq_readback_once_pending = false;
              radio->mode_readback_once_pending = false;
            }
            break;
          default:
            break;
          }
        } else {
          switch (interval_process_stat) {
          case 0:
          case 2:
          case 5:
          case 8:
            if (radio->freq_readback_once_pending) {
              send_query_civ(Freq, radio);
              radio->freq_readback_once_pending = false;
            }
            break;
          case 6:
          case 9:
            if (radio->mode_readback_once_pending) {
              send_query_civ(Mode, radio);
              radio->mode_readback_once_pending = false;
            }
            break;
          default:
            break;
          }
        }
        continue;
      }

      if (yaesu_ascii) {
        switch (interval_process_stat & 3) {
        case 0:
        case 2:
          send_query_civ(Freq, radio);       // IF;
          break;
        case 1:
          request_yaesu_tx_meter_poll(radio); // TX; -> SM0;/RM6;
          break;
        case 3: {
          const size_t n = sizeof(slow_yaesu) / sizeof(slow_yaesu[0]);
          send_query_civ(slow_yaesu[slow_yaesu_pos[i]], radio);
          slow_yaesu_pos[i] = (slow_yaesu_pos[i] + 1) % n;
          break;
        }
        }
      } else {
        switch (interval_process_stat) {
        case 0:
        case 2:
        case 5:
        case 8:
          send_query_civ(Freq, radio);
          break;
        case 1:
        case 4:
          send_query_civ(radio_tx_meter_active(radio) ? SWR : Smeter, radio);
          break;
        case 6:
        case 9: {
          const size_t n = sizeof(slow_common) / sizeof(slow_common[0]);
          send_query_civ(slow_common[slow_common_pos[i]], radio);
          slow_common_pos[i] = (slow_common_pos[i] + 1) % n;
          break;
        }
        case 3:
        case 7:
        default:
          break;
        }
      }
    }
    interval_diag_mark(&interval_diag, "radio_queries");
    if (f_mux_transport) mux_transport.recv_pkt();
    interval_diag_mark(&interval_diag, "post_radio_mux");


    if (interval_process_stat == 4) {
      //      rotator_process();
    }

    interval_process_stat++;
    if (interval_process_stat > 9) interval_process_stat = 0;
    timeout_interval = millis() + next_interval;
    //	plogw->ostream->print("PTT:");plogw->ostream->print(plogw->ptt_stat);plogw->ostream->print(" S_stat:");plogw->ostream->println(plogw->smeter_stat);
  }
  interval_diag_mark(&interval_diag, "interval_100ms");

  // satellite process 500ms
  if (timeout_interval_sat < millis()) {
    if (f_mux_transport) mux_transport.recv_pkt();
    sat_process();
    if (f_mux_transport) mux_transport.recv_pkt();
    interval_diag_mark(&interval_diag, "sat_process");
    timeout_interval_sat = millis() + 500;
  }
  interval_diag_mark(&interval_diag, "satellite_gate");

  // Re-enable only the radio that Alt-I temporarily disabled.
  // Radios disabled explicitly with Alt-End / Shift-Alt-End / Ctrl-Alt-End
  // must remain disabled.
  if (temporarily_disabled_radio >= 0 &&
      timeout_rig_disable_temporally < millis()) {
    int idx_radio = temporarily_disabled_radio;
    enable_radios(idx_radio,1);
  }
  interval_diag_mark(&interval_diag, "rig_reenable");

  // second process
  if (timeout_second < millis()) {
    // interval job every second
    //    console->print("receive_civport nmax in 1ms interrupt.:");
    //    console->println(receive_civport_count);

    receive_civport_count=0;
    if (plogw->autopoweroff) {
      plogw->count_autopoweroff++;
      if (verbose&4) {
	console->print("count_autopoweroff=");console->print(plogw->count_autopoweroff);
	console->print(" autopoweroff=");console->println(plogw->autopoweroff);
      }
      if (plogw->autopoweroff< plogw->count_autopoweroff) {
	// power down
	sprintf(dp->lcdbuf,"Auto powerdown\nafter %d sec \ninactive.\n",plogw->count_autopoweroff);
	console->print(dp->lcdbuf);
	upd_display_info_flash(dp->lcdbuf);
	
	// subcpu put to reset state
	//	mcp_write_pin(reset_trigger_mcp_pin, 0);
	mcp_write_pin(15, 0);	
	// main cpu deep sleep
	esp_deep_sleep(3600000000UL); // 1 hr deep sleep 
      }
    }
    
    interval_diag_mark(&interval_diag, "autopower");
    timeout_second = millis() + 1000;

    if (verbose & VERBOSE_MEM) {
      console->printf("DMA free block=%6d\n",heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    // check transport status and send command
    mux_transport.sync_transport_modes_master(); // send transport command
    process_subcpu_late_recovery();
    interval_diag_mark(&interval_diag, "mux_sync");
    

    // print_time_measure results and clear counter
    //    usb_task_memory_watermark=uxTaskGetStackHighWaterMark(gxHandle_USBloop);
    //    plogw->ostream->print("usb task mem=");plogw->ostream->print(usb_task_memory_watermark);
    if (verbose & VERBOSE_PERF) {
      plogw->ostream->print(" profile:");
      for (int i = 0; i < PROF_BANK_COUNT; ++i) {
        const char *name = time_measure_get_name(i);
        if (name[0] == '\0') continue;
        plogw->ostream->print(' ');
        plogw->ostream->print(name);
        plogw->ostream->print('=');
        plogw->ostream->print(time_measure_get(i));
      }
      plogw->ostream->print(" revs=");
      plogw->ostream->println(main_loop_revs);

      // max/total/calls for each bank.  The existing max-only line is kept
      // for easy comparison with older logs.  total is accumulated CPU time
      // in us during this reporting window.
      plogw->ostream->print(" profile_tc:");
      for (int i = 0; i < PROF_BANK_COUNT; ++i) {
        const char *name = time_measure_get_name(i);
        if (name[0] == '\0') continue;
        plogw->ostream->print(' ');
        plogw->ostream->print(name);
        plogw->ostream->print('=');
        plogw->ostream->print(time_measure_get(i));
        plogw->ostream->print('/');
        plogw->ostream->print((unsigned long long)time_measure_get_total(i));
        plogw->ostream->print('/');
        plogw->ostream->print(time_measure_get_calls(i));
      }
      plogw->ostream->println();
      interval_diag_mark(&interval_diag, "profile_print");
    }
    for (int i = 0; i < PROF_BANK_COUNT; ++i) time_measure_clear(i);
    main_loop_revs=0;
    interval_diag_mark(&interval_diag, "profile_clear");

  }
  interval_diag_mark(&interval_diag, "second_gate");

  // Receive ASCII CAT data from the configured ports.
  for (int i = 0; i < N_RADIO; i++) {
    if (!unique_num_radio(i)) continue;

    radio = &radio_list[i];

    if (!radio->enabled || radio->rig_spec == NULL) continue;

    if (radio->rig_spec->cat_type != 0) {
      // Yaesu / Kenwood / other ASCII CAT over USB/SoftwareSerial.
      receive_cat_data(radio);
    }
  }
  interval_diag_mark(&interval_diag, "ascii_cat");

  // Receive CI-V data at the existing 50 ms service interval.
  if (timeout_cat < millis()) {
    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;

      radio = &radio_list[i];

      if (!radio->enabled || radio->rig_spec == NULL) continue;

      if (radio->rig_spec->cat_type == 0) {
        receive_civport(radio);
      }
    }
    timeout_cat = millis() + 50;
  }
  interval_diag_mark(&interval_diag, "civ_receive");


  if (timeout_interval_minute < millis()) {
    // minute processes

    // remove old bandmap entry for all bands
    int i;
    for (i = 1; i < N_BAND; i++) {
      delete_old_entry(i, bandmap_lifetime_minutes);
    }
    // Keep the special all-band/new-entry list at its existing short lifetime.
    delete_old_entry(N_BAND, 5);
    
    bandmap_disp.f_update = 1;
    timeout_interval_minute = millis() + 60000;

    // frequency notification to zserver
    zserver_freq_notification();
    interval_diag_mark(&interval_diag, "minute_jobs");
    
    
  }
  interval_diag_mark(&interval_diag, "minute_gate");

  interval_diag_finish(&interval_diag);
  
}


// written in decl.h
//  // SO-2R related
//  int so2r_tx; // selected tx 0,1
//  int so2r_rx; // selected rx 0,1
//  int so2r_stereo; // stereo rx 0,1
//  int focused_radio; // radio currently focused
//  int focused_radio_prev; // previously focused radio will be saved to here (used on toggling stereo mode to select both current focus and previous focus)
//  // \ (backspace) will switch actively receiving radio but not transmitting radio
//  //   at the same time, switch focused display by changing radio_selected.


//  int radio_mode ; // 0 so1r  1 sat 2 two radio (main transmit sub receive)
//  int sequence_mode ; // 0 manual 1 repeat function 2 auto cq+s&p  3 dueling CQ (alternate CQ) 4 2BSIQ  (wait sending until the other send finishes)
//  // sequence mode, radio_mode 1に基づき f_repeat_func_stat を制御しつつradio0, radio1 の制御を行う。
//  // 制御は、process.cpp のsequence_manager()  sequence_manager_timer_expired() で行う。sequence_manager_cancel_repeat() で0 manual に戻る。repeat でcancelをした際radioの切り替えは行わない。


// repeat_func_radio is now the which sends message so always set when function_keys is called. (--> change to msg_tx_radio )
// f_repeat_func_stat holds status of the message sending to control SO2R and repeat functions  (--> change to f_sequence_stat )
// radio_mode is SAT, SO1R, SO2R  in SO1R changing radio will stop sending message
//                          in SO2R keep sending message in so2r_tx and finishing sending message rx,focus comeback to the sent message radio, esc suspends sending message in so2r_tx but focus not change
// set_tx_to_focused() in cw_keying bring back tx to the focused radio and recommended to use
// cancel_current_tx_message stops keying and recommended to use
// append_cwbuf() now always based on so2r_tx radio regardless of the focus 

// sequence
// ui_send_cq etc , or function_keys send message and prior to this set repeat_func_stat and repeat_func_radio
//       this also may change status of the rx and focused ( when rx change occurs focus will also changed in SO2R_setrx)
// when message send completion detected ( in cw by $ and in phone check CAT TX status ), depending on the sequence_mode,
//  bring rx, and focus  back to the message sent tx and would start timer (repeat func) or tx another message at once (SO2R sequence mode dependency)
//  after timer expired, message send with the repeat_func_key in the repeat_func_radio(?)

// may better define SO2R class to hold all these status and functions (so2r_tx, rx, stereo, msg_tx_radio f_sequence_stat and focused_radio, ui_send_cq
