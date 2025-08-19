/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2025 Eiichiro Araki
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
#include "log.h"


enum QueryCIVType {Freq,Mode,Smeter,Ptt,Id,Preamp,Gps,Att};
void send_query_civ(enum QueryCIVType type,struct radio *radio) {
  switch(type) {
  case Freq:		send_freq_query_civ(radio);break;//0
  case Mode:		send_mode_query_civ(radio);break;//1
  case Smeter:	    send_smeter_query_civ(radio);//2
  case Ptt:		send_ptt_query_civ(radio);break;//3
  case Att:		send_att_query_civ(radio);break;//3    
  case Id:	      send_identification_query_civ(radio);  // 5
  case Preamp:		send_preamp_query_civ(radio);break;   //7 
  case Gps:		send_gps_query_civ(radio); break; 
  }
}


int interval_process_stat = 0;
void interval_process() {
  struct radio *radio;
  int next_interval;
  static int query_item = 0;
  next_interval = 100;
  if (timeout_interval < millis()) {
    if (verbose & 256) {
      if (so2r.repeat_timer()!=0) {
	plogw->ostream->print("repeat timer=");
	//      plogw->ostream->print(plogw->repeat_func_timer);
	plogw->ostream->print(so2r.repeat_timer());
	plogw->ostream->print("stat ");
	plogw->ostream->println(so2r.sequence_stat());
      }
    }
    if (bandmap_disp.f_update) {
      upd_display_bandmap();
      bandmap_disp.f_update = 0;
    }

    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      if (!radio->enabled) continue;
      // query item map
      // query_items[]={ 0,1,2,3,4,5, 0,1,2,3,4,6, 0,1,2,3,4,7, 0,1,2,3,4,5
      if (!radio->f_civ_response_expected) {
	if (!plogw->f_show_signal) {
	  // normal 
	  switch (interval_process_stat) {  
	  case 0:
	    send_query_civ(Freq,radio); break;
	  case 1:
	    send_query_civ(Mode,radio); break;
	    break;
	  case 2:
	    send_query_civ(Smeter,radio); break;	    
	    break;
	  case 3:
	    //	    send_query_civ(Ptt,radio); break;
	    send_query_civ(Gps,radio); break;       // 6	      	    
	    break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // ATT query or other information query
	    switch(query_item) {
	    case 0:send_query_civ(Id,radio); break;	    	    
	    case 1:send_query_civ(Ptt,radio); break;       // 6
	    case 2:send_query_civ(Freq,radio); break;       // 6
	    case 3:send_query_civ(Mode,radio); break;       // 6
	    case 4:send_query_civ(Smeter,radio); break;	    
	    case 5:send_query_civ(Preamp,radio); break;       // 6
	    case 6:send_query_civ(Ptt,radio); break;        // 6
	    case 7:send_query_civ(Freq,radio); break;       // 6
	    case 8:send_query_civ(Mode,radio); break;       // 6
	    case 9:send_query_civ(Smeter,radio); break;	    
	    case 10:send_query_civ(Att,radio); break;       // 6
	    case 11:send_query_civ(Gps,radio); break;       // 6	      
	    query_item++;
	    if (query_item >= 12) query_item = 0;
			
	    break;
	    }
	  }
	} else {
	  // show signal 
	  switch (interval_process_stat) {  
	  case 0:
	  case 1:
	  case 2:
	  case 3:
	    send_query_civ(Gps,radio); break;       // 6	      	    	    
	    //	    send_smeter_query_civ(radio);//2
	    break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // ATT query or other information query 
	    switch(query_item) {
	    case 0:send_query_civ(Id,radio); break;	    	    
	    case 1:send_query_civ(Ptt,radio); break;       // 6
	    case 2:send_query_civ(Freq,radio); break;       // 6
	    case 3:send_query_civ(Mode,radio); break;       // 6
	    case 4:send_query_civ(Smeter,radio); break;	    
	    case 5:send_query_civ(Preamp,radio); break;       // 6
	    case 6:send_query_civ(Ptt,radio); break;        // 6
	    case 7:send_query_civ(Freq,radio); break;       // 6
	    case 8:send_query_civ(Mode,radio); break;       // 6
	    case 9:send_query_civ(Smeter,radio); break;	    
	    case 10:send_query_civ(Att,radio); break;       // 6
	    case 11:send_query_civ(Gps,radio); break;       // 6	      
	      query_item++;
	      if (query_item >= 12) query_item = 0;
	      break;
	    }
	  }
	}
      }
    }		


    if (interval_process_stat == 4) {
      //      rotator_process();
    }

    interval_process_stat++;
    if (interval_process_stat > 5) interval_process_stat = 0;
    timeout_interval = millis() + next_interval;
    //	plogw->ostream->print("PTT:");plogw->ostream->print(plogw->ptt_stat);plogw->ostream->print(" S_stat:");plogw->ostream->println(plogw->smeter_stat);
  }

  // satellite process 500ms
  if (timeout_interval_sat < millis()) {
    sat_process();
    timeout_interval_sat = millis() + 500;
  }

  // temporally timeout rig enable
  if (timeout_rig_disable_temporally < millis()) {
    struct radio *radio;
    radio=so2r.radio_selected();
    if (!radio->enabled) {
      enable_radios(so2r.focused_radio(),1);
    }
  }

  // second process
  if (timeout_second < millis()) {
    // interval job every second
    //    Serial.print("receive_civport nmax in 1ms interrupt.:");
    //    Serial.println(receive_civport_count);

    receive_civport_count=0;
    timeout_second = millis() + 1000;

    if (verbose & VERBOSE_MEM) {
      console->printf("DMA free block=%6d\n",heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    // check transport status and send command
    mux_transport.sync_transport_modes_master(); // send transport command
    

    // print_time_measure results and clear counter
    //    usb_task_memory_watermark=uxTaskGetStackHighWaterMark(gxHandle_USBloop);
    //    plogw->ostream->print("usb task mem=");plogw->ostream->print(usb_task_memory_watermark);
    if (verbose & 1024) plogw->ostream->print(" usec:");
    char tmp_buf[10];
    for (int i=0;i<15;i++) {
      sprintf(tmp_buf,"%4d ",time_measure_get(i));
      if (verbose & 1024)       plogw->ostream->print(tmp_buf);
      time_measure_clear(i);
    }
    if (verbose & 1024) {
      plogw->ostream->print(" revs=");
      plogw->ostream->print(main_loop_revs);
      plogw->ostream->println("");
    }
    main_loop_revs=0;

  }

  if (timeout_cat < millis()) {

    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      //      console->print(i);      
      receive_cat_data(radio);
    }

    timeout_cat = millis() + 50;
  }


  if (timeout_interval_minute < millis()) {
    // minute processes

    // remove old bandmap entry for all bands
    int i;
    for (i = 0; i < N_BAND; i++) {
      delete_old_entry(i, 20);
    }
    delete_old_entry(N_BAND, 5);
    
    bandmap_disp.f_update = 1;
    timeout_interval_minute = millis() + 60000;

    // frequency notification to zserver
    zserver_freq_notification();
    
    
  }
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

