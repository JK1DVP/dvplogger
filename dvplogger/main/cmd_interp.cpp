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
#include "callhist.h"
#include "callhist_remote.h"
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
#include "zserver.h"
#include "dac-adc.h"
#include "mcp_interface.h"
#include "morse_decoder_simple.h"
#include "mux_transport.h"
#include "usb_host.h"
int cmd_interp_state = 0;

static Stream *command_output = nullptr;

static Stream *current_command_output()
{
  return command_output ? command_output : console;
}

struct terminal_help_entry {
  const char *command;
  const char *description;
};

static const terminal_help_entry terminal_help_entries[] = {
  {"help", "show this command list"},
  {"emu", "enter terminal screen emulation; EXITEMU exits"},
  {"verbose[n]", "toggle verbose output, or set verbose bit mask n"},
  {"loadsat", "load saved satellite information"},
  {"savesat", "save satellite information"},
  {"satellite", "load/update TLE data"},
  {"nextaos", "calculate and display upcoming AOS"},
  {"decoder", "start CW decoder"},
  {"decoderstop", "stop CW decoder"},
  {"play <text>", "speech synthesis output"},
  {"playcw<text>", "send text using tone CW"},
  {"playwpm<n>", "set tone CW speed in WPM"},
  {"playq", "show current audio/CW playback queue"},
  {"newqsolog", "start a new QSO.TXT log"},
  {"zmerge [dry|repair]", "merge, compare, or repair duplicate QSOs"},
  {"makedupe", "rebuild dupe/multiplier data from QSO.TXT"},
  {"dumpqso[n]", "dump current raw QSO log, or backup log n"},
  {"readqso", "print QSO.TXT in importable text format"},
  {"dumpcur", "dump the currently selected QSO record"},
  {"dumptop", "dump the first QSO record"},
  {"dumpnext", "dump the next QSO record"},
  {"dumpprev", "dump the previous QSO record"},
  {"dumplast", "dump the last QSO record"},
  {"dump <n>", "dump QSO record number n"},
  {"listdir", "list files in the microSD root directory"},
  {"DX de ...", "inject a cluster spot line"},
  {"status", "show radio status"},
  {"setstninfo <call>", "set target station information"},
  {"switch_radio <n>", "switch radio n to the next rig definition"},
  {"enable_radio <n>", "enable/disable radio n"},
  {"focus <n>", "change the focused radio"},
  {"switch_bands", "switch selected radio to the next available band"},
  {"set_rig <name>", "set selected radio by rig definition name"},
  {"show_summary", "show QSO summary"},
  {"show_bandmap", "show bandmap"},
  {"show_multi", "show multiplier list"},
  {"contest_id <n>", "select contest definition number n"},
  {"save[name]", "save settings, optionally under name"},
  {"load[name]", "load settings, optionally from name"},
  {"settings", "show all current settings"},
  {"assign <name> <value>", "assign one setting value"},
  {"post_assign", "apply settings after assign operations"},
  {"callhist_enable", "toggle Call History search"},
  {"callhist_status", "show current Call History status"},
  {"callhist_open [file]", "set/open a Call History file"},
  {"callhist_set [file]", "receive Call History entries until 'end'"},
  {"callhist_search", "interactive Call History search; 'end' exits"},
  {"mem", "show Main CPU memory information"},
  {"memstat [watch|stop]", "show memory once or start/stop 1-second terminal watch"},
  {"submem", "alias of memstat"},
  {"addap <ssid> <password>", "add a Wi-Fi access point"},
  {"time [yyyy-mm-ddThh:mm:ss]", "show or set RTC time"},
  {"ntp_stat", "show NTP status"},
  {"disptype0", "select original 1.3-inch OLED"},
  {"disptype1", "select 2.4-inch OLED"},
  {"disptype2", "select mini 1.3-inch OLED"},
  {"reset_display", "reinitialize the display"},
  {"muxtrans", "request transition to MUXTRANS communication"},
  {"flashersd [boot part app spiffs]", "flash selected Sub CPU images from microSD"},
  {"flasher", "flash the minimum Sub CPU firmware"},
  {"subcpu_halt", "hold the Sub CPU in reset"},
  {"reset_settings", "remove saved settings files"},
  {"restart_dvplogger", "restart the Main CPU"},
  {"usb_desc", "show USB device descriptors"},
  {"serial", "show serial-port allocation"},
  {"send <text>", "send text directly to Serial2"},
  {"i2c_scan", "scan the I2C bus"},
  {"kbread", "show CardKB key codes; Fn+BS exits"},
  {"adcstat", "show ADC statistics"},
  {"gpio<n> <value>", "write MCP GPIO port n"},
  {"cp2105stat", "show CP2105 port status"},
  {"cp2105port0 / cp2105port1", "select CP2105 CAT port"},
  {"cp2105baud0 <baud>", "set CP2105 port 0 baud rate"},
  {"cp2105baud1 <baud>", "set CP2105 port 1 baud rate"},
  {"cp2105debug", "toggle CP2105 TX/RX debug dump"},
  {"cp2105send0 <text>", "send raw text through CP2105 port 0"},
  {"cp2105send1 <text>", "send raw text through CP2105 port 1"},
  {"ANT0<n> / ANT1<n>", "set antenna relay 0/1 state"},
  {"ANTALT[n]", "enable antenna alternation after n receptions"},
  {"SIGNAL", "toggle periodic signal/antenna/azimuth display"},
  {"ROT...", "rotator commands: EN, TYPE, NORTH, SOUTH, TR, AZ, STEP, SWEEP"}
};

static void print_terminal_help(Print *out)
{
  out->println("Available Commands:");
  const size_t n = sizeof(terminal_help_entries) / sizeof(terminal_help_entries[0]);
  for (size_t i = 0; i < n; ++i) {
    out->print(terminal_help_entries[i].command);
    out->print(" - ");
    out->println(terminal_help_entries[i].description);
  }
}
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
      current_command_output()->print("ptt on and ");
      mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
      current_command_output()->print("sent ");current_command_output()->println(buf);
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
    current_command_output()->print("sent ");current_command_output()->println(buf);
  }
}

void play_wpm_cmd(char *cmd)
{
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"playw%s",cmd);
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    current_command_output()->print("sent ");current_command_output()->println(buf);
  }
  
}

void play_queue_cmd()
{
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"playq");
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    current_command_output()->print("sent ");current_command_output()->println(buf);	  
  }
}

void play_stop_cmd() {
  if (f_mux_transport) {
    char buf[20];
    sprintf(buf,"plays");
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    current_command_output()->print("sent ");current_command_output()->println(buf);	  
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
      current_command_output()->print("ptt on and ");
      mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)buf,strlen(buf));
      current_command_output()->print("sent ");current_command_output()->println(buf);
      so2r.set_queue_monitor_status(1);
    }
  }
}

namespace {
class CommandOutputScope {
public:
  explicit CommandOutputScope(Stream *output)
      : saved_(command_output) {
    command_output = output ? output : console;
  }

  ~CommandOutputScope() {
    command_output = saved_;
  }

  Stream *get() const { return current_command_output(); }

private:
  Stream *saved_;
};
}  // namespace

void cmd_interp(char *cmd, Stream *output) {
  CommandOutputScope output_scope(output);
  Stream *out = output_scope.get();
  int tmp, tmp1;
  struct radio *radio;
  switch (cmd_interp_state) {
    case 0:  // command line
      out->print("cmd:");
      out->println(cmd);

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
	out->print("stopped morse decoder");
	break;
      }
      if (strcmp(cmd,"decoder")==0) {
	decoder.start_i2s_adc_24k_rms_task();// start morse decoder
	out->print("started morse decoder");
	break;
      }

      if (strcmp(cmd, "cp2105debug") == 0) {
	CP2105toggleDebug();
	break;
      }
      if (strncmp(cmd, "cp2105send0 ", 12) == 0) {
	CP2105sendRaw(0, cmd + 12);
	return;
      }

      if (strncmp(cmd, "cp2105send1 ", 12) == 0) {
	CP2105sendRaw(1, cmd + 12);
	return;
      }      
      
      if (strcmp(cmd, "emu") == 0) {
        // enter into console terminal emulation mode
        plogw->f_console_emu = 1;
	// send clear screen command
	char buf[40];	
	for (int i=0;i<20;i++) {
	  sprintf(buf,"\033[%d;1H\033[K",i);
	  out->print(buf);
	}
        break;
      }
      
      if (strncmp(cmd,"play ",5)==0) {
	// play_string
	out->println("play command");
	play_string_cmd(cmd+5);
	break;
      }
      if (strncmp(cmd,"playcw",6)==0) {
	// play_string
	out->println("playcw command");
	play_cw_cmd(cmd+6);
	break;
      }
      if (strncmp(cmd,"playwpm",7)==0) {
	// play queue query
	out->println("playwpm command");
	play_wpm_cmd(cmd+7);
	break;
      }
      if (strncmp(cmd,"playq",5)==0) {
	// play queue query
	out->println("playq command");
	play_queue_cmd();
	break;
      }
      if (strcmp(cmd,"serial")==0) {
	// status of serial port allocation
	print_serial_instance(out);
	break;
      }
      if (strncmp(cmd, "send ", 5) == 0) {
	//        SO2Rprint(cmd + 5);
	Serial2.println(cmd+5);
	out->print("sent Serial2:");out->println(cmd+5);
        break;
      }
      if (strcmp(cmd,"i2c_scan")==0) {
	// scan i2c bus and print result
	i2c_scan(out);
	break;
      }
      if (strcmp(cmd,"usb_desc")==0) {
	USB_desc();
	break;
      }
      if (strcmp(cmd,"cp2105stat")==0) {
	CP2105status(out);
	break;
      }
      if (strcmp(cmd,"cp2105port0")==0 || strcmp(cmd,"cp2105port1")==0) {
	CP2105selectPort(cmd[strlen(cmd)-1]-'0');
	break;
      }
      if (strncmp(cmd,"cp2105baud0 ",12)==0 || strncmp(cmd,"cp2105baud1 ",12)==0) {
	uint8_t port = cmd[10]-'0';
	uint32_t baud = strtoul(cmd+12, NULL, 10);
	if (!CP2105setBaud(port, baud)) out->println("CP2105 baud setting failed");
	break;
      }

      if (strcmp(cmd,"kbread")==0) {
	while(1) {
	  Wire.requestFrom(0x5F, 1);
	  while(Wire.available())	  {
	    char c = Wire.read(); // receive a byte as characterif
	    if (c != 0) {
	      if (c==0x8b) { // Fn+BS exit from kbread
		out->println("exit from kbread");
		goto end_kb;
	      }
	      out->print("cardkb:");
	      if (isprint(c)) {
		out->print(c);
		out->print(":");		
	      } else {
		out->print(":");
	      }
	      out->println(c, HEX);
	    }
	  }
	  delay(10);
	}
      end_kb:
	break;
      }

      if (strcmp(cmd,"ntp_stat")==0) {
	print_ntpstatus(out);
	break;
      }

      if (strcmp(cmd,"reset_display")==0) {
	  out->print("init_display()");
	  init_display();
	  break;
      }
      if (strncmp(cmd,"flashersd",9)==0) {
	out->println("flashersd boot part app spiffs (put what you want to flash).");
	// stop mux serial port
	deinit_mux_serial();
	// stop using SD card
	close_qsolog();
	out->println("esp_flashersd() ... ");		
	//      	esp_flasher();
	esp_flasher_sd(cmd+9);
	out->println("esp_flashersd() end... ");		
	// restore mux serial port
	init_mux_serial();
	attach_interrupt_civ();
      	break;
      }

      if (strcmp(cmd,"memstat watch")==0) {
        start_memstat_watch(out);
        break;
      }
      if (strcmp(cmd,"memstat stop")==0) {
        stop_memstat_watch(out);
        break;
      }
      if (strcmp(cmd,"submem")==0 || strcmp(cmd,"memstat")==0) {
        request_memstat_main_subcpu(true, out);
        break;
      }
      if (strcmp(cmd,"subcpu_halt")==0) {
	// keep subcpu reset pin low to halt subcpu
	//const uint8_t reset_trigger_mcp_pin = 15;
	mcp_write_pin(15, 0);
	out->println("halted subcpu by keep en pin low.");
	break;
      }
      
      if (strcmp(cmd,"flasher")==0) {
	// stop mux serial port
	deinit_mux_serial();
	close_qsolog();
	out->println("esp_flasher() ... ");
	esp_flasher();
	out->println("esp_flasher() end... ");	
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
        out->printf("verbose=%d\n", verbose);
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
	out->println("addap command");	
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

          out->print("write mcp gpio port ");
          out->print(tmp);
          out->print(" value ");
          out->println(tmp1);
          for (int i = 0; i < 16; i++) {
            out->print(i);
            out->print(" ");
            out->println(read_mcp_gpio(i));
          }
        } else {
          out->println("gpio mcp param error");
        }
        break;
      }



      if (strcmp(cmd, "zmerge") == 0) {
        zserver_start_merge(false);
        break;
      }
      if (strcmp(cmd, "zmerge dry") == 0) {
        zserver_start_merge(true);
        break;
      }
      if (strcmp(cmd, "zmerge repair") == 0) {
        zserver_start_repair();
        break;
      }

      if (strcmp(cmd, "listdir") == 0) {
        listDir(SD, "/", 0, out);
        break;
      }
      if (strcmp(cmd, "help") == 0) {
        print_terminal_help(out);
        break;
      }
      if (strncmp("callhist_set", cmd, 12) == 0) {
        out->println("callhist_set command");
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
	dump_qso_current(out);
	break;
      }
      if (strcmp("dumptop",cmd)==0) {
	info_disp.pos=0;
	dump_qso_current(out);
	break;
      }
      if (strcmp("dumpnext",cmd)==0) {
	info_disp.pos+=sizeof(qso.all);
	dump_qso_current(out);
	break;
      }
      if (strcmp("dumpprev",cmd)==0) {
	info_disp.pos-=sizeof(qso.all);
	if (info_disp.pos<0) info_disp.pos=0;
	dump_qso_current(out);
	break;
      }
      if (strcmp("dumplast", cmd) == 0) {      
	unsigned long pos = qsologf.position();
	info_disp.pos = pos - sizeof(qso.all);
	if (info_disp.pos < 0) {
	  info_disp.pos = 0;
	}
	dump_qso_current(out);
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
	    dump_qso_current(out);
	  } else {
	    out->print(tmp);
	    out->println(", beyond range <");
	    out->print(pos/sizeof(qso.all));
	  }
	} else {
	    out->println("dump qso#(0-)");
	}
	break;
      }
      
      if (strncmp("dumpqso", cmd, 7) == 0) {
        out->println("dumpqso command");
        out->println(cmd + 8);
	if (strlen(cmd)>7) {
	  dump_qso_bak(cmd + 8, out);
	} else {
	  out->println("dumping current qso log.");	  
	  dump_qso_log(out);
	}
        break;
      }

      
      if (strncmp("readqso", cmd, 7) == 0) {
        out->println("readqso command");
        read_qso_log(READQSO_PRINT, out);
        break;
      }
      if (strcasecmp(cmd, "listqsofile") == 0) {
        list_qso_backup_files();
        break;
      }
      if (strncasecmp(cmd, "switchlog", 9) == 0) {
        const char *arg = cmd + 9;
        while (*arg == ' ') arg++;
        char *endp = NULL;
        long n = strtol(arg, &endp, 10);
        if (arg == endp || *endp != '\0' || n < 0 || n > 999) {
          out->println("Usage: SWITCHLOGnnn");
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                   "Usage:\nSWITCHLOGnnn");
          upd_display_info_flash(dp->lcdbuf);
        } else {
          switch_qso_log((int)n);
        }
        break;
      }
      if (strncmp("mailqso", cmd, 7) == 0) {
        out->println("mailsqso command");
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
          out->print("assign:");
          out->print(cmd + 7);
          out->println(":");
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
	    out->println("contest_id out of range");
	    break;
	  }
	  plogw->contest_id =tmp;
	  set_contest_id();
	  out->print("contest:");
	  out->println(plogw->contest_name+2);
        } else {
          out->println("contest_id contest_id#");
        }
        break;
      }
      if (strcmp("show_multi",cmd)==0) {      
	print_multi_list(out);
	break;
      }
      if (strcmp(cmd,"disptype1")==0) {
	out->print("reset_display() type1");
	display_type=1;
	init_display();
	break;
      }
      if (strcmp(cmd,"disptype0")==0) {
	out->print("reset_display() type0");
	display_type=0;
	init_display();
	break;
      }
      if (strcmp(cmd,"disptype2")==0) {
	out->print("reset_display() type2");
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
	show_summary(out);
	break;
      }	
      if (strcmp("callhist_enable",cmd)==0) {
        if (plogw->enable_callhist) {
          plogw->enable_callhist = 0;
        } else {
          plogw->enable_callhist = 1;
        }
        if (!plogw->f_console_emu) {
          out->print("callhist en =");
          out->println(plogw->enable_callhist);
        }
        if (plogw->enable_callhist) {
          if (!plogw->f_console_emu) out->println("open callhist");
          int n = 0;
          if (callhist_at == 1) {
            if (load_callhist_subcpu(callhistfn)) {
              n = get_callhist_subcpu_count();
            }
          } else {
            n = read_callhist_list(callhistfn);
          }
          out->printf("callhist at=%s entries=%d\n",
                                 callhist_at ? "SUBCPU" : "MAIN", n);
        } else {
          if (!plogw->f_console_emu) out->println("close callhist");
          close_callhist();
        }
        sprintf(dp->lcdbuf, "callhist en:%d\nDone.\n", plogw->enable_callhist);
        if (!plogw->f_console_emu) out->println(dp->lcdbuf);
        break;
      }	
      if (strcmp("settings", cmd) == 0) {
        dump_settings(out, settings_dict);
        break;
      }
      if (strncmp("save", cmd, 4) == 0) {
        // release other settings  including sat
        release_memory();
        save_settings(cmd + 4);
        if (!plogw->f_console_emu) out->println("save");
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
	out->println(dp->lcdbuf);
	break;
      }
      if (strncmp("switch_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          switch_radios(tmp, -1);
        } else {
          out->println("switch_radio radio#");
        }
        break;
      }
      if (strncmp("enable_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          enable_radios(tmp, -1);
        } else {
          out->println("enable_radio radio#");
        }
        break;
      }
      if (strncmp("makedupe", cmd, 8) == 0 ) {
        init_score();
	//        init_multi();
	clear_multi_worked();
	//        init_dupechk(NMAXQSO,0);
	init_dupechk_maincpu();
	reset_dupechk_subcpu();
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
        out->println("reset_settings by removing files settings.txt ch.txt wifiset.txt");
	break;
      }
      if (strcmp(cmd,"restart_dvplogger")==0) {
        out->println("restarting DVPlogger by esp32 reset...");
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
	  out->println("set by time yyyy-mm-ddThh:mm:ss  . ");
	}
	break;
      }

      if (strcmp("callhist_status", cmd) == 0) {
        show_callhist_status();
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
        out->println("callhist_search command");
        cmd_interp_state = 2;
        break;
      }
      // other commands follow
      out->println("???");
      break;
    case 1:  // after callhist_set commsnd
      if (strcmp("end", cmd) == 0) {
        out->println("callhist_set end");
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
        out->println("callhist_search end");
        cmd_interp_state = 0;
        break;
      }
      search_callhist(strtoupper(cmd));
      break;
  }
}


void cmd_interp(char *cmd) {
  cmd_interp(cmd, plogw->ostream ? plogw->ostream : console);
}
