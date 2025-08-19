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

// memo
// behavior in So2r keep cq alternating radio rather than (modal)
// even in ESM mode
// may be effective switch rig always when sending response
// S&P sending cancel cqing in the main rig and send message
// these transition logic should be well defined 
// we need to enable radio to switch
// show bandmap back to cq mode
// rig does not set 2nd rig ???  serial is not initialized correctly by loading
// bandmask is strange for IC-9700
// sub main rig choise should depend on the bandmask for each rig

// selecting ic rigs select Radio4 ... strange

// interesting on am carrier frequency change
// mode change send band scope width! --> inplemented need to test

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "so2r.h"
#include "cat.h"
#include "cw_keying.h"
#include "bandmap.h"
#include "ui.h"
#include "cw_keying.h"
#include "settings.h"
#include "display.h"
#include "edit_buf.h"
#include "dupechk.h"
#include "multi.h"
#include "multi_process.h"
#include "qso.h"
#include "log.h"
#include "misc.h"
#include "cluster.h"
#include "callhist.h"
#include <hidboot.h>
#include "SD.h"
#include "contest.h"
#include "keyboard.h"
#include "satellite.h"
#include "sd_files.h"
#include "cmd_interp.h"
#include "network.h"
#include "zserver.h"
#include "cty_chk.h"
#include "iambic_keyer.h"
#include "mux_transport.h"
#include "timekeep.h"
#include "processes.h"
#include "so2r.h"
#include "dac-adc.h"
#include "morse_decoder_simple.h"


int ui_cursor_next_entry_partial_check (struct radio *radio)
{
  // partial check cursor move to the next entry
  radio->check_entry_list.cursor++;
  if (radio->check_entry_list.cursor>=radio->check_entry_list.nentry) {
    radio->check_entry_list.cursor=0;
  }
  display_partial_check(radio);
  return 0;
}

int ui_pick_partial_check(struct radio *radio)
{
  plogw->ostream->println("ui_pick_partial_check()");
  strcpy(radio->callsign+2,radio->check_entry_list.entryl[radio->check_entry_list.cursor].callsign);
  radio->callsign[1]=strlen(radio->callsign +2);
  strcpy(radio->recv_exch + 2, radio->check_entry_list.entryl[radio->check_entry_list.cursor].exch);
  radio->recv_exch[1]=strlen(radio->recv_exch+2);
  upd_display();
  return 0;
}

int ui_toggle_partial_check_flag()
{
  // toggle partial check
  if (plogw->f_partial_check & PARTIAL_CHECK_CALLSIGN_AUTO) {
    plogw->f_partial_check &=~PARTIAL_CHECK_CALLSIGN_AUTO;
  } else {
    plogw->f_partial_check |=PARTIAL_CHECK_CALLSIGN_AUTO;		
  }
  sprintf(dp->lcdbuf,"partial chk=%d\n",plogw->f_partial_check);
  upd_display_info_flash(dp->lcdbuf);	      
  return 0;
}

int ui_perform_exch_partial_check(struct radio *radio)
{

  if (plogw->f_partial_check&PARTIAL_CHECK_CALLSIGN_AUTO) {
    //partial check

    // 3 characters
    // partial check
    int nentry;
    int flag;
		  
    // check
    radio->check_entry_list.cursor=0; // reset cursor to top
    radio->check_entry_list.nmax_entry=5;
    exch_partial_check(radio,radio->recv_exch+2,bandmode(radio),plogw->mask,1,&radio->check_entry_list);
    display_partial_check(radio);
  }
  return 0;

}


int ui_perform_partial_check(struct radio *radio)
{
  //  log_d(VERBOSE_UI,"ui_perform_partial_check()\n");
  if (plogw->f_partial_check&PARTIAL_CHECK_CALLSIGN_AUTO) {
    //partial check

    // 3 characters
    // partial check
    int nentry;
    int flag;
		  
    if (strlen(radio->callsign + 2) >=3) {
      if (strcmp(radio->callsign_prev+2,radio->callsign+2)==0) {
      } else {
	// new check
	radio->check_entry_list.cursor=0; // reset cursor to top
	strcpy(radio->callsign_prev+2,radio->callsign+2); // store current callsign to callsign_prev
	radio->check_entry_list.nmax_entry=5;
	dupe_partial_check(radio->callsign+2,bandmode(radio),plogw->mask,1,&radio->check_entry_list);
      }
      display_partial_check(radio);		

    }
  }
  return 0;
}


int ui_response_call_and_move_to_exch(struct radio *radio)
{
  so2r.send_call_exch();
  return 0;
}  




int ui_send_mycall(struct radio *radio)
{
  // repeat function off
  so2r.cancel_msg_tx();
      
  so2r.set_msg_tx_to_focused();
  //  so2r.set_rx_in_sending_msg();
  if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying) ) { 
    // send my callsign
    strcpy(radio->callsign_previously_sent, radio->callsign + 2);
    append_cwbuf_string(plogw->cw_msg[3] + 2);  // F4 my callsign
    append_cwbuf('$');     
  } else if (radio->modetype== LOG_MODETYPE_PH) {
    if (plogw->voice_memory_enable>=2) {
      send_voice_memory(radio, 4);  // F4 my callsign
    }
  } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
    // send call and exchange , then move to my exch entry
    set_rttymemory_string(radio, 4, plogw->rtty_msg[3] + 2);  // set rtty memory on rig
    // and send it on the air
    //                          delay(200);
    //                          send_rtty_memory(radio, 5);
  }

  return 0;
}


void show_cw_spd()
{
      sprintf(dp->lcdbuf, "CW_SPD=%d\n", cw_spd);
      upd_display_info_flash(dp->lcdbuf);
      if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R) {
	if (plogw->f_so2rmini) {
	  //            tmpbuf[0] = 0x02;
	  //            tmpbuf[1] = cw_spd;
	  //            SO2Rsend((uint8_t *)tmpbuf, 2);
	}
      }
  
}

int modkey_shift(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftShift == 1) || (modkey.bmRightShift == 1)) {  
    return 1;
  } else {
    return 0;
  }
}

int modkey_ctrl(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftCtrl == 1) || (modkey.bmRightCtrl == 1) || (f_capslock)) {
    return 1;
  } else {
    return 0;
  }
}
int modkey_alt(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftAlt == 1) || (modkey.bmRightAlt == 1)) {
    return 1;
  } else {
    return 0;
  }
}

void send_single_char_radio(struct radio *radio,char c)
{
  if (radio->modetype == LOG_MODETYPE_CW || (radio->f_tone_keying)) {
    // CW
#if JK1DVPLOG_HWVER >=2
    if (radio->f_tone_keying) {
      char buf[5];
      c=append_cwbuf_convchar(c);
      sprintf(buf,"%c",c);
      play_wpm_set();
      play_cw_cmd(buf);
    } else {
      append_cwbuf(c);
    }
#else
      append_cwbuf(c);    
#endif
  } else if (radio->modetype == LOG_MODETYPE_PH) {
    // phone
    char buf[5];
    if (c=='?') c='/';
    if (c=='\"') c='/';
    sprintf(buf,"%c",tolower(c));
    play_string_cmd(buf);
  }
}

void on_key_down(MODIFIERKEYS modkey, uint8_t key, uint8_t c) {
  //  verbose=1;
  if (verbose & 1) {
    plogw->ostream->print("Key=<");
    PrintHex<uint8_t>(key, 0x80);
    plogw->ostream->print(">c=");
    plogw->ostream->print((char)c);
    plogw->ostream->print("<");
    PrintHex<uint8_t>(c, 0x80);
    plogw->ostream->println(">");
    plogw->ostream->print("f_caps:");
    plogw->ostream->println(f_capslock);
  }
  if (f_printkey) {
    sprintf(dp->lcdbuf,"on key down\nKey %d\nc %c <%02x>\ncaps %d\n",
	    key,c,c,f_capslock);
    upd_display_info_flash(dp->lcdbuf);
  }
  if (key == 0x39) {
    // check capslock for another modifier
    f_capslock = 1;
  }
  //  console->print ("caps=");
  //  console->println (f_capslock);      

  uint8_t tmpbuf[2];

  struct radio *radio;
  radio = so2r.radio_selected();
  // the following is mixed with function in logw_handler() ... unify sometime 21/10/24 ea
  // Shift + alphanumeric key will be sent to cw keying regardless of keyer mode .. 21/11/22 ea
  if (modkey_shift(modkey)) {
    ///// shift+
    if (!modkey_alt(modkey) && !modkey_ctrl(modkey)) {
      // only shift + --> basically cw keying
    
      /*      case 0x1f:
	      console->print ("shift+2 pressed caps=");
	      console->print (f_capslock);      
	      if ((modkey.bmLeftCtrl == 1) || (modkey.bmRightCtrl == 1) || (f_capslock==1) ) {
	      console->println("shift+2+ctrl pressed ");	
	      // shift-ctrl-2  // switch contest	
	      plogw->contest_id--;
	      if (plogw->contest_id <0 ) plogw->contest_id = 0;
	      set_contest_id();
	      }
	      break;
      */


      switch(key) {
	
      case 0x28:   // Enter + SHIFT
	process_enter(1);
	break;
      case 0x2b:  // TAB + SHIFT
      case 0x2c:  // SPACE + SHIFT
	switch (radio->keyer_mode) {
	case 0:
	  switch_logw_entry(1);
	  break;
	case 1: // keyer
	  //	  plogw->f_repeat_func_stat = 0;
	  so2r.suspend();
	  send_single_char_radio(radio,c);
	  break;
	}
	break;
      
      case 0x51:  // DOWN + SHIFT
	if (plogw->sat) {      
	  adjust_sat_offset(-100);
	} else { // adjust frequency of a station for the mode
	  adjust_frequency(-freq_width_mode(radio->opmode));	  
	}
	upd_display();      
	break;
      case 0x52:  // UP + SHIFT
	if (plogw->sat) {    
	  adjust_sat_offset(100);
	} else {
	  adjust_frequency(freq_width_mode(radio->opmode));	  
	}
	upd_display();            
	break;

	// shift + left / right  cursor in bandmap change
      case 0x50:  // left
	select_bandmap_entry(-1,radio->bandid_bandmap);
	break;
      case 0x4f:  //right
	select_bandmap_entry(+1,radio->bandid_bandmap);
	break;

      case 0x2a:  // BS + SHift
	delete_cwbuf();
	break;

      case 0x31:  // \ backslash to switch focused rig and RX to hear(on SO2R)
	switch (so2r.focused_radio()) {
	case 0:
	  //	  select_radio(1);
	  so2r.change_focused_radio(1);
	  break;
	case 1:
	  //	  select_radio(2);
	  so2r.change_focused_radio(2);	  
	  break;
	case 2:
	  //	  select_radio(0);
	  so2r.change_focused_radio(0);	  
	  break;
	}

	upd_display();
	bandmap_disp.f_update = 1;
	break;
	//

      default:
	// shift+
	// function key  --> start rx switching in SO2R 
	if ((key >= 0x3a) && (key <= 0x45)) {
	  //	  plogw->f_repeat_func_stat = 0;
	  so2r.suspend();
	  so2r.radio_mode = SO2R::RADIO_MODE_SO2R; // force go to SO2R 
	  if (modkey_ctrl(modkey)) {
	    so2r.sequence_mode(SO2R::SO2R_Repeat_Func);
	  } else {
	    if (key==0x3a) { // CQ
	      so2r.sequence_mode(SO2R::SO2R_CQSandP);
	    }
	  }
	  so2r.cancel_msg_tx();	
	  so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
	  so2r.set_rx_in_sending_msg();
	  function_keys(key, c); 
	  break;
	}
	if (verbose &4) {
	console->print("key=");
	console->println ((int)key);
	}
	// shift only (somehow come here) leads to mulfunction of SO2R messaging, so check and pass through
	if (key==229 || key==225) {
	  if (verbose&4) console->println("shift only pass thru");
	  break;
	}
	
	// list ptr_curr Shift+ keyer mode interfere setting the window characters
	// sent_exch, Remarks, rig_name, cluster_name, email-addr, cluster_cmd, wifi_ssid, wifi_passwd, rig_spec, zserver_name, my_name
	if (!((radio->ptr_curr == 5) || (radio->ptr_curr == 6) || (radio->ptr_curr == 20) || (radio->ptr_curr == 21) || (radio->ptr_curr == 22) || (radio->ptr_curr == 23) || (radio->ptr_curr == 25) || (radio->ptr_curr == 26) || (radio->ptr_curr == 27) || (radio->ptr_curr == 28) || (radio->ptr_curr == 29) || (radio->ptr_curr == 40) || ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) || ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)))) {
	  if (key == 0x37) {
	    // Shift-.  Shift-, manual band change up/down
	    radio->bandid_bandmap++;
	    if (radio->bandid_bandmap > N_BAND)
	      radio->bandid_bandmap = 1;  // allow bandid_bandmap == N_BAND
	    upd_display_bandmap();
	    break;
	  }
	  
	  if (key == 0x36) {
	    radio->bandid_bandmap--;
	    if (radio->bandid_bandmap < 1)
	      radio->bandid_bandmap = N_BAND ; // allow bandid_bandmap == N_BAND
	    upd_display_bandmap();
	    break;
	  }
	  // cluster edit mode send all characters

	} else {
	  logw_handler(key, c);  // in cw msg edit mode , send 'SPC' to editor (use TAB to switch entry)
	  break;
	}
	if (key >= 0x1e && key <= 0x26) {
	  c = ((int)key - 0x1e) + '1';
	}
	if (key == 0x27) c = '0';
	if (key == 0x87) c = '/';  // ad-hoc remapping next to ?/ key in Japanese keyboard but not so in US.
	//	plogw->f_repeat_func_stat = 0;
	if (so2r.sequence_stat()==SO2R::Sending_Msg) {
	  so2r.cancel_current_tx_message();
	  so2r.suspend();	  
	}
	so2r.set_msg_tx_to_focused();
	send_single_char_radio(radio,c);
	break;
      }
    } // !alt ! ctrl
    else {
      if (modkey_alt(modkey) && !modkey_ctrl(modkey)) {
	// shift + alt
	if (key == 0x04 ) {  // Shift-Alt-A show next aos
	  if (plogw->sat) {
	    plogw->nextaos_showidx += 5;
	    if (plogw->nextaos_showidx >= N_SATELLITES) {
	      plogw->nextaos_showidx = 0;
	    }
	    print_sat_info_aos();
	  }
	}
	else if (key==0x07) {
	  // Shift+ Alt-D   delete entry in the bandmap on_frequenry
	  delete_on_freq_entry_bandmap();
	  upd_display_bandmap();
	}
	else if (key==0x4a) {    // Shift-Alt-HOME   switch rig (sub)
	  if (!plogw->f_console_emu) plogw->ostream->println("select subrig");
	  switch_radios(1, -1);
	}
	else if (key== 0x4d) {  // Shift-Alt-END   enable/disable rig
	  if (!plogw->f_console_emu) plogw->ostream->println("enable/disable  subrig");
	  enable_radios(1, -1);
	}
	
      } // shift alt !ctrl 

    }
  } else if (modkey_alt(modkey)) {
    // ALT+
    // ALT
    // 0           1               2
    // 456778abcdef0123456789abcdef01234567
    // ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890
    if (modkey_ctrl(modkey)) {
      // Alt-Ctrl-
      switch (key) {
      case 0x4a:  // HOME   switch rig (sub)

	if (!plogw->f_console_emu) plogw->ostream->println("select 3rd rig ");
	// Alt-Shift-HOME sub rig change
	switch_radios(2, -1);
	break;
      case 0x4d:  // END   enable/disable rig
	if (!plogw->f_console_emu) plogw->ostream->println("enable/disable  3rd rig");
	enable_radios(2, -1);
	break;
      }
      return;
    }
	
    if (key == 0x38) { // alt-'/' antenna rotator sweep suspension 
      if (plogw->f_rotator_sweep_suspend>0) {
	plogw->f_rotator_sweep_suspend=0;
      } else {
	plogw->f_rotator_sweep_suspend=1;
      }
      //plogw->f_rotator_sweep_suspend = 1-plogw->f_rotator_sweep_suspend;
      plogw->ostream->print("rotator sweep suspend=");
      plogw->ostream->println((int)plogw->f_rotator_sweep_suspend);
    }
    if (key == 0x2e) { // Alt-'=' antenna relay1 toggling
      alternate_antenna_relay();
      return;
    }
    if (key == 0x0c) {  // Alt-i temporally disbling rig if enabled, enable if disabled
      radio=so2r.radio_selected();
      if (radio->enabled) {
	enable_radios(so2r.focused_radio(),0);
	timeout_rig_disable_temporally=millis()+10000;
      } else {
	enable_radios(so2r.focused_radio(),1);	
      }
      return;
    }
    if (key == 0x2d) {  // Alt-'-'  scope setting
      set_scope();
      return;
    }
    /*  if (key == 0x19) { // Alt-v 0x19 // this is removed to avoid unaware invoking verbose condition
	if (verbose) {
	verbose = 0;
	} else {
	verbose = 1;
	}
	return;
	}
    */
    if (key == 0x17) {  // Alt-T tx/rx toggle ptt
      radio->ptt = 1 - radio->ptt;
      set_ptt_rig(radio, radio->ptt);
      return;
    }
    if (key == 0x1b) {  // Alt-X transverter toggle
      switch_transverter();
      return;
    }

    if (key == 0x1e) {  // Alt-1  // set priority to this rig and band
      set_current_rig_priority();
      return;
    }

    if (key == 0x1d) {  // alt-z  // switch CW <=> PH
      save_freq_mode_filt(radio);
      switch (radio->modetype) {
      case LOG_MODETYPE_CW:
	radio->modetype = LOG_MODETYPE_PH;
	break;
      case LOG_MODETYPE_PH:
	radio->modetype = LOG_MODETYPE_CW;
	break;
      default:
	return;
      }
      // save new modetype to the bank so cq and freq is recalled in the new modetype
      int tmp;
      radio->modetype_bank[radio->bandid]=radio->modetype;
      //      radio->cq_modetype_bank[radio->bandid]=radio->cq[radio->modetype]*4+(radio->modetype&0x3);
      
      // no transision DG and other modes
      recall_freq_mode_filt(radio);

      upd_display_bandmap();      
      // issue: cq <-> s&p transision does not occur in this key
      return;
    }

    if (key == 0x14) {  // alt-q switch CQ / S&P
      // memorize current frequency to freqbank cq freqnency
      save_freq_mode_filt(radio);
      if (radio->cq[radio->modetype] == LOG_CQ) {
	// from cq to s&p
	radio->cq[radio->modetype] = LOG_SandP;
      } else {  // S&P
	// from s&p to cq
	radio->cq[radio->modetype] = LOG_CQ;
      }
      // update cq_bank so that new frequency in cq state is recovered 
      radio->cq_bank[radio->bandid][radio->modetype]=radio->cq[radio->modetype];
      // recall previous memory of S&P
      recall_freq_mode_filt(radio);
      
      if (verbose & 1) {
	plogw->ostream->print("cq:");
	plogw->ostream->println(radio->cq[radio->modetype]);
      }
      upd_display_stat();
      //      u8g2_r.sendBuffer();  // transfer internal memory to the display
      right_display_sendBuffer();
      bandmap_disp.f_update = 0;
      upd_display_bandmap();
      return;
    }
    if (key == 0x04) {  // Alt-A show next aos
      if (plogw->sat) {
	strcpy(dp->lcdbuf, "Calc next AOS-LOS\n");
	upd_display_info_flash(dp->lcdbuf);
	start_calc_nextaos();
      }
    }
    if (key == 0x13) {  // alt-p pick satellite of closest next aos
      if (plogw->sat) {

	if (*sat_info[satidx_sort[0]].name != '\0') {
	  strcpy(plogw->sat_name + 2, sat_info[satidx_sort[0]].name);
	  if (!plogw->f_console_emu) {
	    plogw->ostream->print("Picked satellite:");
	    plogw->ostream->println(plogw->sat_name + 2);
	  }
	  sat_name_entered();
	}

      } else {
	//if (key == 0x13) {
	// alt-p change verbose level of printing cluster input to serial
	f_show_cluster++;
	if (f_show_cluster >= 3) f_show_cluster = 0;
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("alt-p cluster verbose leve = ");
	  plogw->ostream->println(f_show_cluster);
	}
      }
      // }
    }
    if (key == 0x05) {
      // alt-b set sat beacon frequency
      // alt-b bandmap sorting change

      if (plogw->sat) {
	set_sat_beacon_frequency();
      } else {
	bandmap_disp.sort_type++;
	if (bandmap_disp.sort_type >= 2) bandmap_disp.sort_type = 0;
	// sort and update display
	//plogw->ostream->print("sort and update bandmap "); plogw->ostream->println(bandmap_disp.sort_type);
	//sort_bandmap(plogw->bandid_bandmap, bandmap_disp.sort_type);
	upd_display_bandmap();
      }
    }

    if (key == 0x06) {  // Alt-c to edit cw mode
      if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
	// current CW
	radio->ptr_curr = 30;  // RTTY message
      } else {
	if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
	  // rtty
	  radio->ptr_curr = 0;
	} else {
	  radio->ptr_curr = 10;
	  if (verbose & 1) plogw->ostream->println("CW Function key message edit mode");
	}
      }
      upd_display();
      return;
    }

    if (key == 0x07) {
      // Alt-D   delete entry in the bandmap on the cursor
      delete_entry_bandmap();
      upd_display_bandmap();
    }

    if (key == 0x11) {
      // Alt-n   add current frequency and callsign in the bandmap
      int len;
      len = strlen(radio->callsign + 2);
      if (len > 3) {
	register_current_callsign_bandmap();
      }
      upd_display_bandmap();
    }
    
    if (key == 0x08) {  //Alt-E enable/disable usb keying  process (to concentrate in USB DTR keying in the main loop)
      enable_usb_keying = 1 - enable_usb_keying;
      if (!plogw->f_console_emu) {
	plogw->ostream->print("usb keying=");
	plogw->ostream->println(enable_usb_keying);
      }
      sprintf(dp->lcdbuf, "USB keying=%d\n", enable_usb_keying);
      upd_display_info_flash(dp->lcdbuf);
    }

    if (key == 0x0a) {  // Alt-g enable disable tone_keying (radio->f_tone_keying)
      //      if (radio->modetype== LOG_MODETYPE_PH) {
      radio->f_tone_keying = 1 - radio->f_tone_keying;
      set_tone_keying(radio);  // initialize for tone_keying (temporally LED pin later use DAC to generate clean sinusoidal tone)
      set_tone(0,0);  // tone keying status is changed, stop keying anyway
      keying(0); // 
      clear_cwbuf(); // and also clear unsent cw message	
      if (!plogw->f_console_emu) {
	plogw->ostream->print("tone keying=");
	plogw->ostream->println(radio->f_tone_keying);
      }
      if (radio->f_tone_keying==1) {
	radio->modetype=LOG_MODETYPE_CW; // force set modetype to be CW
	
      } else {
	radio->modetype = modetype_string(radio->opmode);

      }
      set_log_rst(radio);      
      sprintf(dp->lcdbuf, "Tone keying=%d\n", radio->f_tone_keying);
      upd_display_info_flash(dp->lcdbuf);

    }

    if (key == 0x2c) {
      // Alt-Space pick spot on the bandmap cursor
      pick_entry_bandmap();
      //      // dupe check here
      //      plogw->ostream->print("dupechk here");      
      //      if (dupe_check(radio->callsign + 2, bandmode(), plogw->mask, 1)) {  // cw/ssb both ok ... 0xff cw/ssb not ok 0xff-3
      //	radio->dupe = 1;
      //      } else {
      //	radio->dupe = 0;
      //      }
      upd_display();
      upd_display_bandmap();
    }

    if (key == 0x4a) {  // alt-HOME will switch selected radios
      switch_radios(0, -1);
    }
    if (key == 0x4c) {  // Alt-Del show rig info
      upd_disp_rig_info();
    }
    if (key == 0x4d) {  // ALT-END enable /disable main rig
      enable_radios(0, -1);
    }

    if (key == 0x09) {  // alt-f  set up/down sat frequency to the center of op band
      if (plogw->sat) {
	set_sat_center_frequency();
      }
    }

    if (key == 0x15) {  // alt-r  switch sat_vfo_mode
      if (plogw->sat) {
	switch (plogw->sat_vfo_mode) {
	case SAT_VFO_SINGLE_A_TX:  //SAT_VFO_SINGLE_A_TX
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
	  break;
	case SAT_VFO_SINGLE_A_RX:
	  plogw->sat_vfo_mode =
	    SAT_VFO_MULTI_TX_0;
	  break;
	case SAT_VFO_MULTI_TX_0:
	  plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_1;
	  // may need to force transmit on radio #0 using so2r_tx function
	  break;
	case SAT_VFO_MULTI_TX_1:
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_TX;
	  break;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("Switched sat_vfo_mode ");
	  plogw->ostream->println(plogw->sat_vfo_mode);
	}
	upd_display_sat();
      }
    }
    if (key == 0x16) {  // alt-s satellite tracking mode switch
      if (plogw->sat) {
	switch (plogw->sat_freq_tracking_mode) {
	case SAT_RX_FIX:  // rx fix
	  plogw->sat_freq_tracking_mode = SAT_TX_FIX;
	  break;
	case SAT_TX_FIX:  // tx fix
	  plogw->sat_freq_tracking_mode = SAT_SAT_FIX;
	  break;
	case SAT_SAT_FIX:  // sat fix
	  plogw->sat_freq_tracking_mode = SAT_NO_TRACK;
	  break;
	case SAT_NO_TRACK:  // sat no_track
	  plogw->sat_freq_tracking_mode = SAT_RX_FIX;
	  break;
	}
      }
      upd_display_sat();
    }
    if (key == 0x1a) {
      // Alt-w to wipe the qso
      wipe_log_entry();
      upd_display();
    }
    if (key == 0x37) {
      // Alt->,  Alt-<. manual band change up/down
      switch_bands(radio);
    }
    if (key == 0x36) {
      // Alt->  Alt-< manual band change up/down
      int tmp, count;
      count = 0;
      tmp = radio->bandid;
      while (count < N_BAND) {
	tmp--;
	if (tmp == 0) tmp = N_BAND - 1;
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("bandid:");
	  plogw->ostream->print(tmp);
	  plogw->ostream->print(" count:");
	  plogw->ostream->println(count);
	}
	if (((1 << (tmp - 1)) & radio->band_mask) == 0) {
	  // ok to change
	  band_change(tmp, radio);
	  break;
	}
	count++;
      }
      //      band_change(radio->bandid - 1, radio);
      upd_display();
    }
    // shift + left / right  cursor in bandmap change
    if (key == 0x52) {  // alt-left  --> remap to alt-up 0x52
      select_bandmap_entry(-1,radio->bandid_bandmap);
    }
    if (key == 0x51) {  //alt-right  ---> remap to alt-down 0x51
      select_bandmap_entry(+1,radio->bandid_bandmap);
    }
    if (key == 0x50) {  // Alt-DOWN  --> remap to alt-left 0x50
      radio->bandid_bandmap--;
      if (radio->bandid_bandmap < 1)
	radio->bandid_bandmap = N_BAND - 1;
      upd_display_bandmap();
    }
    if (key == 0x4f) {  // Alt-UP  --> remap to alt-right 0x4f
      radio->bandid_bandmap++;
      if (radio->bandid_bandmap >= N_BAND)
	radio->bandid_bandmap = 1;
      upd_display_bandmap();
    }

    if (key == 0x10) {
      //      plogw->ostream->print("before radio->modetype=");
      //      plogw->ostream->println(radio->modetype);      
      
      // alt-m to manually switch modes
      const char *mode;
      
      mode = switch_rigmode();
      
      int filt;
      filt = radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];
      if (filt==0) {
	filt=default_filt(radio->opmode);
      }
      set_mode(mode, filt, radio);
      send_mode_set_civ(mode, filt);

      //      plogw->ostream->print("after radio->modetype=");
      //      plogw->ostream->println(radio->modetype);      
      
      upd_display();
    }

    // abcdefghijklmnopqrstuvwxyz
    // 456789abcdef0123456789abcd
    // others
    if ((key >= 0x04) && (key <= 0x1d)) {
      if (verbose & 1) {
	plogw->ostream->print("<Alt-");
	plogw->ostream->print((char)(key - 0x04 + 'A'));
	plogw->ostream->print(">");
      }
    } else {
      if (verbose & 1) {
	print_key(modkey, key);
      }
    }
	
  } // end of alt+
  else if (modkey_ctrl(modkey)) {
    // Ctrl+
    // 0           1               2
    // 456789abcdef0123456789abcdef01234567
    // ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890
    if ((key >= 0x3a) && (key <= 0x45)) {
      // Ctrl + function keys = repeat function
      plogw->ostream->println("repeat function");

      switch (so2r.radio_mode) {
      case SO2R::RADIO_MODE_SAT:
      case SO2R::RADIO_MODE_SO1R:
	// repeat 
	so2r.sequence_mode(SO2R::Repeat_Func);
	break;
      case SO2R::RADIO_MODE_SO2R:
	so2r.sequence_mode(SO2R::SO2R_Repeat_Func);
	break;
      }

      so2r.cancel_msg_tx();	      
      //      plogw->repeat_func_radio = plogw->focused_radio;
      //      so2r.msg_tx_radio(so2r.focused_radio());
      //      SO2R_settx(plogw->repeat_func_radio);
      //      so2r.set_tx(so2r.msg_tx_radio());

      

      //      plogw->repeat_func_key = key;
      //      so2r.msg_key(key);
      //      modkey.bmLeftCtrl = 1;                     // force set bmLeftCtrl in modkey
      //      plogw->repeat_func_key_modifier = modkey;  // repeat func設定した際のmodifier を憶えておく。

      //      function_keys(key, modkey, c);

      so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
      so2r.set_rx_in_sending_msg();
      so2r.repeat_cq_radio(so2r.msg_tx_radio()); // memorize      
      function_keys(key,c);
    }

    // key code check for special shortcut
    if ((key >= 0x04) && (key <= 0x34)) {

      if (verbose & 1) {
	plogw->ostream->print("<Ctrl-");
	plogw->ostream->print((char)(key - 0x04 + 'A'));
	plogw->ostream->print(">");
      }
      int pos;
      switch (key) {
      case 0x05: // ctrl-b exch partial check
	ui_perform_exch_partial_check(radio);
	break;
      case 0x08:  // ctrl-e direct move to recv_exch
	switch_logw_entry(5);
	break;
	
      case 0x06:  // ctrl-c direct move to callsign
	switch_logw_entry(3);
	break;
	// enable/disable voice cq on/off ? if set, TU message voice record will be sent on Enter 220116
      case 0x16:  // ctrl-s direct move to his RST
	switch_logw_entry(4);
	break;
      case 0x04: // ctrl-a direct move to sent RST
	switch_logw_entry(6);
	break;
      case 0x0a: // ctrl-g find entity information from callsign
	//	console->println("ctrl-g");
	if (strlen(radio->callsign+2)>2) {
	  show_entity_info(radio->callsign+2);
	}
	break;
      case 0x09:  // ctrl-f find multi from remarks or numbers 
	switch (radio->ptr_curr) {
	case 6: // in remarks, reverse search jcc/jcg from city name 
	  reverse_search_multi();
	  break; 
	case 1: // in exchange input, search multi from the number (multi_check)
	  radio->multi = multi_check_option(radio->recv_exch+2,radio->bandid,1);
	  //	  serial.print("multi checked ");
	  //	  serial.print(radio->multi);	  
	  upd_display_info_contest_settings(radio);
	  break;
	}
	break;
      case 0x14: // ctrl-q switch QSL specification
	radio->f_qsl++;
	if (radio->f_qsl>=3) {
	  radio->f_qsl=0;
	}
	switch(radio->f_qsl) {
	case 0: // NOQSL
	  sprintf(dp->lcdbuf, "Card=NoQSL\n");
	  break;
	case 1: // JARL
	  sprintf(dp->lcdbuf, "Card=JARL\n");
	  break;
	case 2: // hQSL
	  sprintf(dp->lcdbuf, "Card=hQSL\n");
	  break;
	}
	upd_display_info_flash(dp->lcdbuf);
	break;
      case 0x0e:  // ctrl-k keyer mode switch

	if (radio->keyer_mode == 1) {
	  radio->keyer_mode = 0;
	} else {
	  radio->keyer_mode = 1;
	  clear_cwbuf();
	}
	// need to update info line
	upd_display_stat();
	//          u8g2_r.sendBuffer();  // transfer internal memory to the display
	right_display_sendBuffer();

	if (verbose & 1) {
	  plogw->ostream->print("keyer_mode:");
	  plogw->ostream->println(radio->keyer_mode);
	}
	break;
      case 0x15:  // ctrl-r direct move to remarks
	switch_logw_entry(2);
	break;
      case 0x1a:  // Crrl-w to wipe the QSO
	wipe_log_entry();
	upd_display();
	break;
      case 0x1d: // ctrl-z send Remarks to Z-server
	if (radio->ptr_curr == 6) {
	  zserver_send(radio->remarks+2);
	}
	break;
      case 0x19:  // Ctrl-v to toggle voice memory enable
	plogw->voice_memory_enable++;
	if (plogw->voice_memory_enable>=4) {
	  plogw->voice_memory_enable=0;
	}
	//	if (plogw->voice_memory_enable) {
	//	  plogw->voice_memory_enable = 0;
	//	} else {
	//	  plogw->voice_memory_enable = 1;
	//	}
	sprintf(dp->lcdbuf, "VoiceMemory=%d\n%s\n", plogw->voice_memory_enable,
		(plogw->voice_memory_enable==0 ? "Disabled" :
		 (plogw->voice_memory_enable==1 ? "Enabled(NoTU)":
		  (plogw->voice_memory_enable==2 ? "Enabled(TU)":"Enabled(voice gen)")
		  )));

	upd_display_info_flash(dp->lcdbuf);
	plogw->ostream->print(dp->lcdbuf);	
	break;
	/// qso entry display related
      case 0x0d:  //ctrl-j to show current qso entry
	upd_display_info_qso(0);
	break;
      case 0x13:;  // ctrl-p to show previous qso entry
	info_disp.pos -= sizeof(qso.all);
	if (info_disp.pos < 0) info_disp.pos = 0;
	upd_display_info_qso(0);
	break;
      case 0x11:;  // ctrl-n to show next qso entry
	info_disp.pos += sizeof(qso.all);
	pos = qsologf.position();

	if (info_disp.pos > pos - sizeof(qso.all)) info_disp.pos = pos - sizeof(qso.all);
	upd_display_info_qso(0);
	break;
      case 0x12:;  // ctrl -o to the last recorded qso entry and show
	pos = qsologf.position();
	info_disp.pos = pos - sizeof(qso.all);
	if (info_disp.pos < 0) {
	  info_disp.pos = 0;
	} else {
	  upd_display_info_qso(0);
	}
	break;
      case 0x2d:  // 		Ctrl-'-'  to load current (shown by Ctrl-j) into editing buffer
	if (!plogw->f_console_emu) {
	  plogw->ostream->println("qso SD-> edit");
	}
	upd_display_info_qso(1);
	break;

      case 0x2f: // Ctrl-'[' toggle partial check flag
	ui_toggle_partial_check_flag();
	break;

      case 0x34: // ctrl-'\'' (apostroph) pick partial check
	ui_pick_partial_check(radio);
	break;
	      

	// contest related settings
	
      case 0x1e:  // ctrl-1  // switch dupe cw/phone
	if (plogw->mask == 0xff) {
	  plogw->mask = 0xff - 3;
	} else {
	  plogw->mask = 0xff;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("mask:");
	  plogw->ostream->println(plogw->mask);
	}
	upd_display_info_contest_settings(radio);
	break;
      case 0x1f:  // ctrl-2  // switch contest
	plogw->contest_id++;
	if (plogw->contest_id >= N_CONTEST) plogw->contest_id = 0;
	set_contest_id();
	break;
      case 0x20:  // ctrl-3  bandmap mask toggle for current operating band
	if (bandmap_mask & (1 << (radio->bandid - 1))) {
	  bandmap_mask &= ~(1 << (radio->bandid - 1));
	} else {
	  bandmap_mask |= (1 << (radio->bandid - 1));
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("bandmap_mask=");
	  plogw->ostream->println(bandmap_mask, BIN);
	}
	print_bandmap_mask();
	break;


      case 0x21:  // ctrl-4 callhistory search toggle
	if (plogw->enable_callhist) {
	  plogw->enable_callhist = 0;
	} else {
	  plogw->enable_callhist = 1;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("callhist en =");
	  plogw->ostream->println(plogw->enable_callhist);
	}
	sprintf(dp->lcdbuf, "callhist en:%d\nStart.\n", plogw->enable_callhist);
	upd_display_info_flash(dp->lcdbuf);
	int n;
	if (plogw->enable_callhist) {
	  if (!plogw->f_console_emu) plogw->ostream->println("open callhist");
	  //	  n=read_callhist_list("/23FD.PCK");
	  n=read_callhist_list(callhistfn);	  
	  print_callhist_list((const char **)callhist_list,n);
	  
	  //	  open_callhist();
	} else {
	  if (!plogw->f_console_emu) plogw->ostream->println("close callhist");
	  //	  close_callhist();
	}
	sprintf(dp->lcdbuf, "callhist en:%d\nDone.\n", plogw->enable_callhist);
	upd_display_info_flash(dp->lcdbuf);
	break;

      case 0x22:  // ctrl-5 radio_mode switch
	switch (so2r.radio_mode) {
	case 0:  // current so1r
	  so2r.radio_mode = SO2R::RADIO_MODE_SAT;
	  break;

	case 1:  // sat wro radio
	  so2r.radio_mode = SO2R::RADIO_MODE_SO2R;
	  break;
	case 2:  // so2r
	  so2r.radio_mode = SO2R::RADIO_MODE_SO1R;
	  break;
	}
	sprintf(dp->lcdbuf, "radio_mode =%d\n%s\n", so2r.radio_mode, (so2r.radio_mode == SO2R::RADIO_MODE_SO1R) ? "SO1R" : (so2r.radio_mode == SO2R::RADIO_MODE_SAT) ? "SAT 0 TX 1 RX "
		: "SO2R");
	upd_display_info_flash(dp->lcdbuf);
	break;

	// multi display
      case 0x17:  // ctrl-t , (ctrl-y ) multi list page up and down and display
	info_disp.multi_ofs -= 4;
	if (info_disp.multi_ofs < 0) info_disp.multi_ofs = 0;
	upd_display_info_contest_settings(radio);
	break;

      case 0x1c:  // ctrl-y
	info_disp.multi_ofs += 4;
	if (*multi_list.multi != NULL) {
	  if (info_disp.multi_ofs >= multi_list.n_multi[radio->bandid-1]) info_disp.multi_ofs = multi_list.n_multi[radio->bandid-1] - 1;
	}
	upd_display_info_contest_settings(radio);
	break;
	// score summary
      case 0x18:  // ctrl-u    to work number of stations and worked stations/multis page show
	if (info_disp.show_info == INFO_DISP_SUMMARY &&  info_disp.timer > 2000) { 
	  plogw->bandmap_summary_idx += 6;
	  if (plogw->bandmap_summary_idx >= N_BAND) plogw->bandmap_summary_idx = 0;
	}
	upd_display_info_to_work_bandmap();	    
	break;
      }
    } // 0x04-0x34
    else {
      print_key(modkey, key);
    }
  } // ctrl *

  else {
    /////////////////////////////////////////////////////////////////////
    // no modifier keys
    // check with special keys
    switch (key) {
    case 0x28:  // Enter
      process_enter(0);
      break;
      //break;
    case 0x2c:  // SPACE
      switch (radio->keyer_mode) {
      case 0:
	if (check_edit_mode() == 1) {
	  if (verbose & 1) plogw->ostream->println("check_edit_mode==1");
	  logw_handler(key, c);  // in cw msg edit mode , send 'SPC' to editor (use TAB to switch entry)
	} else {
	  if (radio->ptr_curr == 1) { // exchange input space will make callsign window move
	    radio->ptr_curr=0;
	    upd_display();
	  } else {
	    if (verbose & 1) plogw->ostream->println("switch_logw_entry(0)");
	    switch_logw_entry(0);
	  }
	}
	break;
      case 1: // in keyer mode
	so2r.cancel_msg_tx();
	so2r.set_msg_tx_to_focused();
	send_single_char_radio(radio,c);
	break;
      }
      break;

    case 0x29:  // ESC cancel messaging
      if (verbose & 1) plogw->ostream->print("(ESC)");
      so2r.cancel_msg_tx();
      switch(so2r.radio_mode) {
      case SO2R::RADIO_MODE_SO2R:
	so2r.sequence_mode(SO2R::SO2R_CQSandP);
	break;
      case SO2R::RADIO_MODE_SO1R:
      case SO2R::RADIO_MODE_SAT:	
	so2r.sequence_mode(SO2R::Manual);
	break;
      }
      so2r.sequence_stat(SO2R::Default);

      /* looks like the following is done in cancel_msg_tx() -> cancel_current_tx_message() -> cancel_keying()
      if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying)) {
	// clear cw buffer (stop CW keying)
	clear_cwbuf();
      } else if (radio->modetype== LOG_MODETYPE_PH) {
	if (plogw->voice_memory_enable) {
	  struct radio *radio1;
	  radio1=so2r.radio_msg_tx();
	  send_voice_memory(radio1, 0); // need stop msg tx radio
	}
      } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
	//            send_rtty_memory(radio, 0); // this will not stop sending ! (22/7/23)
	if (radio->f_tone_keying) {
	  //              ledcWrite(LEDC_CHANNEL_0, 0);  // stop sending tone
	} else {
	  //digitalWrite(LED, 0);
	  keying(0);
	}
	set_ptt_rig(radio, 0);
      }
      */
      break;

    case 0x2a:  // BS
      if (verbose & 1) plogw->ostream->print("\H(BS)");
      if (radio->keyer_mode) {
	delete_cwbuf();
      } else {
	logw_handler(key, c);
      }
      break;
    case 0x2b:  // TAB
      // exchange current entry
      switch_logw_entry(0);
      break;
    case 0x35:  // ` to switch stereo/mono on SO2R
      switch (so2r.radio_mode) {
      case 0:
      case 1:
      case 2:
	so2r.setstereo(1 - so2r.stereo());
      }
      break;

    case 0x31:  // \ backslash to switch focused rig and RX to hear(on SO2R)

      switch (so2r.focused_radio()) {
      case 0:
	so2r.change_focused_radio(1);
	break;
      case 1:
	so2r.change_focused_radio(0);
	break;
      case 2:
	so2r.change_focused_radio(0);
	break;
      }
      //

      break;


    case 0x4b:  // PGUP
      // cw keying speed change
      cw_spd++;
      if (cw_spd >= 35) cw_spd = 35;
      show_cw_spd();
      break;

    case 0x4e:  // PGDN
      cw_spd--;
      if (cw_spd <= 10) cw_spd = 10;
      show_cw_spd();
      break;
      // editing related keys

    case 0x4c:  // DEL
    case 0x4f:  // RIGHT
    case 0x50:  // LEFT
      logw_handler(key, c);
      break;

      // frequency control keys
    case 0x51:  // DOWN
      adjust_frequency(-100/FREQ_UNIT);
      upd_display();            
      break;
    case 0x52:  // UP
      adjust_frequency(+100/FREQ_UNIT);
      upd_display();            
      break;

    case 0x46:  // PRTSC
    case 0x49:  // INS
    case 0x4a:  // HOME
    case 0x4d:  // END

      if (verbose & 1) {
	plogw->ostream->print("Key=<");
	PrintHex<uint8_t>(key, 0x80);
	plogw->ostream->print(">");
      }
      // frequency adjustment


      // Ctrl +Down , Up  get next spot higher in frequency
      // Ctrl Alt Down , Up get next spot higher in multiplyer (not implemented)
      // Alt+Q CQ/S&P switch
      // Ctrl-W wipe
      // Space/Tab jump
    default:
      // no mod 
      // function keys
      if ((key >= 0x3a) && (key <= 0x45)) {
	so2r.cancel_msg_tx();	
	so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
	so2r.set_rx_in_sending_msg();	
	function_keys(key, c);
      } else {
	// key input in the repeat function radio window will stop repeating until concluding interrupted qso 
	if (so2r.sequence_mode() == SO2R::Repeat_Func || so2r.sequence_mode() == SO2R::SO2R_Repeat_Func) {
	  if (so2r.msg_tx_radio() == so2r.focused_radio()) {
	    if (radio->ptr_curr == 0 || radio->ptr_curr == 1) {
	      so2r.suspend();
	    }
	  }
	} else if (so2r.sequence_mode() == SO2R::SO2R_ALTCQ || so2r.sequence_mode() == SO2R::SO2R_2BSIQ) {
	  so2r.suspend();
	  
	}
	// other alphabetical / numerical keys
	//  OnKeyPressed(c);
	switch (radio->keyer_mode) {
	case 0:
	  if ((radio->ptr_curr == 0)||(radio->ptr_curr == 1) ) { // 25/3/23 same response in call/exchange ; 
	    // additional check for ; (send F5 and move to my exch window)
	    if (c == ';') {
	      ui_response_call_and_move_to_exch(radio);
	      break;
	    } else if (c== '\'') {  // apostroph
	      ui_cursor_next_entry_partial_check (radio);
	      break;
	    }
	  }
	  logw_handler(key, c);
	  break;
	case 1:
	  so2r.cancel_msg_tx();
	  if (c == ';') {
	    ui_response_call_and_move_to_exch(radio);
	    break;
	  }
	  send_single_char_radio(radio,c);
	  break;
	} // keyer mode
      }	    // other alphabetical / numerical keys
    } // switch key
  } // no modifier
}

//void function_keys(uint8_t key, MODIFIERKEYS modkey, uint8_t c) {
void function_keys(uint8_t key, uint8_t c) {
  uint8_t tmpbuf[2];

  struct radio *radio;
  so2r.cancel_msg_tx();
  radio=so2r.radio_msg_tx(); // transmit always in the radio of msg_tx
  so2r.set_tx_to_msg_tx();

  // function key
  if (verbose & 1) {
    plogw->ostream->print("<F");
    plogw->ostream->print(key - 0x3a + 1);
    plogw->ostream->print(">");
  }
  if ((key >= 0x3a) && (key <= 0x3a + N_CWMSG - 1)) {
    if (key == 0x3a) {
      so2r.msg_key(key); // memorize only in cq (for repeating functions)
      // F1 CQ  go to CQ mode in the current frequency
      if (radio->cq[radio->modetype] == LOG_SandP) {
        radio->cq[radio->modetype] = LOG_CQ;
      }
      // memorize current frequency to freqbank cq freqnency
      save_freq_mode_filt(radio);
      // abcdef012345
      //F123456
    }
    if (key == 0x3d) {  // F4 callsign
      if (!radio->cq[radio->modetype]) {
        // S & P  start getting his S meter reading
        radio->smeter_stat = 1;
        radio->smeter_peak = SMETER_MINIMUM_DBM;

        //
        int len;
        len = strlen(radio->callsign + 2);
        if (len == 0) {  // no existing callsign entry
          pick_onfreq_station();
        } else {
          if (len > 3) {
            register_current_callsign_bandmap();
          }
        }
      }
    }
    if (key == 0x3b) {  // F2 send number
      if (!radio->cq[radio->modetype]) {
        if (radio->smeter_stat == 1)
          radio->smeter_stat = 2;  // finished getting s-meter statistics
      }
    }

    // check ctrl-key repeat function するか？
    //    plogw->repeat_func_timer = 0;
    so2r.cancel_repeat_timer();

    so2r.sequence_stat(SO2R::Sending_Msg);
    // send corresponding memory
    if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying)) {
      append_cwbuf_string(plogw->cw_msg[key - 0x3a] + 2);  // send CW msg
      append_cwbuf('$');  // repeat command
    } else if (radio->modetype== LOG_MODETYPE_PH) {
      // こちらもsequence_modeにしたがって制御する必要あり。
	// voice memory playback
	if (plogw->voice_memory_enable) {
	  send_voice_memory(radio, key - 0x3a + 1);  // F3 voice memory send
	}
    } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
      set_rttymemory_string(radio, key - 0x3a + 1, plogw->rtty_msg[key - 0x3a] + 2);
      // こちらもsequence_modeにしたがって制御する必要あり。      
      // rtty message sent to cw buffer (or rig's memory )
      // and send it on the air
      if (!enable_usb_keying) {
	delay(200);
	send_rtty_memory(radio, key - 0x3a + 1);  // command sending the stored memory
      }
      //       send cat command to transmit memory
    }
  }
}






// 入力欄でEnter を押したときの処理
void process_enter(int option) {
  // option 0: normal
  //        1: with SHIFT 
  struct radio *radio;
  radio = so2r.radio_selected();
  so2r.set_default_sequence_mode(); // set appropriate operating mode
  so2r.qso_process_radio(so2r.focused_radio());// set qso_process_radio to the focused

  if (verbose &4) {
    console->print("process_enter() process ");
    console->print(so2r.qso_process_radio());
    console->print("focus  ");
    console->print(so2r.focused_radio());
    console->print("tx  ");  
    console->print(so2r.tx());    
    console->print("rx  ");  
    console->println(so2r.rx());
  }
  
  char buf[60];
  char *p;
  if (verbose & 1) plogw->ostream->println("Enter pressed");
  //        switch (plogw->keyer_mode) {
  //          case 0: // non keyer mode
  // 21/11/7 Enter will also be processed similarly in keyer mode
  switch (radio->ptr_curr) {
  case 1:  // number entry
    // check multipliers
    int ret;
    radio->multi = multi_check(radio->recv_exch+2,radio->bandid);
    if (strncasecmp(plogw->contest_name+2,"NOMULTI",7)!=0) {
      if (radio->multi == -1) {
	// not valid multipliers
	if (!plogw->f_console_emu) plogw->ostream->println("not valid multiplier");
	upd_display();
	upd_display_info_contest_settings(radio);
	break;
      }
    }

    // ESM
    if (plogw->f_esm==1) {
      if (radio->cq[radio->modetype] == LOG_SandP && option!=1) {
	// ESM && S&P -- > send exchange
	if (!so2r.send_exch(radio)) return; // this need to be done for the previously focused radio
	// 2bsiq delay so set flag and return
      }
    }
    
    // determine qsoid
    plogw->qsoid=  plogw->txnum* 100000000 + plogw->seqnr *10000 + random(100)*100;
    
    // log current QSO
    print_qso_log();
    if (verbose&4) 	{
      if (!plogw->f_console_emu) plogw->ostream->println("print_qso_log() finished");
    }

    // reset qso_interval_timer
    plogw->qso_interval_timer =0;

    if (!radio->dupe) {
      // update statistics in score
      score.worked[radio->modetype == LOG_MODETYPE_CW ? 0 : 1][radio->bandid - 1]++;
      entry_dupechk(so2r.radio_qso_process());
    }
    plogw->seqnr_band[radio->bandid-1]++;
    if (verbose&4) 	{
      if (!plogw->f_console_emu) plogw->ostream->println("dupechk entered");
    }

    entry_multiplier(so2r.radio_qso_process());
    if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("multi entered ");
    }

    // check bandmap and update flags
    int idx;
    idx = search_bandmap(radio->bandid, radio->callsign + 2, modeid_string(radio->opmode));
    if (idx != -1) {
      //
      struct bandmap_entry *p;
      p = bandmap[radio->bandid - 1].entry + idx;
      p->flag |= BANDMAP_ENTRY_FLAG_WORKED;
      console->print("search_bandmap found the station in");console->println(idx);
    } else {
      console->println("search_bandmap didn't find the station ");
    }

    // bandmap update
    if (radio->cq[radio->modetype] == LOG_SandP) {
      if (strlen(radio->callsign + 2) > 3) {
	register_current_callsign_bandmap();
      }
    }

    // send TU
    if (option == 0 && ((plogw->f_esm==0) || (plogw->f_esm==1 && (radio->cq[radio->modetype] == LOG_CQ)))) {    
      so2r.send_tu();
      if (verbose&4) console->println("end of tu");      
    }
    so2r.print_sequence_mode();
    
    if (verbose&4) 	{
      if (!plogw->f_console_emu) plogw->ostream->println("bandmap updated  ");
    }

    upd_display_info_contest_settings(so2r.radio_qso_process());

    // prepare new log entry
    //    new_log_entry();
    if (verbose&4)     {
      console->print("after tu qso_process_radio="); console->println(so2r.qso_process_radio());
    }
    wipe_log_entry_radio(&radio_list[so2r.qso_process_radio()]); // up to here need to  use tx radio
    if (verbose &4) {
      console->print("ptr_curr=");console->println(so2r.radio_selected()->ptr_curr);
    }
    plogw->seqnr++;

    // check repeat func and start timer 
    if (so2r.sequence_mode() == SO2R::Repeat_Func || so2r.sequence_mode() == SO2R::SO2R_Repeat_Func) {
      if (so2r.msg_tx_radio() == so2r.focused_radio()) {
	so2r.request_set_rx(so2r.msg_tx_radio());
	if (so2r.sequence_stat()!=SO2R::Sending_Msg) {
	  // no tu or exchange being sent --> need to start timer2 to resume repeat cq 
	  so2r.repeat_timer2_start();
	  if (verbose&4) {
	    console->println("so2r start timer2 on enter");
	  }
	}
      }
    }
    
    // update display by the current focus
    so2r.qso_process_radio(so2r.focused_radio()); // set qso_process_radio to the focused 
    upd_display();
    bandmap_disp.f_update = 1;
    upd_display_bandmap();
    break;
  case 8:  // grid locator
    set_grid_locator_information();
    break;
  case 7:  // satellite name
    if (strlen(plogw->sat_name + 2) != 0) {

      // force stop rotator tracking
      plogw->f_rotator_track = 0;

      // satellite name entered
      sat_name_entered();

    } else {
      // go to contest qso
      if (!plogw->f_console_emu) plogw->ostream->println("non satellite");
      plogw->sat = 0;
      // force stop rotator tracking
      plogw->f_rotator_track = 0;
    }
    upd_display();
    break;
  case 20:  // rig entry
    set_rig_from_name(radio);
    //
    
    upd_display();
    break;
  case 21:  // cluster name
    // split port and body and copy to cluster
    set_cluster();
    //    connect_cluster();
    cluster.stat=0;// reset connection status
    upd_display();
    break;
  case 23:  // cluster cmd
    send_cluster_cmd();
    upd_display();
    break;
  case 25:  // wifi_ssid
    multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);
    break;
  case 26:  // wifi_passwd
    multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);    
    break;
  case 27:  // rig_spec_str -> set spec to the rig
    sprintf(dp->lcdbuf, "modifying rig_spec... ");
    upd_display_info_flash(dp->lcdbuf);
    set_rig_spec_from_str(radio,radio->rig_spec_str+2);
    if (verbose&4) {
    console->print("Now edit rig name is ");
    console->print(radio->rig_name+2);
    console->print(" and rig_spec rig name is ");
    console->print(radio->rig_spec->name);
    console->print(" and rig_spec cwport is ");
    console->print(radio->rig_spec->cwport);
    console->print(" and rig_spec_str is ");
    console->println(radio->rig_spec_str+2);
    }
    select_rig(radio);
    if (verbose&4) {
    console->print("After select_rig(), Now edit rig name is ");
    console->print(radio->rig_name+2);
    console->print(" and rig_spec rig name is ");
    console->print(radio->rig_spec->name);
    console->print(" and rig_spec cwport is ");
    console->print(radio->rig_spec->cwport);
    console->print(" and rig_spec_str is ");
    console->println(radio->rig_spec_str+2);
    }
    sprintf(dp->lcdbuf, "modifying rig_spec\nfinished.");
    upd_display_info_flash(dp->lcdbuf);
    break;
  case 28:  // zserver_name -> connect to zserver
    reconnect_zserver();
    break;
  case 40: // contest_name
    search_contest_id_from_name() ;

    switch(plogw->multi_type) {
    case MULTI_TYPE_NORMAL:      sprintf(buf,"NORMAL"); break;
    case MULTI_TYPE_CQWW:        sprintf(buf,"NORMAL/CQWW"); break;      
    case MULTI_TYPE_JARL_PWR:    sprintf(buf,"JARL PWR"); break;      
    case MULTI_TYPE_UEC:    sprintf(buf,"UEC"); break;      
    case MULTI_TYPE_JARL_PWR_NOMULTICHK:    sprintf(buf,"JARL PWR \nNOMULTICHK"); break;      
    case MULTI_TYPE_NOCHK_LASTCHR:    sprintf(buf,"IGN LASTCHR"); break;      
    case MULTI_TYPE_GL_NUMBERS:    sprintf(buf,"GL/NUMBER"); break;      
    case MULTI_TYPE_ARRLDX:    sprintf(buf,"ARRLDX"); break;      
    case MULTI_TYPE_ARRL10M:    sprintf(buf,"ARRL10M"); break;
    default:sprintf(buf,"N/A"); break;
    }
    
    sprintf(dp->lcdbuf, "contest \n%s\nselected.\nD:%s\nMult:%s",plogw->contest_name+2,plogw->mask == 0xff ? "OK C/P" : "NG C/P",buf);
    
    upd_display_info_flash(dp->lcdbuf);
    
    break;
    
  case 0:  // callsign entry
    // コールサイン入力前でS&Pの際onfreq の局があったらコールサインを取り込み。
    int len;
    int tmp;
    len = strlen(radio->callsign + 2);
    if (len == 0) {
      if (!radio->cq[radio->modetype]) {  // no existing callsign entry
	// S&P
	pick_onfreq_station();
      } 
      if (plogw->f_esm==1) {
	if (radio->cq[radio->modetype]==LOG_CQ) {
	  // CQ & ESM -> CQ send
	  //	  plogw->f_repeat_func_stat = 0;
	  so2r.cancel_msg_tx();	
	  //	  so2r.suspend();
	  //	  set_tx_to_focused();
	  so2r.set_msg_tx_to_focused();
	  //	  ui_send_cq(radio);
	  //	  so2r.msg_tx_radio(so2r.focused_radio()); // set message send tx to the currently focued radio	  
	  so2r.send_cq();
	}
      }
      break;
    }
    // check for commands
    if (strcmp(radio->callsign + 2, "DISCONN") == 0) {
      disconnect_cluster();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"PRINTKEY")==0) {
      f_printkey=1-f_printkey;
      sprintf(dp->lcdbuf, "printkey=%d", f_printkey);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "SATELLITE") == 0) {
      // first read tle information from sd file 
      readtlefile();
      // check time
      long int dt;
      //      dt=rtctime.unixtime()-plogw->tle_unixtime;
      dt=my_rtc.unixtime()-plogw->tle_unixtime;      
      sprintf(dp->lcdbuf, "TLE %ld hrs old\n",dt/3600);
      if (dt>84600*3) {
	// need to update from the internet
	strcat(dp->lcdbuf,"Update...\n");
	upd_display_info_flash(dp->lcdbuf);	
	getTLE();
      } else {
	strcat(dp->lcdbuf,"Keep.\n");
	upd_display_info_flash(dp->lcdbuf);
      }
      clear_buf(radio->callsign);
      break;
    }

    if (strncmp(radio->callsign + 2, "NEXTAOS", 7) == 0) {
      // find next aos
      start_calc_nextaos();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "SMETER") == 0) {
      plogw->show_smeter++;
      if (plogw->show_smeter >= 3) plogw->show_smeter = 0;
      sprintf(dp->lcdbuf, "show_smeter=%d", plogw->show_smeter);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "INTERVAL") == 0) {
      plogw->show_qso_interval=1-plogw->show_qso_interval;
      sprintf(dp->lcdbuf, "show_qso_interval=%d", plogw->show_qso_interval);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "SEQNR") == 0) {
      plogw->show_smeter=0;
      sprintf(dp->lcdbuf, "show_smeter=%d", plogw->show_smeter);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign +2, "DECODER") == 0 ) {
      decoder.start_i2s_adc_24k_rms_task();
      sprintf(dp->lcdbuf, "CW decoder start\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign +2, "DECODERSTOP") == 0 ) {
      decoder.stop_i2s_adc_24k_rms_task();
      sprintf(dp->lcdbuf, "CW decoder stopped\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strcmp(radio->callsign + 2, "NEWQSOLOG") == 0) {
      // create new QSO log
      create_new_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"LOADRIGS")==0) {
      load_rigs("RIGS");
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"SAVERIGS")==0) {
      save_rigs("RIGS");
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"RESETRIGS")==0) {
      //      save_rigs("RIGS");
      init_rig();
      clear_buf(radio->callsign);      
      break;
    }

    if (strcmp(radio->callsign+2,"MUXTRANS")==0) {
	f_mux_transport_cmd=1; // transition to mux
	sprintf(dp->lcdbuf,"mux_transport->mux\ncurrent=%d",f_mux_transport);
	upd_display_info_flash(dp->lcdbuf);
	clear_buf(radio->callsign);
	break;
    }
    
    if (strcmp(radio->callsign+2,"NOMUXTRANS")==0) {	
      f_mux_transport_cmd=2;
      sprintf(dp->lcdbuf,"mux_transport->no mux\ncurrent=%d",f_mux_transport);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strcmp(radio->callsign + 2, "PADDLENOR")==0) {
      paddle_setting&=~1;
      normal_paddle();      
      sprintf(dp->lcdbuf, "PADDLE\nNORMAL\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "PADDLEREV")==0) {
      paddle_setting|=1;
      //      reverse_paddle();
      set_paddle();
      sprintf(dp->lcdbuf, "PADDLE\nREVERSE\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "IAMBICA")==0) {
      paddle_setting|=2;
      //      paddle_iambic_a();
      set_paddle();
      sprintf(dp->lcdbuf, "IAMBIC_A\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "IAMBICB")==0) {
      paddle_setting&=~2;
      //      paddle_iambic_b();
      set_paddle();
      sprintf(dp->lcdbuf, "IAMBIC_B\n");      
      upd_display_info_flash(dp->lcdbuf);
            clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "KBDJP")==0) {
      kbdtype=1;
      Prs.jp();
      Prs1.jp();      
      sprintf(dp->lcdbuf, "Kbd:Japanese\n");      
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "KBDUS")==0) {
      kbdtype=0;
      Prs.us();
      Prs1.us();
      sprintf(dp->lcdbuf, "Kbd:US\n");      
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"RESETDISP")==0) {
      plogw->ostream->print("reset_display()");
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"DISPTYPE0")==0) {
      plogw->ostream->print("reset_display()");
      display_type=0;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }

    if (strcmp(radio->callsign+2,"DISPTYPE1")==0) {
      plogw->ostream->print("reset_display()");
      display_type=1;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"DISPTYPE2")==0) {
      plogw->ostream->print("reset_display()");
      display_type=2;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strcmp(radio->callsign + 2, "READQSOLOG") == 0) {
      // create new QSO log
      read_qso_log(READQSO_PRINT);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "MAILQSOLOG") == 0) {
      // mail qso log
      //        mail_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "MAKEDUPE") == 0) {
      init_score();
      //      init_multi();
      clear_multi_worked();      
      init_dupechk();
      sprintf(dp->lcdbuf, "Reading Log\nfor MAKEDUPE...\n");
      upd_display_info_flash(dp->lcdbuf);
      read_qso_log(READQSO_MAKEDUPE);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "ESM")==0 ) {
      // ESM toggle
      plogw->f_esm = 1- plogw->f_esm;
      sprintf(dp->lcdbuf, "ESM mode = %d\n",plogw->f_esm);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
      
    if (strncmp(radio->callsign + 2, "KEY", 3) == 0) {
      // change rigspec radio->rig_spec->cwport
      int tmp, tmp1;
      if ((tmp = sscanf(radio->callsign + 5, "%d", &tmp1)) == 1) {
	if (tmp1 >= 0 && tmp1 <= 2) {
	  radio->rig_spec->cwport = tmp1;
	  plogw->ostream->print("cwport -> ");
	  plogw->ostream->println(radio->rig_spec->cwport);
	}
      } else {
	plogw->ostream->println("invalid cwport");
      }
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "STRAIGHT") == 0) {
      // straight key flag toggle
      plogw->f_straightkey = 1 - plogw->f_straightkey;
      plogw->ostream->print("straight key enable = ");
      plogw->ostream->println(plogw->f_straightkey);
      sprintf(dp->lcdbuf, "straight key=%d", plogw->f_straightkey);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "TOGGLEPTT") == 0) {
      // straight key flag toggle
      plogw->f_toggle_ptt_mode = 1 - plogw->f_toggle_ptt_mode;
      plogw->ostream->print("toggle ptt by right shift key = ");
      plogw->ostream->println(plogw->f_toggle_ptt_mode);
      sprintf(dp->lcdbuf, "toggle ptt\nRIGHT SHIFT=%d", plogw->f_toggle_ptt_mode);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strncmp(radio->callsign + 2, "SAVE", 4) == 0) {

      // release other settings  including sat
      release_memory();

      save_settings(radio->callsign + 6);
      if (!plogw->f_console_emu) plogw->ostream->println("save");
      clear_buf(radio->callsign);
      break;
    }
    if (rotator_commands(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
    if (antenna_switch_commands(radio->callsign + 2)) {
      clear_buf(radio->callsign);

      break;
    }
    if (strncmp(radio->callsign + 2, "NEXTRIG", 7) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("next rig");
      switch_radios(radio->rig_idx,-1);
      break;
    }
    if (strncmp(radio->callsign + 2, "PREVRIG", 7) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("prev rig");
      switch_radios(radio->rig_idx,-2);      
      break;
    }
    if (strcmp(radio->callsign + 2, "ENABLERIG") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("enable rig");
      enable_radios(radio->rig_idx,1);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "DISABLERIG") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("disable rig");
      enable_radios(radio->rig_idx,0);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strncmp(radio->callsign + 2, "LOAD", 4) == 0) {
      load_settings(radio->callsign + 6);
      if (!plogw->f_console_emu) plogw->ostream->println("load");
      clear_buf(radio->callsign);
      break;
    }
    if (strncmp(radio->callsign + 2, "NATTO", 5) == 0) {
      cw_dah_ratio_bunshi=40;
      cw_ratio_bunbo=10;
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CWJQF", 5) == 0) {
      cw_duty_ratio=13;
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CWNORMAL", 5) == 0) {
      cw_dah_ratio_bunshi=30;
      cw_ratio_bunbo=10;
      cw_duty_ratio=10;      
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign+2,"VERBOSE",7)==0) {
      if (sscanf(radio->callsign+2+7,"%d",&tmp)==1) {
	verbose=tmp;
      } else {
	verbose=0;
      }
      break;
    }
    if (strcmp(radio->callsign+2,"ALTCQ")==0) {
      // alt cq
      so2r.sequence_mode(SO2R::SO2R_ALTCQ);
      so2r.sequence_stat(SO2R::Default);      
      if (verbose &4) {
	console->println("SO2R_ALTCQ");
      }
      clear_buf(radio->callsign);            
      break;
    }
    if (strcmp(radio->callsign+2,"2BSIQ")==0) {
      // 2bsiq
      so2r.sequence_mode(SO2R::SO2R_2BSIQ);
      so2r.sequence_stat(SO2R::Default);      
      if (verbose&4)       console->println("SO2R_2BSIQ");
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CALLHIST", 8) == 0) {
      set_callhistfn(radio->callsign + 10);
      plogw->enable_callhist = 1;
      //      open_callhist();
      read_callhist_list(callhistfn);
      if (!plogw->f_console_emu) plogw->ostream->println("callhist");
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "LISTDIR") == 0) {
      listDir(SD, "/", 0);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"ADCSTAT")==0) {
      adc_statistics();
      break;
    }
    if (strcmp(radio->callsign + 2, "HELP") == 0) {
      print_help(plogw->help_idx++);
      if (plogw->help_idx >= 7) plogw->help_idx = 0;
      //        clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "HELPR") == 0) {
      print_help(plogw->help_idx--);
      if (plogw->help_idx == -1) plogw->help_idx = 7;
      //        clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "WIFI") == 0) {
      wifi_enable = 1 - wifi_enable;
      cluster.stat = 0;
      wifi_count = 0;
      if (!wifi_enable) {
	WiFi.disconnect(true);
	WiFi.mode(WIFI_OFF);
      } else {
			
      }
      sprintf(dp->lcdbuf, "wifi_enable=%d", wifi_enable);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DUMPQSOLOG") == 0) {
      // create new QSO log
      dump_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "EXITEMU") == 0) {
      // exit from console_emu
      plogw->f_console_emu = 0;
      plogw->ostream->println("\nexit from console_emu");
      clear_buf(radio->callsign);
      break;
    }
    //      if (strncmp(radio->callsign + 2, "BAND", 4) == 0) {
    //        switch_bands(radio);
    //        break;
    //      }
    if (strncmp(radio->callsign + 2, "RADIO", 5) == 0) {
      // config ratio command
      // radio radio#+0 disable 1 enable 2 prev_radio 3 next_radio
      int tmp, tmp1;
      tmp1 = sscanf(radio->callsign + 7, "%d", &tmp);
      if (tmp1 == 1) {
	int idx_radio, radio_cmd;
	radio_cmd = tmp % 10;
	idx_radio = radio_cmd / 10;
	plogw->ostream->print("radio cmd idx=");
	plogw->ostream->println(idx_radio);
	plogw->ostream->print(" cmd 0 disable 1 enable 2 sw - 3 sw + =");
	plogw->ostream->println(radio_cmd);
	if (idx_radio < 0 || idx_radio > 2) break;  // invalid radio#
	switch (radio_cmd) {
	case 0:  // disable
	  enable_radios(idx_radio, 0);
	  break;
	case 1:  // enable
	  enable_radios(idx_radio, 1);
	  break;
	case 2:  // setch -
	  switch_radios(idx_radio, -1);
	  break;
	case 3:  // switch +
	  switch_radios(idx_radio, -2);
	  break;
	}
      }
      break;
    }

    if (strncmp(radio->callsign + 2, "BANDEN", 6) == 0) {
      // define bandenable in hex
      if (*(radio->callsign+2+6)!='\0') {
	radio->band_mask = (~strtol(radio->callsign + 2 + 6, NULL, 16))&((1<<(N_BAND-1))-1);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band_mask=");
	plogw->ostream->println(radio->band_mask, HEX);
      }
      print_band_mask(radio);
      break;
    }

    if (strncmp(radio->callsign + 2, "BANDMASK", 8) == 0) {
      // define bandmask in hex
      if (*(radio->callsign+2+8)!='\0') {
	radio->band_mask = strtol(radio->callsign + 2 + 8, NULL, 16);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band_mask=");
	plogw->ostream->println(radio->band_mask, HEX);
      }
      print_band_mask(radio);
      break;
    }
    if (signal_command(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
    if (antenna_alternate_command(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
	  
    if (strncmp(radio->callsign + 2, "BANDMAP", 7) == 0) {
      // define bandmap_mask  in hex
      if (*(radio->callsign+2+7)!='\0') {      
	bandmap_mask = strtol(radio->callsign + 2 + 7, NULL, 16);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("bandmap_mask=");
	plogw->ostream->println(bandmap_mask, HEX);
      }
      print_bandmap_mask();
      break;
    }
    // check if mode change, freq change
    if (rig_modenum(radio->callsign + 2) != -1) {
      if (is_manual_rig(radio)) { // manual rig set directly
	set_mode_nonfil(radio->callsign +2,radio);
      } else {
	if (plogw->sat) {
	  set_sat_opmode(radio, radio->callsign + 2);
	} else {
	  if (verbose & 1) {
	    plogw->ostream->print("setting mode filt ");
	    plogw->ostream->println(radio->modetype);
	    plogw->ostream->println(radio->cq[radio->modetype]);
	    plogw->ostream->println(radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype]);
	  }
	  send_mode_set_civ(radio->callsign + 2, radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype]);  // cw phone switch filter selection!
	}
      }
      clear_buf(radio->callsign);
      break;
    }
    // frequency entry
    double frequency;
    frequency = check_frequency((String)(radio->callsign + 2));
    if (frequency != ErrorFloat) {
      if (verbose & 1) {
	plogw->ostream->print("setting frequency(kHz):");
	plogw->ostream->println(frequency);
      }
      if (is_manual_rig(radio)) {
	set_frequency((unsigned int)(frequency * (1000/FREQ_UNIT)), radio);
	radio->f_freqchange_pending=0;
      } else {
	set_frequency_rig((unsigned int)(frequency * (1000/FREQ_UNIT)));  // kHz -> Hz
      }
      clear_buf(radio->callsign);      
      break;
    }

    // ESM
    if (plogw->f_esm==1) {
      if (radio->cq[radio->modetype]) {
	// CQ -> send his callsign and number and move to exch
	ui_response_call_and_move_to_exch(radio); // send call and number and move to exch
      } else {
	// S&P -> send my callsign
	ui_send_mycall(radio);
      }
    }
    break;
  }
  upd_display();
  // break;

  //          case 1:
  //            append_cwbuf(c); break;
}

int check_edit_mode()  // CW key input and Remarks  return 1 (insert) else return 0 (overwrite edit)
{
  struct radio *radio;
  radio = so2r.radio_selected();
  if (radio->ptr_curr == 0) return 2;   // callsign edit  (　insert mode edit exception)
  if (radio->ptr_curr == 6) return 1;   // Remarks edit
  if (radio->ptr_curr == 9) return 1;   // jcc edit  
  if (radio->ptr_curr == 21) return 1;  // cluster  name edit
  if (radio->ptr_curr == 22) return 1;  // email edit
  if (radio->ptr_curr == 25) return 1;  // wifi ssid
  if (radio->ptr_curr == 26) return 1;  // wifi pass
  if (radio->ptr_curr == 23) return 1;  // cluster cmd edit
  if (radio->ptr_curr == 27) return 1;  // rig_spec_str
  if (radio->ptr_curr == 28) return 1;  // zserver_name
  if (radio->ptr_curr == 29) return 1;  // my_name
  if (radio->ptr_curr == 40) return 0;  // contest_name

  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) return 1;  // CW key msg
  if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) return 1;  // CW key msg
  return 0;
}


int ptr_curr_req_uppercase(struct radio *radio) {
    if ((radio->ptr_curr != 6) && (radio->ptr_curr != 20) &&(radio->ptr_curr != 21) && (radio->ptr_curr != 22) && (radio->ptr_curr != 23) && (radio->ptr_curr != 25) && (radio->ptr_curr != 26) && (radio->ptr_curr != 27) && (radio->ptr_curr != 28) && (radio->ptr_curr != 29) && (radio->ptr_curr != 40) )   {
      return 1;
    } else {
      return 0;
    }
  
}

int ptr_curr_req_nospace(struct radio *radio) {
  if (
      (!((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)))
      && (!((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)))
      && (radio->ptr_curr != 6)
      && (radio->ptr_curr != 23)
      && (radio->ptr_curr != 28)
      && (radio->ptr_curr != 29)
      && (radio->ptr_curr != 9)
      ) {
    // JCC allow space
      return 1;
    } else {
      return 0;
    }
}

int ptr_curr_req_callsign_exch_chr(struct radio *radio) {
  switch(radio->ptr_curr) {
  case 0:
  case 1:
  case 4:
    //  case 5: // now allow macro expansion so allow non callsign exchange chr for sent_exch
    return 1;
  default:
    return 0;
  }
}


int ptr_curr_req_num(struct radio *radio) {
  switch(radio->ptr_curr) {
  case 2:
  case 3:
    return 1;
  default:
    return 0;
  }
}


void logw_handler(char key, char c)
// key: key code
// c: ascii code 0x00 --> non character keys
{

  struct radio *radio;
  radio = so2r.radio_selected();
  char *pwin;  // pointer to the window
  // handle input character  in window
  // ptr_curr: 10-14(N_CWMSG-1)  cw_msg window
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    pwin = plogw->cw_msg[radio->ptr_curr - 10];
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // cw msg
      pwin = plogw->rtty_msg[radio->ptr_curr - 30];
    } else {
      switch (radio->ptr_curr) {
      case 0:  // call sign window
	pwin = radio->callsign;
	break;
      case 1:  // my exchange
	pwin = radio->recv_exch;
	break;
      case 2:  // his  rst
	pwin = radio->sent_rst;
	break;
      case 3:  // his rst (recv_rst)
	pwin = radio->recv_rst;
	break;
      case 4:  // my callsign
	pwin = plogw->my_callsign;
	break;
      case 5:  // his exchange
	pwin = plogw->sent_exch;
	break;
      case 6:  // remarks
	//plogw->ostream->println("remarks");
	pwin = radio->remarks;
	break;
      case 7:  // sat name
	pwin = plogw->sat_name;
	break;
      case 8:  // grid locator
	pwin = plogw->grid_locator;
	break;
      case 9:  // jcc
	pwin = plogw->jcc;
	break;
      case 20:  // rig_name
	pwin = radio->rig_name;
	break;
      case 21:  // cluster_name
	pwin = plogw->cluster_name;
	break;
      case 22:  // email_addr
	pwin = plogw->email_addr;
	break;
      case 23:  // cluster_cmd
	pwin = plogw->cluster_cmd;
	break;
      case 24:  // power_code`
	pwin = plogw->power_code;
	break;
      case 25:  // wifi_ssid
	pwin = plogw->wifi_ssid;
	break;
      case 26:  // wifi_passwd
	pwin = plogw->wifi_passwd;
	break;
      case 27:  // rig_spec
	pwin = radio->rig_spec_str;
	break;
      case 28:  // zserver_name
	pwin = plogw->zserver_name;
	break;
      case 29:  // my_name
	pwin = plogw->my_name;
	break;
      case 40:  // contest_name
	pwin = plogw->contest_name;
	break;
	
      default:
	// do nothing and ignore
	if (verbose & 1) {
	  plogw->ostream->print("ptr_curr!?=");
	  plogw->ostream->println(radio->ptr_curr);
	}
	return;
      }
    }
  }
  // check keys
  if (c == 0x00) {
    switch (key) {
    case CHR_BS:
      // backspace character
      if (verbose & 1) plogw->ostream->println("(BS)");
      backspace_buf(pwin);
      break;
    case CHR_DEL:
      if (verbose & 1) plogw->ostream->println("(DEL)");
      delete_buf(pwin);
      break;
    case 0x4f:  // RIGHT
      if (verbose & 1) plogw->ostream->println("(RIGHT)");
      right_buf(pwin);
      break;
    case 0x50:  // LEFT
      if (verbose & 1) plogw->ostream->println("(LEFT)");
      left_buf(pwin);
      break;
    default:
      break;
    }
  } else {
    //plogw->ostream->println("chk");
    // other character change to upper case
    // list ptr_curr in which cases matters 
      // not cluster address (need small characters to allow input )
    if (ptr_curr_req_uppercase(radio)) {
      if (c >= 'a' && c <= 'z') c += 'A' - 'a';
    }
    
    if (ptr_curr_req_callsign_exch_chr(radio)) {
      if (!(isalnum(c)||(c=='/')||(c=='.'))) { // callsign characters
	return ;
      }
    } else {
      if (ptr_curr_req_num(radio)) {
	if (!isdigit(c)) {
	  return;
	}
      }
    }
    if (ptr_curr_req_nospace(radio)) {
      // do not allow space
      if (isSpace(c)) return;
    }
    if (!isPrintable(c)) return;
    //plogw->ostream->println("chk1");
    if (check_edit_mode()) {
      insert_buf(c, pwin);  // insert to buffer in cw msg edit mode
    } else {
      overwrite_buf(c, pwin);  // overwriting is more appropriate than insert in logging
    }
    // activity in log window leads to memorize frequency if S&P
    if (radio->cq[radio->modetype] == LOG_SandP) {
      // memorize current frequency to freqbank s and p freqnency if s&p mode
      save_freq_mode_filt(radio);
    }

  }

  
    // on-demand processes after editing
    switch (radio->ptr_curr) {
    case 0:  // call sign window
      if (strlen(radio->callsign + 2) >= 5) {
	// dupecheck
	if (dupe_check(radio,radio->callsign + 2, bandmode(radio), plogw->mask, 0)) {  // cw/ssb both ok ... 0xff cw/ssb not ok 0xff-3
	  radio->dupe = 1;
	  // duped callsign and frequency is registered in the bandmap in S&P //(only for phone?)
	  //	  if (radio->modetype==LOG_MODETYPE_PH) {
	  if (radio->cq[radio->modetype] == LOG_SandP) {
	    register_current_callsign_bandmap();
	  }
	} else {
	  radio->dupe = 0;
	}

      }
      // partial check if appricable
      ui_perform_partial_check(radio);
      
      
      break;
    case 1:  // recv_exch
      if (strlen(radio->recv_exch + 2) >= 1) {
	radio->multi = multi_check(radio->recv_exch+2,radio->bandid);
	if (radio->multi >= 0) {
	  upd_display_info_contest_settings(radio);
	}
      }
      break;
    }


  upd_display();
}

void 	check_call_show_dx_entity_info(struct radio *radio) {
  if (strlen(radio->callsign+2)>2) {
    switch (plogw->multi_type) {
    case MULTI_TYPE_CQWW:
      show_entity_info(radio->callsign+2);
      break;
    }
  }
}



// switch from a entry to the other with TAB or SPC
void switch_logw_entry(int option) {
  struct radio *radio;
  radio = so2r.radio_selected();
  switch (option) {
  case 3:  // direct move to Callsign
    radio->ptr_curr = 0;
    break;
  case 2:  // direct move to Remarks
    radio->ptr_curr = 6;
    break;
  case 4:  // direct move to Rcv rst
    radio->ptr_curr = 3;
    if (strlen(radio->recv_rst+2)>=2) {
      radio->recv_rst[1]=1; // cursor move to S position
    }
    break;
  case 5:  // direct move to recv_exch
    radio->ptr_curr = 1;
    break;
  case 6: // direct move to Sent rst 
    radio->ptr_curr = 2 ;
    if (strlen(radio->sent_rst+2)>=2) {
      radio->sent_rst[1]=1; // cursor move to S position
    }
    break;
  case 0:  // forward
    if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func forward");
      radio->ptr_curr = radio->ptr_curr + 1;
      if (radio->ptr_curr >= 10 + N_CWMSG) radio->ptr_curr = 10;
      break;
    }
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func forward");
      radio->ptr_curr = radio->ptr_curr + 1;
      if (radio->ptr_curr >= 30 + N_CWMSG) radio->ptr_curr = 30;
      break;
    }

    switch (radio->ptr_curr) {
    case 0:  // his callsign
      // check callsign for callhist, my history and fill in my exchange
      // dupe check
      if (strlen(radio->callsign + 2) >= 3) {
	if (dupe_check(radio,radio->callsign + 2, bandmode(radio), plogw->mask, 1)) {  // cw/ssb both ok ... 0xff cw/ssb not ok 0xff-3
	  radio->dupe = 1;
	} else {
	  radio->dupe = 0;
	}
	// 
	check_call_show_dx_entity_info(radio);
      }
      radio->ptr_curr = 1;
      break;
    case 1:  // my exchange
      radio->ptr_curr = 2;
      break;
    case 2:  // my rst (sent rst)
      radio->ptr_curr = 3;
      break;
    case 3:  // his rst (recv rst)
      radio->ptr_curr = 4;
      break;
    case 4:  // my_callsign
      radio->ptr_curr = 5;
      break;
    case 5:  // his exchange
      radio->ptr_curr = 6;
      break;
    case 6:  // remarks
      radio->ptr_curr = 40;
      break;
    case 40: // contest_name
      radio->ptr_curr = 7;
      break;
    case 7:  // sat name
      radio->ptr_curr = 8;
      break;
    case 8:  // grid locator
      radio->ptr_curr = 9;
      break;
    case 9:  // jcc
      radio->ptr_curr = 29;
      break;
    case 29:  // my_name (next to jcc)
      radio->ptr_curr = 20;
      break;
    case 20:  // rig_name
      radio->ptr_curr = 27;
      break;
    case 27:  // rig_spec_str (next to rig_name)
      radio->ptr_curr = 21;
      break;
    case 21:  // cluster_name
      radio->ptr_curr = 23;
      break;
    case 23:  // cluster_cmd
      radio->ptr_curr = 22;
      break;
    case 22:  // email_addr
      radio->ptr_curr = 24;
      break;
    case 24:  // power
      radio->ptr_curr = 25;
      break;
    case 25:  // wifi_ssid
      radio->ptr_curr = 26;
      break;
    case 26:  // wifi_passwd
      radio->ptr_curr = 28;
      break;
    case 28:  // zserver_name
      radio->ptr_curr = 0;
      break;
    }
    break;
  case 1:  // backward  easier to go back to exchange
    if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func backward");
      radio->ptr_curr = radio->ptr_curr - 1;
      if (radio->ptr_curr < 10) radio->ptr_curr = 10 + N_CWMSG - 1;
      break;
    }
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func backward");
      radio->ptr_curr = radio->ptr_curr - 1;
      if (radio->ptr_curr < 30) radio->ptr_curr = 30 + N_CWMSG - 1;
      break;
    }
    switch (radio->ptr_curr) {
    case 0:  // his callsign
      // check callsign for callhist, my history and fill in my exchange
      radio->ptr_curr = 1;
      break;
    case 1:  // my exchange
      radio->ptr_curr = 0;
      break;
    case 2:  // my rst
      radio->ptr_curr = 1;
      break;
    case 3:  // his rst
      radio->ptr_curr = 2;
      break;
    case 4:  // my_callsign
      radio->ptr_curr = 3;
      break;
    case 5:  // his exchange
      radio->ptr_curr = 4;
      break;
    case 6:  // remarks
      radio->ptr_curr = 5;
      break;
    case 40: // contest_name
      radio->ptr_curr = 6;
      break;
    case 7:  // sat name
      radio->ptr_curr = 40;
      break;
    case 8:  // grid locator
      radio->ptr_curr = 7;
      break;
    case 9:  // jcc
      radio->ptr_curr = 8;
      break;
    case 29:  // my_name (next to jcc)
      radio->ptr_curr = 9;
      break;
    case 20:  // rig_name
      radio->ptr_curr = 29;
      break;
    case 27:  // rig_spec_str
      radio->ptr_curr = 20;
      break;
    case 21:  // cluster_name
      radio->ptr_curr = 27;
      break;
    case 23:  // cluster_cmd
      radio->ptr_curr = 21;
      break;
    case 22:  // email_addr
      radio->ptr_curr = 23;
      break;
    case 24:  // power_code
      radio->ptr_curr = 22;
      break;
    case 25:  // wifi_ssid
      radio->ptr_curr = 24;
      break;
    case 26:  // wifi_passwd
      radio->ptr_curr = 25;
      break;
    case 28:  // zserver_name
      radio->ptr_curr = 26;
      break;
    }
    break;
  }
  if (verbose & 1) {
    plogw->ostream->print("ptr_curr:");
    plogw->ostream->println(radio->ptr_curr);
    plogw->ostream->print("\t");
  }
  upd_display();
}


void sat_name_entered() {
  plogw->sat = 1;
  // check sat_name
  //#ifdef notdef
  // check if tlefile read
  if (plogw->tle_unixtime==0) {
    readtlefile();
  }
  if (strcmp(plogw->sat_name + 2, plogw->sat_name_set) != 0) {
    load_satinfo();
    // set new satellite
    if (find_satname(plogw->sat_name + 2) != -1) {
      strcpy(plogw->sat_name_set, plogw->sat_name + 2);
      //        set_sat_info_calc(plogw->sat_name_set);
      set_sat_index(plogw->sat_name_set);

      // automatically set sat_vfo_mode from
      // 1) how many rig enabled  single and two trx
      // 2) band assigned for the radio #0 and #2
      // 3) rig_type (IC-705, IC-9700)
      int n_active_rigs;
      int i;
      i = plogw->sat_idx_selected;
      if (radio_list[1].enabled && radio_list[0].enabled) {
        // two trx
        if (freq2bandid(sat_info[i].up_f0) == radio_list[0].bandid) {
          // uplink is radio 0
          plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_0;
          // select radio to 0 ( transmit focus )
	  //          select_radio(0);
	  so2r.change_focused_radio(0);

        } else {
          if (freq2bandid(sat_info[i].up_f0) == radio_list[1].bandid) {
            // uplink is radio 1
            plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_1;
            // select radio 1
	    //            select_radio(1);
	    so2r.change_focused_radio(1);
          }
        }
      } else {
        // single trx
        switch (radio_list[0].rig_spec->rig_type) {
	case 0:  // IC-705
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
	  break;
	case 1:  // IC-9700
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_TX;
	  break;
	default:  // others RX selected
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
	  break;
        }
      }
      struct radio *radio;
      radio=so2r.radio_selected();
      // set satellite opmode
      //set_sat_opmode(plogw->opmode);
      set_sat_opmode(radio, "CW");
    }
  }
  //#endif
}


// print help on the left screen
void print_help(int option) {
  switch (option) {
  case 0:  // basic help 0
    sprintf(dp->lcdbuf, "A-Home:SW_RIG\nA-End:Tgl_Rig\nA-x:Tgl_Xvtr\nA-m:SW_Mode\nA-t:Xmit,-b:BMapSrt\n");

    break;
  case 1:  // on the callsign enter
    sprintf(dp->lcdbuf, "SATELLITE\nNEW/READ/MAIL/\nDUMPQSOLOG\nMAKEDUPE,LISTDIR\nSAVE/LOAD\n");
    break;
  case 2:  // Ctrl
    sprintf(dp->lcdbuf, "C-r,c,s:Rem,Cl,RST\nC-f:Rem->Mul,-v:Tgl_Voice\nC-j,p,n,o:Cur,Prev,Next,Last\nC-1,2:C/PDupe,Contest\nC-3,t,y:MaskBandmap,MultiShow\n");
    break;
  case 3:  // Ctrl #2
    sprintf(dp->lcdbuf, "C-'-':editQSO\nC-4:callhist\nC-5:SO1R/SAT/SO2R\nC-u:Score\nPGUP/DN:CW SPD\nC-v:Voice\n");
    break;

  case 4:  // Ctrl #2
    sprintf(dp->lcdbuf, "A-'-':Scope\nA-q:CW/S&P\nA-a:nextAOS\nA-p:pick sat AOS\nA-b:beacon \nA-c:CW/RTTY edit\n");
    break;
  case 5:  // SAT
    sprintf(dp->lcdbuf, "A-d,n:bandmap del/add.\nA-g:tone key\nA-Spc:pick spot\nA-DEL:show rig info\nA-f:set center freq.\nA-r:vfo mode sw\nA-s:track mode");
    break;
  case 6:  // Ctrl #2
    sprintf(dp->lcdbuf, "A-w:wipe QSO\nA-<=>:bandmap sel.\nA-m:mode\nA-<>:band\nC-l:QSLcard\n");
    break;
  }
  upd_display_info_flash(dp->lcdbuf);
}
