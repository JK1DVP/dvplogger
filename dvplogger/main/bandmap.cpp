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
#include "main.h"
#include "display.h"
#include "dupechk.h"
#include "log.h"
#include "cat.h"
#include "ui.h"
#include "bandmap.h"
#include "tcp_server.h"
#include "dupechk.h"
#include "multi_process.h"
#include "misc.h"
#include "timekeep.h"
#include "so2r.h"


// now bandmap[N_BAND] is bandmap entry list for all valid band sorted by time of arrival 25/8/16
// will always sort by time and the last item will be deleted 

void print_bandmap_mask()
{
  sprintf(dp->lcdbuf, "BandMap Enable\n");
  print_bin(dp->lcdbuf+strlen(dp->lcdbuf),reverse_bits(~bandmap_mask,N_BAND-1),N_BAND-1);
  strcat(dp->lcdbuf," En\n1371225141251\n");
  strcat(dp->lcdbuf,     ".. 418043...0\n");
  strcat(dp->lcdbuf,     "85     40246G\n");
  
  //  strcat(dp->lcdbuf," En\n1521415221731\n");
  //  strcat(dp->lcdbuf,     "0642340814 58\n");
  //  strcat(dp->lcdbuf,     "G   00       \n");
  upd_display_info_flash(dp->lcdbuf);
  
}

int compare_bandmap_entry(const void *a, const void *b) {
  struct bandmap_entry *a1, *b1;
  a1 = (struct bandmap_entry *)a;
  b1 = (struct bandmap_entry *)b;
  int dt;
  // check type
  // check if either side is empty (lead to return to last)
  if (a1->station[0] == '\0') return 10000000;   // big number
  if (b1->station[0] == '\0') return -10000000;  // big negative number
  switch (bandmap_disp.sort_type) {
    case 0:  // time
      dt=abs(b1->time - a1->time);
      if (dt<60) {
	// small time difference then check new multi
	if (b1->flag&BANDMAP_ENTRY_FLAG_NEWMULTI) {
	  if (a1->flag&BANDMAP_ENTRY_FLAG_NEWMULTI) {
	    return (b1->time - a1->time); // both newmulti
	  } else {
	    return -1;
	  }
	} else {
	  if (a1->flag&BANDMAP_ENTRY_FLAG_NEWMULTI) {
	    return 1;
	  } else {
	    return (b1->time - a1->time); // both not new multi
	  }
	}
      } else {
	return (b1->time - a1->time);
      }
      break;
    case 1:  // freq
      return (a1->freq - b1->freq);
      break;
    default:
      break;
  }
  return 0;
}

// sort the bandmap with the type specified
void sort_bandmap(int bandid) {
  int idx;
  if (bandid <= 0) return;
  //  if (bandid >= N_BAND) return; // > ? as max band number is bandid
  if (bandid > N_BAND) return; // > ? as max band number is bandid   
  idx = bandid - 1; // index is bandid -1 
  // set sort type
  //bandmap_disp.sort_type = type;
  // and sort
  if (verbose & 1) {
    plogw->ostream->print("sorting bandmap for band ");
    plogw->ostream->println(bandid);
  }
  qsort(bandmap[idx].entry, bandmap[idx].nentry, sizeof(struct bandmap_entry), compare_bandmap_entry);
  if (verbose & 1) plogw->ostream->println("sorted bandmap");
}

int find_on_freq_bandmap(int bandid, unsigned int freq, int tolerance) {
  // find entry on the frequency and return with index
  int idx;
  if (bandid <= 0) return -1;
  if (bandid >= N_BAND) return -1;

  idx = bandid - 1;
  int i;
  int dfreq;
  int ret;
  struct bandmap_entry *p;
  ret = -1;
  for (i = 0; i < bandmap[idx].nentry; i++) {
    p = bandmap[idx].entry + i;
    if (*p->station == '\0') continue;
    // check myself
    if (strcasecmp(plogw->my_callsign+2, p->station)==0) {
      // myself
      continue;
    }
    
    dfreq = p->freq - freq;
    if (dfreq < 0) dfreq = -dfreq;
    if (dfreq < tolerance) {
      p->flag |= BANDMAP_ENTRY_FLAG_ONFREQ;
      if (ret == -1) ret = i;
      //		return i; // return with the first entry that satisfy criteria
    } else {
      p->flag &= ~BANDMAP_ENTRY_FLAG_ONFREQ;
    }
  }
  return ret;
}

// verbose 16 bandmap debugging
void delete_old_entry(int bandid, int life)
// delete old (> life mins) entry of the bandmap
{

  if (bandid <= 0) return;
  //  if (bandid >= N_BAND) return;
  if (bandid > N_BAND) return;
  int idx;
  idx = bandid - 1;
  int i;
  int dt;
  int t0;  // current unixtime
  //  t0 = rtctime.unixtime();
  t0=my_rtc.unixtime();
  struct bandmap_entry *entry;
  int count;
  count=0;
  if (verbose & 16) {
    // show
    plogw->ostream->print("delete_old entry(): band ");
    plogw->ostream->print(bandid);
    plogw->ostream->print(" ");    
    plogw->ostream->print(bandid_str[bandid-1]);    
    plogw->ostream->print(" nentry ");
    plogw->ostream->println(bandmap[idx].nentry);
  }
  for (i = 0; i < bandmap[idx].nentry; i++) {
    entry = bandmap[idx].entry + i;
    if (*entry->station == '\0') continue;
    dt = t0 - entry->time;
    if (dt > 60 * life) {
      // old entry
      if (verbose & 16) {
        plogw->ostream->print((String)entry->station);
        plogw->ostream->print(" dt=");
        plogw->ostream->print(dt);	
        plogw->ostream->println("... delete.");
      }
      *entry->station = '\0';  // deleted
    }
    count++;
  }
  if (verbose & 16) {
    plogw->ostream->print(" delete end. entries=");
    plogw->ostream->println(count);
  }
}


void delete_on_freq_entry_bandmap()
{
  plogw->ostream->println("delete_on_freq_entry_bandmap() called");
  struct radio *radio;
  radio = so2r.radio_selected();
  if (radio->bandid_bandmap <= 0) return;
  if (radio->bandid_bandmap >= N_BAND) return;
  int idx;
  idx = radio->bandid_bandmap - 1;
  if (*bandmap_disp.on_freq_station=='\0') {
    plogw->ostream->println("No on_freq_station now.");    
    return;
    // no on freq station
  }
  int i;
  struct bandmap_entry *entry;
  for (i = 0; i < bandmap[idx].nentry; i++) {
    entry = bandmap[idx].entry + i;
    if (strcmp(entry->station, bandmap_disp.on_freq_station) == 0) {
      *entry->station = '\0';  // delete this entry
      //      if (verbose & 1) {
      plogw->ostream->print((String)entry->station);
      plogw->ostream->println(" deleted (on_freq).");
      //      }
      break;
    }
  }
}

void delete_entry_bandmap() {
  struct radio *radio;
  radio = so2r.radio_selected();
  // delete current cursor entry in the bandmap
  if (*bandmap_disp.on_cursor_station == '\0') return;
  if (radio->bandid_bandmap <= 0) return;
  if (radio->bandid_bandmap >= N_BAND) return;
  int idx;
  idx = radio->bandid_bandmap - 1;
  int i;
  struct bandmap_entry *entry;
  for (i = 0; i < bandmap[idx].nentry; i++) {
    entry = bandmap[idx].entry + i;
    if (strcmp(entry->station, bandmap_disp.on_cursor_station) == 0) {
      *entry->station = '\0';  // delete this entry
      if (verbose & 1) {
        plogw->ostream->print((String)entry->station);
        plogw->ostream->println(" delete.");
      }
      break;
    }
  }
}

void send_mode_set_civ(const char *opmode, int filnr);


void set_current_rig_priority() {
  struct radio *radio;
  radio = so2r.radio_selected();
  int tmp;
  tmp = (1 << (radio->bandid - 1));  // band_mask bit
  // reset priority flag to all rigs
  for (int i = 0; i < 3; i++) {
    radio_list[i].band_mask_priority &= ~tmp;
  }
  // set current radio (focused_radio) a priority
  so2r.radio_selected()->band_mask_priority |= tmp;
  plogw->ostream->print("set focused radio ");
  plogw->ostream->print(so2r.focused_radio());
  plogw->ostream->println(" priority");

  radio = so2r.radio_selected();
  sprintf(dp->lcdbuf,"BAND En/Prio R%1d\n",radio->rig_idx);
  //  sprintf(dp->lcdbuf, "band_mask=%02X\npriority=%02X\n", radio->band_mask, radio->band_mask_priority);
  print_bin(dp->lcdbuf+strlen(dp->lcdbuf),~radio->band_mask,N_BAND-1);
  strcat(dp->lcdbuf," En\n");
  print_bin(dp->lcdbuf+strlen(dp->lcdbuf),radio->band_mask_priority,N_BAND-1);
  strcat(dp->lcdbuf," Prio\n1521415221731\n");
  strcat(dp->lcdbuf,  "0642340814 58\n");
  strcat(dp->lcdbuf,  "G   00       \n");
  upd_display_info_flash(dp->lcdbuf);
}


// choose appropriate radio for the bandid
// return -1 not found any radio else chosen target radio

int find_appropriate_radio(int bandid) {
  struct radio *radio;
  int i ;
  i = so2r.focused_radio();
  radio = &radio_list[i];
  // first check current selected radio
  if (((1 << (bandid - 1)) & radio->band_mask) == 0) {
    // no problem keep the radio
    return i;
  }
  console->println("find_appropriate_radio(): check current radio band_mask OK");
  // seek radios and find the priotized radio
  int tmp1;
  tmp1 = -1;
  for ( i = 2; i >= 0; i--) {
    if (!radio_list[i].enabled) continue;
    int tmp_mask;
    tmp_mask = (1 << (bandid - 1));
    radio = &radio_list[i];
    if ((tmp_mask & radio->band_mask) == 0) {
      // consider this radio
      if ((tmp_mask & radio->band_mask_priority) == 1) {
        // this is prioritized radio
        return i;
      } else {
        tmp1 = i;
      }
    }
  }
  return tmp1;
}


int select_appropriate_radio(int bandid) {
  int i ;
  i = find_appropriate_radio(bandid);
  if (i >= 0) {
    so2r.change_focused_radio(i);
    return 1;
  } else {
    return 0;
  }
}


// set_mode()  opmode string , byte filt to set to the specified radio
// set_frequency()
// set_frequency_rig() --> not able to select radio so new se_frequency_rig_radio() to define
// these are data base updating routine from information from the radio
// so to change freq, mode externally needs sending rig command by set_frequency_rig_radio() and send_mode_set_civ()
// also radio->filtbank need to be referred.

// freq -> bandid calc (done in set_frequency()
// mode -> modetype calc (done in set_mode)
// cq ?    force s&p (done in set_frequency)
// filt -> after set_frequency() and set_mode() then refer to radio->filtbank to set filt set_frequency_rig_radio() and send_mode_set_civ()

int set_station_entry(struct radio *radio, char *station, unsigned int freq, const char *opmode)
{
  if (!plogw->f_console_emu) {
  plogw->ostream->print("set_station_entry():");
  plogw->ostream->print(station);
  plogw->ostream->print(" ");
  plogw->ostream->print(freq);
  plogw->ostream->print(" rig_idx ");
  plogw->ostream->print(radio->rig_idx);
  plogw->ostream->print(" opmode ");  
  plogw->ostream->println(opmode);
  
  }
  int modenum;
  if (!plogw->f_console_emu) {    
  plogw->ostream->print("set_station_entry opmode:");
  plogw->ostream->print(opmode);
  }

  modenum = rig_modenum(opmode);
  if (!plogw->f_console_emu) {    
    plogw->ostream->print("modenum ");
    plogw->ostream->println(modenum);
  }
  if (modenum < 0) {
  if (!plogw->f_console_emu) {      
    plogw->ostream->println("invalid opmode");
  }
    return 0;
  }
  const char *opmode_str ;
  opmode_str = opmode_string(modenum);
  if (!plogw->f_console_emu) {    
  plogw->ostream->print("modenum:");
  plogw->ostream->print(modenum);
  plogw->ostream->print(" opmode_str:");
  plogw->ostream->println(opmode_str);
  }

  strncpy(radio->callsign + 2, station, 10);


  //set_frequency(freq,radio);  // this should be civ process not here ? 23/4/23
  // set_mode_nonfil(opmode_str, radio); // here also

  // radio->modetype need to be changed here

  int modetype = modetype_string(opmode_str);
  // and force S&P for the modetype
  radio->cq[modetype] = LOG_SandP;
  // and temporal bandid
  int bandid = freq2bandid(freq);

  // then check filt for the target modetype and band
  int filt;
  filt = radio->filtbank[bandid][radio->cq[modetype]][modetype];
  if (!plogw->f_console_emu) {    
    plogw->ostream->println("filt="); plogw->ostream->println(filt);
  }
  // then set these target values to the radio
  set_frequency_rig(freq);
  send_mode_set_civ(opmode_str, filt);

  upd_display();

  return 1;
}

void pick_entry_bandmap() {
  struct radio *radio;
  radio = so2r.radio_selected();
  console->print("pick_entry_bandmap() radio_selected ");
  console->println(radio->rig_idx);
  
  if (*bandmap_disp.on_cursor_station == '\0') return;
  // check if selected_radio do not have bandmask bit set
  int tmp;
  tmp = freq2bandid(bandmap_disp.on_cursor_freq);
  if (!select_appropriate_radio(tmp)) {
    if (!plogw->f_console_emu) {      
      plogw->ostream->print("no appropriate rig for bandid");
      plogw->ostream->println(tmp);
    }
    return;
  }
  radio = so2r.radio_selected();
  if (!plogw->f_console_emu) {    
    console->println("pick_entry_bandmap()");
    console->println(bandmap_disp.on_cursor_station);
    console->println(bandmap_disp.on_cursor_freq);
    console->println(bandmap_disp.on_cursor_modeid);
    console->println(mode_str[bandmap_disp.on_cursor_modeid]);
  }
  set_station_entry(radio, bandmap_disp.on_cursor_station, bandmap_disp.on_cursor_freq, mode_str[bandmap_disp.on_cursor_modeid]);

}

// bandmap でオンフレのcallsign を、取り込み。
void pick_onfreq_station() {
  struct radio *radio;
  radio = so2r.radio_selected();
  if (bandmap_disp.f_onfreq) {  // onfreq station exists
    // pick up onfreq station
    strncpy(radio->callsign + 2, bandmap_disp.on_freq_station, 10);
  }
}


void init_bandmap_entry(struct bandmap_entry *p) {
  p->station[0] = '\0';
  p->mode = 0;
  p->remarks[0] = '\0';
  p->time = 0;
  p->type = 0;
  p->freq = 0;
  p->flag = 0;
}

void init_bandmap() {
  for (int i = 0; i <= N_BAND; i++) { // include bandmap[N_BAND] new entries for all band
    bandmap[i].bandid = i + 1;
    bandmap[i].nstations = 0;
    bandmap[i].nentry = 0;
    bandmap[i].entry = NULL;
    
    bandmap_disp.cursor[i] = 0;
    bandmap_disp.top_column[i] = 0;
  }

  bandmap_disp.f_update = 0;
}

int search_bandmap(int bandid, char *stn, int md) {
  int idx;
  idx = bandid - 1;
  if (bandid < 0) return -1;
  //  if (bandid >= N_BAND) return -1;
  if (bandid > N_BAND) return -1;  
  int found;
  int i;
  found = 0;
  for (i = 0; i < bandmap[idx].nentry; i++) {
    //   if (bandmap[idx].entry[i].station[0] == '\0') break;
    if (bandmap[idx].entry[i].station[0] == '\0') continue;  // need to continue to search all entries? 22/7/24
    if (strcmp(bandmap[idx].entry[i].station, stn) == 0) {
      // matched station
      found = 1;
      break;
    }
  }
  if (found) return i;
  else return -1;
}


// search bandmap of allband
int search_bandmap_allband(int bandid, char *stn, int md) {

  return -1;
  int idx=N_BAND-1;
  //  idx = bandid - 1;
  
  //  if (bandid < 0) return -1;
  //  if (bandid >= N_BAND) return -1;
  //  if (bandid > N_BAND) return -1;  
  int found;
  int idx1;
  int i;
  found = 0;
  for (i = 0; i < bandmap[idx].nentry; i++) {
    //   if (bandmap[idx].entry[i].station[0] == '\0') break;
    if (bandmap[idx].entry[i].station[0] == '\0') continue;  // need to continue to search all entries? 22/7/24
    if (strcmp(bandmap[idx].entry[i].station, stn) == 0) {
      // matched station
      // check bandid for the frequency if matches
      idx1=freq2bandid(bandmap[idx].entry[i].freq);
      if (verbose &4) {
	console->print("bandmap entry freq=");console->print(bandmap[idx].entry[i].freq);
	console->print(" bandid=");console->println(idx1);
      }
      if (idx1==bandid) {
	found = 1;
	break;
      }
    }
  }
  if (found) return i;
  else return -1;
}



int new_entry_bandmap(int bandid,int nmax) {
  int idx;
  idx = bandid - 1;
  if (bandid < 0) return -1;
  //  if (bandid >= N_BAND) return -1;
  if (bandid > N_BAND) return -1;  
  int i;
  // search released entry
  console->print("new_entry_bandmap() band idx=");
  console->println(idx);
  for (i = 0; i < bandmap[idx].nentry; i++) {
    if (bandmap[idx].entry[i].station[0] == '\0') {
      // found
      if (verbose & 16) {
        plogw->ostream->print("bandmap entry found:");
        plogw->ostream->println(i);
      }
      return i;
    }
  }
  // not found
  // allocate by expanding size
  int nentry_prev;
  nentry_prev = bandmap[idx].nentry;
  if (verbose & 16) {
    plogw->ostream->print("nentry_prev:");
    plogw->ostream->println(nentry_prev);
  }
  
  if (bandmap[idx].nentry + 50 > nmax) {
    // exceed allowed number of entries
    return -1;  // unsuccessful allocation
  }
    
  bandmap[idx].nentry += 50;
  // bandmap[idx].nentry += 5; // make it smaller increment to debug
  struct bandmap_entry *p;
  //  p = (struct bandmap_entry *)realloc(bandmap[idx].entry, sizeof(struct bandmap_entry) * bandmap[idx].nentry); // original
#ifdef PSRAM_EXISTS
  //  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)>sizeof(struct bandmap_entry) * bandmap[idx].nentry ) {
  p = (struct bandmap_entry *)heap_caps_realloc(bandmap[idx].entry, sizeof(struct bandmap_entry) * bandmap[idx].nentry,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // try to allocate from heap
  //  p = (struct bandmap_entry *)realloc(bandmap[idx].entry, sizeof(struct bandmap_entry) * bandmap[idx].nentry); // original    
  //  } else {
#else
  p = (struct bandmap_entry *)realloc(bandmap[idx].entry, sizeof(struct bandmap_entry) * bandmap[idx].nentry); // original    
  //  }
#endif
  if (p != NULL) {
    // success reallocation
    if (verbose & 16) plogw->ostream->println(" realloc ok ");
    bandmap[idx].entry = p;
    for (int i = nentry_prev; i < bandmap[idx].nentry; i++) {
      // initialize reallocated portion
      init_bandmap_entry(bandmap[idx].entry + i);
    }
    return nentry_prev;
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("realloc failed");
    return -1;  // unsuccessful allocation
  }
}


void set_info_bandmap(int bandid, char *stn, int modeid, unsigned int ifreq, char *remarks)
// mode code is for rig mode (refered to set rig mode)
{
  struct bandmap_entry *entry;
  /*  struct bandmap_entry *entry_allband; // entry for all band bandmap*/
  if (!plogw->f_console_emu) {
    plogw->ostream->println("");
    plogw->ostream->print("Set info bandmap band ");
    plogw->ostream->print(bandid);
    plogw->ostream->print(" stn ");
    plogw->ostream->print((String)stn);
    plogw->ostream->print(" mode ");
    plogw->ostream->print(modeid);
    plogw->ostream->print(" freq ");
    plogw->ostream->print(ifreq);
    plogw->ostream->print(" remarks :");
    plogw->ostream->print((String)remarks);
    plogw->ostream->println(":");
  }
  if ((bandid <= 0) || (bandid >= N_BAND)) {
    if (!plogw->f_console_emu) plogw->ostream->println("illegal bandid");
    return;
  }

  //////////////////////////////////////
  int idx;
  /*  int idx_allband; // bandmap of all band*/
  // find new entry point  for the station
  idx = search_bandmap(bandid, stn, modeid);
  /*  idx_allband =search_bandmap_allband(bandid, stn, modeid);*/

  if (!plogw->f_console_emu) {
    if (verbose & 16) {
      plogw->ostream->print("set_info_bandmap () searched idx ");
      plogw->ostream->print(idx);
    }
  }

  if (idx != -1) {
    // found existing entry
    // replace the entry with current one
    if (verbose & 16) {
      plogw->ostream->print("found existing entry idx=");
      plogw->ostream->println(idx);
    }
    entry = bandmap[bandid - 1].entry + idx;
  } else {
    // new entry
    idx = new_entry_bandmap(bandid,200);  // return entry which is not used 
    if (verbose & 16) {
      plogw->ostream->print("new entry idx=");
      plogw->ostream->println(idx);
    }
    if (idx == -1) {
      if (!plogw->f_console_emu) {
        plogw->ostream->print("no new entry available, ignore this;");
        plogw->ostream->println(stn);
      }
      return;
      
    }
    entry = bandmap[bandid - 1].entry + idx;
  }

  /*  entry_allband=NULL;
  if (idx_allband != -1) {
    // found existing entry
    // replace the entry with current one
    if (verbose & 16) {
      plogw->ostream->print("found existing entry idx allband=");
      plogw->ostream->println(idx_allband);
    }
    entry_allband = bandmap[N_BAND].entry + idx_allband;
  } else {
    // new entry
    idx_allband = new_entry_bandmap(N_BAND+1,200);  // return entry which is not used
    if (verbose & 16) {
      plogw->ostream->print("new entry allband idx=");
      plogw->ostream->println(idx_allband);
    }
    if (idx_allband == -1) {
      if (!plogw->f_console_emu) {
        plogw->ostream->print("no new entry available, allband ignore this;");
        plogw->ostream->println(stn);
      }
      //      return;
      entry_allband=NULL;
    } else {
      entry_allband = bandmap[N_BAND].entry + idx_allband;
    }
  }
  */
  if (!plogw->f_console_emu) plogw->ostream->println("passed");

  strcpy(entry->station, stn);       // station
  entry->freq = ifreq;               // frequency
  //  entry->time = rtctime.unixtime();  // current time for removing the entry in clean_bandmap();
  entry->time = my_rtc.unixtime();  // current time for removing the entry in clean_bandmap();
  entry->mode = modeid;
  entry->remarks[0] = '\0';
  // temporally commented out
  //  strncat(entry->remarks, remarks, 16); // copy remarks
  //  trim(entry->remarks);
  entry->type = 2;
  if (!plogw->f_console_emu) {
    plogw->ostream->print("setting info bandmap  idx= ");
    plogw->ostream->print(idx);
  }
  /*
  if (entry_allband!=NULL) {
    strcpy(entry_allband->station, stn);       // station
    entry_allband->freq = ifreq;               // frequency
    //  entry->time = rtctime.unixtime();  // current time for removing the entry in clean_bandmap();
    entry_allband->time = my_rtc.unixtime();  // current time for removing the entry in clean_bandmap();
    entry_allband->mode = modeid;
    entry_allband->remarks[0] = '\0';
    // temporally commented out
    //  strncat(entry->remarks, remarks, 16); // copy remarks
    //  trim(entry->remarks);
    entry_allband->type = 2;
    if (!plogw->f_console_emu) {
      plogw->ostream->print("setting info bandmap allband idx= ");
      plogw->ostream->print(idx_allband);
    }
  }
  */
}

int in_contest_frequency(int ifreq) {
  if (ifreq >= 1801000/FREQ_UNIT && ifreq <= 1820000/FREQ_UNIT) return 1;
  if (ifreq >= 3510000/FREQ_UNIT && ifreq <= 3530000/FREQ_UNIT) return 1;
  if (ifreq >= 7010000/FREQ_UNIT && ifreq <= 7040000/FREQ_UNIT) return 1;
  if (ifreq >= 14050000/FREQ_UNIT && ifreq <= 14080000/FREQ_UNIT) return 1;
  if (ifreq >= 21050000/FREQ_UNIT && ifreq <= 21080000/FREQ_UNIT) return 1;
  if (ifreq >= 28050000/FREQ_UNIT && ifreq <= 28080000/FREQ_UNIT) return 1;
  if (ifreq >= 50050000/FREQ_UNIT && ifreq <= 50090000/FREQ_UNIT) return 1;
  if (ifreq >= 144050000/FREQ_UNIT && ifreq <= 144090000/FREQ_UNIT) return 1;
  if (ifreq >= 430050000/FREQ_UNIT && ifreq <= 430090000/FREQ_UNIT) return 1;
  if (ifreq >= 1200000000/FREQ_UNIT) return 1;
  return 0;
}

const char *bandid_str[N_BAND+1] = {
  "1.8", "3.5", "  7", " 14", " 21", " 28", " 50", " 2m", "430", "1.2", "2.4", "5.6","10G","10","18","24","ALL"
  //
  // 1     2      3      4      5      6      7      8      9      10     11    12     13    14  15   16
};

// check callsign and remove erratic string
// -  ??????K  ??????/?K
// -  ??????/QRP
// -  ??????AWT

void adjust_callsign(char *stn) {
  int len;
  len = strlen(stn);
  //plogw->ostream->print(stn);plogw->ostream->print(" len = ");plogw->ostream->println(len);
  if (len == 7) {
    if (stn[6] == 'K') {  // ??????K
      if (!plogw->f_console_emu) plogw->ostream->print(stn);
      stn[6] = '\0';
      if (!plogw->f_console_emu) {
        plogw->ostream->print("->");
        plogw->ostream->println(stn);
      }
      return;
    }
  }
  if (len == 9) {
    if (stn[6] == '/' && stn[8] == 'K') {  // ??????/?K
      if (!plogw->f_console_emu) plogw->ostream->print(stn);
      stn[8] = '\0';
      if (!plogw->f_console_emu) {
        plogw->ostream->print("->");
        plogw->ostream->println(stn);
      }
      return;
    }
    if (stn[6] == 'A' && stn[7] == 'W' && stn[8] == 'T') {  // ??????AWT
      if (!plogw->f_console_emu) plogw->ostream->print(stn);
      stn[6] = '\0';
      if (!plogw->f_console_emu) {
        plogw->ostream->print("->");
        plogw->ostream->println(stn);
      }
      return;
    }
  }
  if (len == 10) {
    if (stn[6] == '/' && stn[7] == 'Q' && stn[8] == 'R' && stn[9] == 'P') {
      if (!plogw->f_console_emu) plogw->ostream->print(stn);
      stn[6] = '\0';
      if (!plogw->f_console_emu) {
        plogw->ostream->print("->");
        plogw->ostream->println(stn);
      }
      return;
    }
  }
}




// when register?
// on F4 in S&P mode
// on Enter (new callsign)
void register_current_callsign_bandmap() {
  struct radio *radio;
  radio = so2r.radio_selected();
  int modeid;
  modeid = modeid_string(radio->opmode);  //
  set_info_bandmap(radio->bandid, radio->callsign + 2, modeid, radio->freq, "");
  if (!plogw->f_console_emu) plogw->ostream->println("OK");
  // delete old entry
  //delete_old_entry(radio->bandid, 20);

  // sort the entry
  //sort_bandmap(radio->bandid, bandmap_disp.sort_type);

  upd_display_bandmap();
}




void upd_display_bandmap_show_entry(struct bandmap_entry *p, int count, int bandid) {
  // calc dtime
  int t0;  // current unixtime
  //  t0 = rtctime.unixtime();
  t0 = my_rtc.unixtime();  
  int dt;
  dt = t0 - p->time;  // should give positive number
  //
  int tmp1;



  char c;
  int iy;
  if (count < 0) {
    // on-freq
    c = ' ';
    iy = 0;
  } else {
    // check cursor position
    if (count - bandmap_disp.top_column[bandid-1]== bandmap_disp.cursor[bandid-1]) {
      c = '<';
      // copy info on cursor
      strncpy(bandmap_disp.on_cursor_station, p->station, 10);
      bandmap_disp.on_cursor_freq = p->freq;
      bandmap_disp.on_cursor_modeid = p->mode;
    } else c = ' ';
    iy = count - bandmap_disp.top_column[bandid-1] + bandmap_disp.f_onfreq;
  }
  
  tmp1 = (p->freq % (1000/FREQ_UNIT)) / (100/FREQ_UNIT);
  sprintf(dp->lcdbuf, "%6d.%01d %-8s%c%2d%c", (p->freq / (1000/FREQ_UNIT))%1000000, tmp1, p->station, (p->flag&BANDMAP_ENTRY_FLAG_NEWMULTI) ? 'm': ' ', dt / 60, c);  // mins since received
  
  if (verbose & 64) {
    // show bandmap to console
    plogw->ostream->println(dp->lcdbuf);
  }
  display_printStr(dp->lcdbuf, 10 + iy);
  //  if (verbose & 64) {
  //    plogw->ostream->print((String)p->station);
  //    plogw->ostream->print(" f=");
  //    plogw->ostream->print(p->freq);
  //    plogw->ostream->println(" show");
  //  }
}


double check_frequency(String s) {
  double result;
  char *ErrPtr;
  if (s == "") return ErrorFloat;
  result = strtod(s.c_str(), &ErrPtr);
  if (*ErrPtr == '\0') {  // 正常に変換できた
    return result;
  } else {  // 変換できなかった
    return ErrorFloat;
  }  // if
}


char *ltrim(const char *string) {
  char *tmp = (char *)string;
  for (; *tmp == 0x20 && *tmp != 0x00; tmp++)
    ;
  return tmp;
}

char *rtrim(const char *string) {
  char *tmp = (char *)string;
  // 文字列の大きさ - ヌルバイトの位置
  int s = (strlen(tmp) - 1);
  for (; s > 0 && tmp[s] == 0x20; s--)
    ;
  tmp[s + 1] = 0x00;
  return tmp;
}

char *trim(const char *string) {
  // ltrim と rtrim関数を使用。
  return ltrim(rtrim(string));
}

void select_bandmap_entry(int dir,int bandid) {
  //if (bandmap_disp.ncount1==0) return;
  switch (dir) {
    case -1:  // up
      //   top_column ...   cursor
      if (bandmap_disp.cursor[bandid-1] == 0) {  // cursor at the top
        if (bandmap_disp.top_column[bandid-1] > 0) {
          bandmap_disp.top_column[bandid-1]--;
          if (bandmap_disp.top_column[bandid-1] < 0) {
            bandmap_disp.top_column[bandid-1] = 0;
          }
        }
      } else {
        bandmap_disp.cursor[bandid-1]--;
        if (bandmap_disp.cursor[bandid-1] < 0) bandmap_disp.cursor[bandid-1] = 0;
      }
      break;
    case 1:  // down
      if (bandmap_disp.cursor[bandid-1] + bandmap_disp.f_onfreq == 5) {
        bandmap_disp.top_column[bandid-1]++;
      } else {
        bandmap_disp.cursor[bandid-1]++;
      }
      // position of the end entry : top_column (0) + cursor (5)   = 6 entry displayed
      // top_column permissive : ncount1 -1 or smaller
      if (bandmap_disp.top_column[bandid-1] > bandmap_disp.ncount1 - bandmap_disp.f_onfreq) {
        bandmap_disp.top_column[bandid-1] = bandmap_disp.ncount1 - bandmap_disp.f_onfreq;
        if (bandmap_disp.top_column[bandid-1] < 0) bandmap_disp.top_column[bandid-1] = 0;
      }
      if (bandmap_disp.top_column[bandid-1] + bandmap_disp.cursor[bandid-1]>= bandmap_disp.ncount1 - 1) {
        bandmap_disp.cursor[bandid-1] = bandmap_disp.ncount1 - bandmap_disp.top_column[bandid-1] - 1;
        if (bandmap_disp.cursor[bandid-1] < 0) bandmap_disp.cursor[bandid-1] = 0;
      }
      break;
  }
  
  upd_display_bandmap();
}
void set_target_station_info(char *arg)
{
  // setstninfo {} {} {}'.format(self.stn_call,int(float(self.stn_freq)*1000),self.stn_mode))
  // station freqHz mode(CW)
  // check target radio if exists
  // check if target radio station already have some string
  // if not set station name and frequency, mode
  int freq_recv;
  char mode_recv[10];
  char stn_recv[30];
  if (3 != sscanf(arg, "%s %d %s", stn_recv, &freq_recv, mode_recv)) {
    plogw->ostream->println("failed to receive args set_target_station_info() stn freq(Hz) mode(CW/LSB/USB...)");
    return;
  }
  if (strlen(stn_recv) == 0) {
    // invalid station
    plogw->ostream->print("invalid station:");
    plogw->ostream->println(stn_recv);
    return;
  }
  int bandid_recv;
  bandid_recv = freq2bandid(freq_recv);
  // scan if any radio matches the band
  struct radio *radio;
  int i;
  i = find_appropriate_radio(bandid_recv);
  if (i < 0) {
    // no appropriate radio found
    plogw->ostream->print("no appropriate radio for bandid ");
    plogw->ostream->println(bandid_recv);
    return ;
  }
  radio = &radio_list[i];
  if (strlen(radio->callsign + 2) > 0) {
    plogw->ostream->println("radio found  but call entry there skip");
    return;
  }
  set_station_entry(radio, stn_recv, freq_recv, (const char *)mode_recv);


}
