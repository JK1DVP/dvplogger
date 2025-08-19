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
#include "callhist.h"
#include "qso.h"
#include "dupechk.h"
#include "ui.h"
#include "log.h"
#include "multi_process.h"
#include "misc.h"
#include "cat.h"
#include "cluster.h"
#include "contest.h"
#include "cluster.h"
#include "settings.h"
#include "bandmap.h"
#include "display.h"
#include "main.h"
#include "sd_files.h"
#include "SD.h"
#include "mcp.h"
#include "satellite.h"
#include "console.h"
#include "timekeep.h"
#include "esp32_flasher.h"
#include "so2r.h"
#include "network.h"
#include "dac-adc.h"
#include "mcp_interface.h"
#include "morse_decoder_simple.h"
int cmd_interp_state = 0;
// command interpreter
// callhist_set
// dumpqsolog
//

char *strtoupper(char *s) {
  char *p;
  p = s;
  while (*p) {
    *p = toupper(*p);
    p++;
  }
  return s;
}

void play_cw_cmd(char *cmd)
{
  struct radio *radio;
  if (f_mux_transport) {
    char buf[80];
    if (strlen(cmd)<80) {
      sprintf(buf,"playc%s",cmd);
      plogw->f_playing=1;
      // ptt control
      radio=so2r.radio_tx();
      set_ptt_rig(radio,1);
      radio->ptt=1;
      console->print("ptt on and ");
      mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
      console->print("sent ");console->println(buf);
      so2r.set_queue_monitor_status(1);
    }
  }
}

void play_wpm_set()
{
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"playw%d",cw_spd);
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    console->print("sent ");console->println(buf);
  }
}

void play_wpm_cmd(char *cmd)
{
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"playw%s",cmd);
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    console->print("sent ");console->println(buf);
  }
  
}

void play_queue_cmd()
{
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"playq");
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    console->print("sent ");console->println(buf);	  
  }
}

void play_stop_cmd() {
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"plays");
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    console->print("sent ");console->println(buf);	  
  }
}

void play_string_cmd(char *cmd)
{
  struct radio *radio;
  if (f_mux_transport) {
    char buf[40];
    if (strlen(cmd)<34) {
      sprintf(buf,"playp%s",cmd);
      plogw->f_playing=1;
      // ptt control
      radio=so2r.radio_tx();
      set_ptt_rig(radio,1);
      radio->ptt=1;
      console->print("ptt on and ");
      mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
      console->print("sent ");console->println(buf);
      so2r.set_queue_monitor_status(1);
    }
  }
}

void cmd_interp(char *cmd) {
  int tmp, tmp1;
  struct radio *radio;
  switch (cmd_interp_state) {
    case 0:  // command line
      plogw->ostream->print("cmd:");
      plogw->ostream->println(cmd);

      if (strcmp("loadsat", cmd) == 0) {
	load_satinfo();
        break;
      }
      if (rotator_commands(cmd)) break;
      if (antenna_switch_commands(cmd)) {
		  break;
	  }
      if (signal_command(cmd)) break;
      if (antenna_alternate_command(cmd)) break;
      if (strcmp("savesat", cmd) == 0) {
	save_satinfo();
        break;
      }
      if (strcmp(cmd,"decoderstop")==0) {
	decoder.stop_i2s_adc_24k_rms_task(); // stop morse decoder
	console->print("stopped morse decoder");
	break;
      }
      if (strcmp(cmd,"decoder")==0) {
	decoder.start_i2s_adc_24k_rms_task();// start morse decoder
	console->print("started morse decoder");
	break;
      }
      
      if (strcmp(cmd, "emu") == 0) {
        // enter into console terminal emulation mode
        plogw->f_console_emu = 1;
	// send clear screen command
	char buf[40];	
	for (int i=0;i<20;i++) {
	  sprintf(buf,"\033[%d;1H\033[K",i);
	  plogw->ostream->print(buf);
	}
        break;
      }
      
      if (strncmp(cmd,"play ",5)==0) {
	// play_string
	console->println("play command");
	play_string_cmd(cmd+5);
	break;
      }
      if (strncmp(cmd,"playcw",6)==0) {
	// play_string
	console->println("playcw command");
	play_cw_cmd(cmd+6);
	break;
      }
      if (strncmp(cmd,"playwpm",7)==0) {
	// play queue query
	console->println("playwpm command");
	play_wpm_cmd(cmd+7);
	break;
      }
      if (strncmp(cmd,"playq",5)==0) {
	// play queue query
	console->println("playq command");
	play_queue_cmd();
	break;
      }
      if (strcmp(cmd,"serial")==0) {
	// status of serial port allocation
	print_serial_instance();
	break;
      }
      if (strncmp(cmd, "send ", 5) == 0) {
	//        SO2Rprint(cmd + 5);
	Serial2.println(cmd+5);
	console->print("sent Serial2:");console->println(cmd+5);
        break;
      }
      if (strcmp(cmd,"i2c_scan")==0) {
	// scan i2c bus and print result
	i2c_scan();
	break;
      }
      if (strcmp(cmd,"usb_desc")==0) {
	USB_desc();
	break;
      }

      if (strcmp(cmd,"ntp_stat")==0) {
	print_ntpstatus();
	break;
      }

      if (strcmp(cmd,"reset_display")==0) {
	  plogw->ostream->print("init_display()");
	  init_display();
	  break;
      }
      if (strncmp(cmd,"flashersd",9)==0) {
	console->println("flashersd boot part app spiffs (put what you want to flash).");
	// stop mux serial port
	deinit_mux_serial();
	// stop using SD card
	close_qsolog();
	console->println("esp_flashersd() ... ");		
	//      	esp_flasher();
	esp_flasher_sd(cmd+9);
	console->println("esp_flashersd() end... ");		
	// restore mux serial port
	init_mux_serial();
	attach_interrupt_civ();
      	break;
      }

      if (strcmp(cmd,"subcpu_halt")==0) {
	// keep subcpu reset pin low to halt subcpu
	//const uint8_t reset_trigger_mcp_pin = 15;
	mcp_write_pin(15, 0);
	console->println("halted subcpu by keep en pin low.");
	break;
      }
      
      if (strcmp(cmd,"flasher")==0) {
	// stop mux serial port
	deinit_mux_serial();
	close_qsolog();
	console->println("esp_flasher() ... ");
	esp_flasher();
	console->println("esp_flasher() end... ");	
	// restore mux serial port
	init_mux_serial();
	attach_interrupt_civ();
      	break;
      }
      if (strncmp(cmd, "verbose", 7) == 0) {
        tmp1 = sscanf(cmd + 7, "%d", &tmp);
        if (tmp1 == 1) {
          verbose = tmp;
        } else {
          if (verbose > 0) {
            verbose = 0;
          } else {
            verbose = 1;
          }
        }
        break;
      }
      if (strncmp(cmd, "nextaos", 7) == 0) {
	start_calc_nextaos();
        break;
      }
      if (strcmp(cmd, "satellite") == 0) {
        f_sat_updated = 0;  // reset flag
	allocate_sat();
	getTLE();
        break;
      }
      if (strncmp(cmd,"addap ",6)==0) {
	console->println("addap command");	
	char arg1[100];char arg0[100];
	copy_token(arg0,cmd+6,0," ");
	copy_token(arg1,cmd+6,1," ");
	multiwifi_addap(arg0,arg1);

	break;
      }

      if (strncmp(cmd, "gpio", 4) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 4, "%d %d", &tmp, &tmp1) == 2) {
	  write_mcp_gpio(tmp,tmp1);

          plogw->ostream->print("write mcp gpio port ");
          plogw->ostream->print(tmp);
          plogw->ostream->print(" value ");
          plogw->ostream->println(tmp1);
          for (int i = 0; i < 16; i++) {
            plogw->ostream->print(i);
            plogw->ostream->print(" ");
            plogw->ostream->println(read_mcp_gpio(i));
          }
        } else {
          plogw->ostream->println("gpio mcp param error");
        }
        break;
      }



      if (strcmp(cmd, "listdir") == 0) {
        listDir(SD, "/", 0);
        break;
      }
      if (strcmp(cmd, "help") == 0) {
        // show help of commands
        plogw->ostream->println("Available Commands:");
        plogw->ostream->println("emu (entering terminal emulation EXITEMU to end)/verbose[num]\nhelp (show this)\nloadsat/nextaos/satellite\n\n// QSO log commands\nnewqsolog (start new log QSO.TXT)\nmakedupe (dupe/multi check from QSO.TXT)\ndumpqso[num](dump raw qso file [num] if read backup files\nreadqso (read log QSO.txt in ctestwin txt imporable format)\nlistdir (show directory content)\n\n// CLUSTER commands\nDx de (push cluster information)\n\n// RADIO remote control commands\nsetstninfo{Callsign} (set target to work station information)\nstatus (get status of the radios)\nswitch_radio {radio#}/enable_radio {radio#}\nfocus {radio#} (change focused radio)\n\n// SETTINGS commands\nsave[name] (save settings)\nload[name]\nsettings (show settings information)\nassign{variable_name} {value} (assign settings variables)\npost_assign (perform post assignment tasks)\n\n// CALLHISTORY commands\ncallhist_set [callhist_filename] (start setting callhist information,input lines of callsign exchange,end will end setting.\ncallhist_open [callhist_fn] (set callhist filename and open)\ncallhist_search (start callhist search, input callsign to search, end will end search session.)\n\n// MAINTENANCE commands\ngpio[port] [value](write mcp gpio port)\nreset_settings/restart_dvplogger\nadcstat (show adc statistics)\n");

        break;
      }
      if (strncmp("callhist_set", cmd, 12) == 0) {
        plogw->ostream->println("callhist_set command");
        if (cmd[12] == ' ') {
          set_callhistfn(cmd + 13);
        } else {
          set_callhistfn("");
        }
        cmd_interp_state = 1;
        break;
      }
      if (strncmp(cmd, "DX de", 5) == 0) {
        // cluster info push from serial
        get_info_cluster(cmd);
        break;
      }
      if (strcmp("mem",cmd)==0) {
	print_memory();
	break;
      }
      if (strcmp("dumpcur",cmd)==0) {
	dump_qso_current();
	break;
      }
      if (strcmp("dumptop",cmd)==0) {
	info_disp.pos=0;
	dump_qso_current();
	break;
      }
      if (strcmp("dumpnext",cmd)==0) {
	info_disp.pos+=sizeof(qso.all);
	dump_qso_current();
	break;
      }
      if (strcmp("dumpprev",cmd)==0) {
	info_disp.pos-=sizeof(qso.all);
	if (info_disp.pos<0) info_disp.pos=0;
	dump_qso_current();
	break;
      }
      if (strcmp("dumplast", cmd) == 0) {      
	unsigned long pos = qsologf.position();
	info_disp.pos = pos - sizeof(qso.all);
	if (info_disp.pos < 0) {
	  info_disp.pos = 0;
	}
	dump_qso_current();
	break;
      }

      if (strncmp("dump ",cmd,5) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 5, "%d", &tmp) == 1) {
	  unsigned long pos = qsologf.position();
	  if (tmp*sizeof(qso.all) <= pos) {
	    info_disp.pos = tmp* sizeof(qso.all);
	    if (info_disp.pos < 0) {
	      info_disp.pos = 0;
	    }
	    dump_qso_current();
	  } else {
	    plogw->ostream->print(tmp);
	    plogw->ostream->println(", beyond range <");
	    plogw->ostream->print(pos/sizeof(qso.all));
	  }
	} else {
	    plogw->ostream->println("dump qso#(0-)");
	}
	break;
      }
      
      if (strncmp("dumpqso", cmd, 7) == 0) {
        plogw->ostream->println("dumpqso command");
        plogw->ostream->println(cmd + 8);
	if (strlen(cmd)>7) {
	  dump_qso_bak(cmd + 8);
	} else {
	  plogw->ostream->println("dumping current qso log.");	  
	  dump_qso_log();
	}
        break;
      }

      
      if (strncmp("readqso", cmd, 7) == 0) {
        plogw->ostream->println("readqso command");
        read_qso_log(READQSO_PRINT);
        break;
      }
      if (strncmp("mailqso", cmd, 7) == 0) {
        plogw->ostream->println("mailsqso command");
	//        mail_qso_log();
        break;
      }

      if (strncmp("status", cmd, 7) == 0) {
        // get status of the radios
        print_status_console();
        break;
      }

      if (strncmp("setstninfo", cmd, 10) == 0) {
        // set target to work station information
        set_target_station_info(cmd + 11);
        break;
      }

      if (strncmp("load", cmd, 4) == 0) {
        // load setting
        load_settings(cmd + 4);
        break;
      }
      if (strncmp("assign", cmd, 6) == 0) {
        // assign variables similarly to load/save file.
        if (!plogw->f_console_emu) {
          plogw->ostream->print("assign:");
          plogw->ostream->print(cmd + 7);
          plogw->ostream->println(":");
        }
        assign_settings(cmd + 7, settings_dict);
        break;
      }
      if (strcmp("post_assign", cmd) == 0) {
        // setting after assigning variables
        radio_list[0].enabled = plogw->radios_enabled & 1;
        radio_list[1].enabled = (plogw->radios_enabled >> 1) & 1;
        radio_list[2].enabled = (plogw->radios_enabled >> 2) & 1;
        set_rig_from_name(&radio_list[0]);
        set_rig_from_name(&radio_list[1]);
        set_rig_from_name(&radio_list[2]);
        set_contest_id();
        set_cluster();
        break;
      }
      if (strncmp("contest_id", cmd, 10) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 10, "%d", &tmp) == 1) {
	  if (tmp <0 || tmp >= N_CONTEST) {
	    plogw->ostream->println("contest_id out of range");
	    break;
	  }
	  plogw->contest_id =tmp;
	  set_contest_id();
	  plogw->ostream->print("contest:");
	  plogw->ostream->println(plogw->contest_name+2);
        } else {
          plogw->ostream->println("contest_id contest_id#");
        }
        break;
      }
      if (strcmp("show_multi",cmd)==0) {      
	print_multi_list();
	break;
      }
      if (strcmp(cmd,"disptype1")==0) {
	plogw->ostream->print("reset_display() type1");
	display_type=1;
	init_display();
	break;
      }
      if (strcmp(cmd,"disptype0")==0) {
	plogw->ostream->print("reset_display() type0");
	display_type=0;
	init_display();
	break;
      }
      if (strcmp(cmd,"disptype2")==0) {
	plogw->ostream->print("reset_display() type2");
	display_type=2;
	init_display();
	break;
      }
      if (strcmp(cmd,"muxtrans")==0) {
	f_mux_transport_cmd=1; // transition to mux
	sprintf(dp->lcdbuf,"mux_transport->mux\ncurrent=%d",f_mux_transport);
	upd_display_info_flash(dp->lcdbuf);
	break;
      }
      if (strcmp("show_bandmap",cmd)==0) {
	upd_display_bandmap();
	break;
      }
      if (strcmp("show_summary",cmd)==0) {
	show_summary();
	break;
      }	
      if (strcmp("callhist_enable",cmd)==0) {
        if (plogw->enable_callhist) {
          plogw->enable_callhist = 0;
        } else {
          plogw->enable_callhist = 1;
        }
        if (!plogw->f_console_emu) {
          plogw->ostream->print("callhist en =");
          plogw->ostream->println(plogw->enable_callhist);
        }
        if (plogw->enable_callhist) {
          if (!plogw->f_console_emu) plogw->ostream->println("open callhist");
          open_callhist();
        } else {
          if (!plogw->f_console_emu) plogw->ostream->println("close callhist");
          close_callhist();
        }
        sprintf(dp->lcdbuf, "callhist en:%d\nDone.\n", plogw->enable_callhist);
        if (!plogw->f_console_emu) plogw->ostream->println(dp->lcdbuf);
        break;
      }	
      if (strcmp("settings", cmd) == 0) {
        dump_settings(plogw->ostream, settings_dict);
        break;
      }
      if (strncmp("save", cmd, 4) == 0) {
        // release other settings  including sat
        release_memory();
        save_settings(cmd + 4);
        if (!plogw->f_console_emu) plogw->ostream->println("save");
        break;
      }
      if (strcmp("switch_bands",cmd)==0) {
	struct radio *radio;
	radio=so2r.radio_selected();
	switch_bands(radio);
	break;
      }
      if (strncmp("set_rig ",cmd,8) == 0) {
	struct radio *radio;
	radio=so2r.radio_selected();
	strcpy(radio->rig_name + 2, cmd+8);
	set_rig_from_name(radio);
	sprintf(dp->lcdbuf,"Rig set:%s",radio->rig_name+2);
	plogw->ostream->println(dp->lcdbuf);
	break;
      }
      if (strncmp("switch_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          switch_radios(tmp, -1);
        } else {
          plogw->ostream->println("switch_radio radio#");
        }
        break;
      }
      if (strncmp("enable_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          enable_radios(tmp, -1);
        } else {
          plogw->ostream->println("enable_radio radio#");
        }
        break;
      }
      if (strncmp("makedupe", cmd, 8) == 0 ) {
        init_score();
	//        init_multi();
	clear_multi_worked();
        init_dupechk();
        read_qso_log(READQSO_MAKEDUPE);
        break;
      }
      if (strncmp("focus", cmd, 5) == 0) {
        int new_focus;
        if (sscanf(cmd + 5, "%d", &new_focus) == 1) {
          so2r.change_focused_radio(new_focus);
        }
        break;
      }

      if (strcmp(cmd, "newqsolog") == 0) {
        // create new QSO log
        create_new_qso_log();
        break;
      }

      if (strcmp(cmd,"reset_settings")==0) {
	// remove files
	SD.remove("/settings.txt");
	SD.remove("/ch.txt");
	SD.remove("/wifiset.txt");
	SD.remove("/spiffs.bin");
	SD.remove("/rigs.txt");	
        plogw->ostream->println("reset_settings by removing files settings.txt ch.txt wifiset.txt");
	break;
      }
      if (strcmp(cmd,"restart_dvplogger")==0) {
        plogw->ostream->println("restarting DVPlogger by esp32 reset...");
	delay(1000);
	ESP.restart();
	break;
      }
      if (strcmp("adcstat",cmd)==0) {
	adc_statistics();
	break;
      }
      if (strncmp("time",cmd,4)==0) {
	// read/set time from RTC chip
	if (cmd[4]==' ') {
	  // set
	  set_rtcclock(cmd+5); // yymmddhhmmss to set 	  
	} else {
	  print_rtcclock();
	  plogw->ostream->println("set by time yyyy-mm-ddThh:mm:ss  . ");
	}
	break;
      }

      if (strncmp("callhist_open", cmd, 13) == 0) {
        if (cmd[13] == ' ') {
          set_callhistfn(cmd + 14);
        } else {
          set_callhistfn("");
        }
        open_callhist();
        break;
      }
      if (strcmp("callhist_search", cmd) == 0) {
        plogw->ostream->println("callhist_search command");
        cmd_interp_state = 2;
        break;
      }
      // other commands follow
      plogw->ostream->println("???");
      break;
    case 1:  // after callhist_set commsnd
      if (strcmp("end", cmd) == 0) {
        plogw->ostream->println("callhist_set end");
	close_callhistf();
	//        callhistf.close();
        callhistf_stat = 0;
        // open callhist
        open_callhist();
        cmd_interp_state = 0;
      } else {
        write_callhist(strtoupper(cmd));
      }
      break;
    case 2:  // call history search
      if (strcmp("end", cmd) == 0) {
        plogw->ostream->println("callhist_search end");
        cmd_interp_state = 0;
        break;
      }
      search_callhist(strtoupper(cmd));
      break;
  }
}
