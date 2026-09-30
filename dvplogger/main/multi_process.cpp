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

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "multi.h"
#include "multi_process.h"
#include "display.h"
#include "log.h"
#include "so2r.h"
#include "user_contest_md.h"
#include "cty_chk.h"

struct multi_list multi_list;

bool split_exchange_fields(const char *src, char separator, struct exchange_fields *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!src || !*src || separator == '\0') return false;

  const char *field_start = src;
  while (true) {
    if (out->count >= EXCHANGE_MAX_FIELDS) return false;
    const char *sep = strchr(field_start, separator);
    size_t len = sep ? (size_t)(sep - field_start) : strlen(field_start);
    if (len == 0 || len > LEN_EXCH) return false;
    memcpy(out->field[out->count], field_start, len);
    out->field[out->count][len] = '\0';
    out->count++;
    if (!sep) break;
    field_start = sep + 1;
  }
  return out->count > 0;
}

static bool cqwwrtty_valid_qth(const char *qth) {
  if (!qth || !*qth) return false;
  if (strcasecmp(qth, "DX") == 0) return true;

  // Reuse the ARRLDX W/VE table.  Its Canadian spellings NT/PE are legacy;
  // CQ WW RTTY requires NWT/PEI, so translate only for the shared lookup.
  const char *lookup = qth;
  if (strcasecmp(qth, "NWT") == 0) lookup = "NT";
  else if (strcasecmp(qth, "PEI") == 0) lookup = "PE";

  for (int i = 0; i < N_MULTI && multi_arrldx.mul[i][0] != '\0'; ++i) {
    if (strcasecmp(lookup, multi_arrldx.mul[i]) == 0) return true;
  }
  return false;
}

bool normalize_cqwwrtty_exchange(char *s, size_t size) {
  if (!s || size == 0 || !*s) return false;

  struct exchange_fields fields;
  if (!split_exchange_fields(s, '/', &fields)) return false;
  if (fields.count < 1 || fields.count > 2) return false;

  const char *zone_s = fields.field[0];
  for (const char *p = zone_s; *p; ++p)
    if (!isdigit((unsigned char)*p)) return false;
  int zone = atoi(zone_s);
  if (zone < 1 || zone > 40) return false;

  const char *qth = (fields.count == 2) ? fields.field[1] : "DX";
  if (!cqwwrtty_valid_qth(qth)) return false;

  char uqth[LEN_EXCH + 1];
  size_t qlen = strlen(qth);
  if (qlen >= sizeof(uqth)) return false;
  for (size_t i = 0; i < qlen; ++i) uqth[i] = toupper((unsigned char)qth[i]);
  uqth[qlen] = '\0';

  int n = snprintf(s, size, "%02d/%s", zone, uqth);
  return n > 0 && (size_t)n < size;
}


bool validate_cqwwrtty_exchange_for_call(const char *exchange, const char *callsign) {
  printf("CQWWRTTY DIAG begin call=[%s] exch=[%s]\n",
         callsign ? callsign : "(null)", exchange ? exchange : "(null)");
  if (!exchange || !*exchange || !callsign || !*callsign) {
    printf("CQWWRTTY DIAG FAIL reason=empty-input\n");
    return false;
  }

  char normalized[LEN_EXCH + 1];
  strlcpy(normalized, exchange, sizeof(normalized));
  if (!normalize_cqwwrtty_exchange(normalized, sizeof(normalized))) {
    printf("CQWWRTTY DIAG FAIL reason=normalize raw=[%s]\n", exchange);
    return false;
  }
  printf("CQWWRTTY DIAG normalized=[%s]\n", normalized);

  struct exchange_fields fields;
  if (!split_exchange_fields(normalized, '/', &fields) || fields.count != 2) {
    printf("CQWWRTTY DIAG FAIL reason=split normalized=[%s] count=%d\n",
           normalized, fields.count);
    return false;
  }
  printf("CQWWRTTY DIAG split count=%d zone=[%s] qth=[%s]\n",
         fields.count, fields.field[0], fields.field[1]);

  // Use the existing CTY lookup, including its per-prefix CQ-zone overrides.
  char callbuf[LEN_CALLSIGN + 1];
  char entity[10], entity_desc[40], cqzone[4], ituzone[4], continent[4];
  char lat[10], lon[10], tz[10];
  strlcpy(callbuf, callsign, sizeof(callbuf));
  int cty_ret = get_entity_info(callbuf, entity, entity_desc, cqzone, ituzone,
                                continent, lat, lon, tz);
  printf("CQWWRTTY DIAG cty ret=%d callbuf=[%s] entity=[%s] cqzone=[%s] itu=[%s] cont=[%s]\n",
         cty_ret, callbuf, cty_ret ? entity : "", cty_ret ? cqzone : "",
         cty_ret ? ituzone : "", cty_ret ? continent : "");
  if (!cty_ret) {
    printf("CQWWRTTY DIAG FAIL reason=cty-not-found\n");
    return false;
  }

  // Do not require the CTY-derived CQ zone to match the received zone here.
  // CQWW uses the station location; a callsign prefix is not authoritative for
  // portable/remote operation.  Zone validity itself is checked by CQZONE.

  const bool is_continental_wve =
      (strcasecmp(entity, "K") == 0 || strcasecmp(entity, "VE") == 0);
  const bool qth_is_dx = (strcasecmp(fields.field[1], "DX") == 0);
  const bool qth_valid = cqwwrtty_valid_qth(fields.field[1]);
  printf("CQWWRTTY DIAG classify wve=%d qth_is_dx=%d qth_valid=%d entity=[%s]\n",
         is_continental_wve ? 1 : 0, qth_is_dx ? 1 : 0, qth_valid ? 1 : 0, entity);

  // Continental USA/Canada require a valid W/VE QTH.  All other entities,
  // including KL (Alaska) and KH6 (Hawaii), use DX in the Cabrillo QTH field.
  bool result = is_continental_wve ? (!qth_is_dx && qth_valid) : qth_is_dx;
  printf("CQWWRTTY DIAG result=%s call=[%s] exch=[%s]\n",
         result ? "OK" : "NG", callsign, normalized);
  return result;
}

// initialize multi for the given bands with given multi_item
void init_multi(const struct multi_item *multi, int start_band, int stop_band) {
  //	multi_list.multi = &multi_tama;
  //	multi_list.multi = &multi_tokyouhf;
  //	multi_list.multi = &multi_cqzones;
  if (start_band<0) {
    // all band
    start_band=1;
    stop_band=N_BAND-1;
  }
  if (stop_band<0) {
    stop_band=N_BAND-1;
  }
  if (start_band>=N_BAND||stop_band>=N_BAND) {
    start_band=N_BAND-1;
    stop_band=N_BAND-1;
  }
  // each band init
  for (int iband=start_band;iband<=stop_band;iband++) {
    multi_list.multi[iband-1]=multi;     // set multi here
    if (multi==NULL) {
      //  if (multi_list.multi == NULL) {
      multi_list.n_multi[iband-1] = 0;
    } else {
      // count n_multi for each band
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band:");
	plogw->ostream->print(iband);
	plogw->ostream->print(":");
	plogw->ostream->print(band_str[iband-1]);
      }
      for (int i = 0; i < N_MULTI; i++) {
	if (*multi_list.multi[iband-1]->mul[i] == '\0' ||
	    *multi_list.multi[iband-1]->name[i] == '\0' ) {
	  // end of the list
	  multi_list.n_multi[iband-1] = i;
	  break;
	} else {
	  if (!plogw->f_console_emu) {
	    //	    plogw->ostream->print(multi_list.multi[iband-1]->mul[i]);
	    //	    plogw->ostream->print(" ");
	    //	    plogw->ostream->print(multi_list.multi[iband-1]->name[i]);
	    //	    plogw->ostream->println(" ");
	  }
	}
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print(" N multi=");
	plogw->ostream->println(multi_list.n_multi[iband-1]);
      }
      // clear worked list
      for (int i = 0; i < multi_list.n_multi[iband-1]; i++) {
	//	for (int iband = 0; iband < N_BAND; iband++) {
	multi_worked_set(&multi_list, iband-1, i, false);
      }
    }
  }
}

void clear_multi_worked() {
  // clear worked list
  for (int iband = 0; iband < N_BAND; iband++) {  
    for (int i = 0; i < multi_list.n_multi[iband-1]; i++) {
      multi_worked_set(&multi_list, iband-1, i, false);
    }
  }
}

int multi_check(char *s,int bandid) {   // s: exch (such as in plogw->recv_exch +2)
  return multi_check_option(s,bandid,0);
}

int check_kennai(char *s,int len) // if *s shows kennai stations in the current contest return 1 else 0
{
    char exch_head[10];char multi_ken[10];
    int ken_number;
    ken_number=(plogw->multi_type & 0xff00)>>8;
    if (ken_number>=100) {
      sprintf(multi_ken,"%03d",ken_number);
    } else {
      sprintf(multi_ken,"%02d",ken_number);
    }
    //    plogw->ostream->print("ken:");
    //    plogw->ostream->println(multi_ken);
    *exch_head='\0';
    strncat(exch_head,s,len);
    //    plogw->ostream->print("exch_ken:");
    //    plogw->ostream->println(exch_head);
    if (strncmp(exch_head,multi_ken,strlen(multi_ken))==0) {
      // contest kennai stations just search for acag entries
      // let the later multi check routine do the job
      return 1;
    } else {
      // contest kengai stations
      // special search for the later ACAG entry (allja entries)
      return 0;

    }
}

int multi_check_option(char *s,int bandid,int option) {   // s: exch (such as in plogw->recv_exch +2)
  // now option is not used
  // check multi for the bandid

  if (bandid<=0|| bandid>N_BAND) return -1;
  //if (bandid<=0|| bandid>N_BAND) return 0;  

  if (multi_list.multi[bandid-1] == NULL) return 0;
  char exch_buf[10];
  char compound_multi_buf[10];  // field 0 of a compound exchange (e.g. CQWWRTTY zone)
  int len;
  int tmp;
  len = strlen(s);
  if (verbose & 4) {
    console->print("multi_type=");
    console->println(plogw->multi_type);
  }
  int idx_multi_start=0; // start index to search target multiplier
  int idx_multi_end=multi_list.n_multi[bandid-1]; // start index to search target multiplier
  if (plogw->contest_id == 18) {
    //    plogw->ostream->println("contest_id == 18");
    idx_multi_end=MULTI_ACAG_ALLJA_OFS;
  }
  switch (plogw->multi_type &0xff) {
  case MULTI_TYPE_USER_MD:
    return user_md_multi_check(s, bandid);
  case MULTI_TYPE_CQWWRTTY: {
    char normalized[LEN_EXCH + 1];
    strlcpy(normalized, s, sizeof(normalized));
    if (!normalize_cqwwrtty_exchange(normalized, sizeof(normalized))) return -1;

    // Validate the compound exchange first, then feed only field 0 (CQ Zone)
    // to the existing CQ-zone multiplier lookup.
    struct exchange_fields fields;
    if (!split_exchange_fields(normalized, '/', &fields) || fields.count != 2) return -1;
    // multi_cqzones stores 1..9 without a leading zero.  Keep 03/CA in the
    // log, but use "3" for the existing multiplier-table lookup.
    snprintf(compound_multi_buf, sizeof(compound_multi_buf), "%d", atoi(fields.field[0]));
    s = compound_multi_buf;
    len = strlen(s);
    break;
  }
  case MULTI_TYPE_NORMAL: // multi same as number
  case MULTI_TYPE_CQWW: // multi same as number     
  case MULTI_TYPE_JARL_PWR_NOMULTICHK: // jarl contest power_code but no multi-check performed (like ACAG)
    break;
  case MULTI_TYPE_KENNAI_KJ:
    // check KenJin stations
    if (len>=2) {
      if (strncasecmp(s+len-2,"KJ",2)==0) { // tailing with KJ
	len-=2;
	if (check_kennai(s,len)) {
	  //      plogw->ostream->println("kennai");      
	  idx_multi_end=MULTI_ACAG_ALLJA_OFS;
	  break;
	} else {
	  return -1; // Kengai and KJ NG
	}
      } else {
	// ken number contests
	if (check_kennai(s,len)) {
	  //      plogw->ostream->println("kennai");      
	  idx_multi_end=MULTI_ACAG_ALLJA_OFS;
	} else {
	  //      plogw->ostream->println("kengai");
	  idx_multi_start=MULTI_ACAG_ALLJA_OFS;
	}
	break;
      }
    }
    break;
  case MULTI_TYPE_KENNAI:
    // ken number contests
    if (check_kennai(s,len)) {
      //      plogw->ostream->println("kennai");      
      idx_multi_end=MULTI_ACAG_ALLJA_OFS;
    } else {
      //      plogw->ostream->println("kengai");
      idx_multi_start=MULTI_ACAG_ALLJA_OFS;
    }
    break;
  case MULTI_TYPE_KENGAI_KJ:
    // check KenJin stations
    if (len>2) {
      if (strncasecmp(s+len-2,"KJ",2)==0) { // tailing with KJ
	len-=2;
      }
    }
    __attribute__((fallthrough)); // validate the remaining exchange as KENGAI
  case MULTI_TYPE_KENGAI:
    // ken number contests
    if (check_kennai(s,len)) {
      //      plogw->ostream->println("kennai");
      idx_multi_end=MULTI_ACAG_ALLJA_OFS;
    } else {
      return -1;
    }
    break;
  case MULTI_TYPE_AA: // all chrs should be numeric
    tmp=0;
    for (int i=0;i<len;i++) {
      if (!isdigit(s[i])) {
	tmp=1;
	break;
      }
    }
    if (tmp==0) {
      return 0;
    }
    return -1;
    break;
  case MULTI_TYPE_JARL_PWR: // jarl contest: ignore last character (power code)
    if (verbose & 1) printf("JARL contest\n");
    // check power character
    char c;
    c = *(s + len - 1);
    if (c != 'P' && c != 'M' && c != 'L' && c != 'H' && c != 'Q') {
      if (verbose & 1) {
	printf("wrong powercode\n");
      }
      return -1;
    }
    len--;
    break;

  case MULTI_TYPE_KCWA: // only check head two character
    len=2;
    break;
  case MULTI_TYPE_NOCHK_LASTCHR: // ignore last character (JA8 contest) no check for the last character
    len--;
    break;
  case MULTI_TYPE_ARRLDX: //  US W/VE stations send a signal report and their state or province. 
    //  DX stations send a signal report and power as a number or abbreviation.
    if (isdigit(s[0])|| (s[0]=='K')) {
      return 0; // allow starting by a number or K (killo)
    }
    break;
  case MULTI_TYPE_ARRL10M:
    //    5.2.1 Multipliers count once on phone and once on CW.
    //5.2.2 W/VE and Mexican states, the District of Columbia (DC), and Canadian Provinces
    //and Territories plus Labrador. (See the ARRL Contest Multipliers List.)
    //5.2.2.1 Hawaii (KH6) and Alaska (KL7) count as US states.
    //5.2.3 DXCC entities
    //5.2.4 ITU region
    // --> allow all characters number 
    tmp=0;
    for (int i=0;i<len;i++) {
      if (!isdigit(s[i])) tmp=1;
    }
    if (tmp==0) {
      return 0;
    }
    break;
  case MULTI_TYPE_GL_NUMBERS: // HS was  check GL or numbers
    if (len==4) {
      // check
      if (isalpha(s[0]) && isalpha(s[1]) && isdigit(s[2]) && isdigit(s[3])) {
	return 0; // acceptable GL multi 
      }
    }
    break;
  case MULTI_TYPE_UEC:  // UEC contest H/I/L/UEC
    if (strcmp(s + len - 1, "I") == 0) {
      // I
      len--;
      break;
    } else if (strcmp(s + len - 1, "H") == 0) {
      len--;
      break;
    } else if (strcmp(s + len - 1, "L") == 0) {
      len--;
      break;
    } else {
      if (len >= 3) {
	if (strcmp(s + len - 3, "UEC") == 0) {
	  len -= 3;
	  break;
	}
      }
      return -1;
    }
    break;

  }

  if (len < 1) return -1;
  *exch_buf = '\0';
  strncat(exch_buf, s, len);
  //  plogw->ostream->print("idx_multi_start:");  
  //  plogw->ostream->println(idx_multi_start);
  //  plogw->ostream->print("idx_multi_end:");  
  //  plogw->ostream->println(idx_multi_end); 
  for (int i = idx_multi_start; i < idx_multi_end; i++) {
    if (strcmp(multi_list.multi[bandid-1]->mul[i], exch_buf) == 0) {
      // hit
      //      log_d(VERBOSE_UI,"hit %d\n",i);      
      return i;
    }
  }
  //  log_d(VERBOSE_UI,"not hit anything\n");
  return -1;
}


void print_multi_list(Stream *out)
{
  if (!out) out = console;
  struct radio *radio;
  radio=so2r.radio_selected();
  // show multi list below
  if (radio->bandid >= 1) {
    if ( (multi_list.multi[radio->bandid-1] != NULL) ) {

      sprintf(dp->lcdbuf, "Multi in %s MHz", band_str[radio->bandid - 1]);
      out->println(dp->lcdbuf);
      //      display_printStr(dp->lcdbuf, 13);

      char buf1[10];
      int count;
      int len;
      count = 0;
      int countrow;
      countrow = 0;
      *dp->lcdbuf = '\0';

      for (int i = 0; i < multi_list.n_multi[radio->bandid-1]; i++) {
	if (i >= multi_list.n_multi[radio->bandid-1]) break;
	sprintf(buf1, "%c%s ", multi_worked_get(&multi_list, radio->bandid - 1, i) ? '*' : ' ', multi_list.multi[radio->bandid-1]->mul[i]);
	len = strlen(buf1);
	if (count + len > 80) {  // use next row
	  out->println(dp->lcdbuf);
	  *dp->lcdbuf = '\0';
	  count = 0;
	  // check row
	  //	  if (countrow>2) { // no displayable area available
	  //	    break;
	  //	  }
	  countrow++;
	}
	strcat(dp->lcdbuf, buf1);
	count += len;
      }
      out->println(dp->lcdbuf);

    }

  }
}

int multi_check_old() {
  struct radio *radio;
  radio = so2r.radio_selected();
  if (multi_list.multi[radio->bandid-1] == NULL) return 0;
  char exch_buf[10];
  int len;
  len = strlen(radio->recv_exch + 2);
  switch (plogw->multi_type) {
    case 0:
    case 3:  // jarl contest power_code but no multi-check performed
      break;
    case 1:
      // jarl contest ignore last character
      if (verbose & 1) plogw->ostream->println("JARL contest");
      // check power character
      char c;
      c = *(radio->recv_exch + 2 + len - 1);
      if (c != 'P' && c != 'M' && c != 'L' && c != 'H' && c != 'Q') {
        if (verbose & 1) {
          plogw->ostream->println("wrong powercode");
        }
        return -1;
      }
      len--;
      break;
    case 4:
      // ignore last character (JA8 contest)
      len--;
      break;
    case 2:  // UEC contest H/I/L/UEC
      if (strcmp(radio->recv_exch + 2 + len - 1, "I") == 0) {
        // I
        len--;
        break;
      } else if (strcmp(radio->recv_exch + 2 + len - 1, "H") == 0) {
        len--;
        break;
      } else if (strcmp(radio->recv_exch + 2 + len - 1, "L") == 0) {
        len--;
        break;
      } else {
        if (len >= 3) {
          if (strcmp(radio->recv_exch + 2 + len - 3, "UEC") == 0) {
            len -= 3;
            break;
          }
        }
        return -1;
      }

      break;
  }
  if (verbose & 1) {
    plogw->ostream->print(radio->recv_exch + 2);
    plogw->ostream->print(":len=");
    plogw->ostream->println(len);
  }
  if (len < 1) return -1;
  *exch_buf = '\0';
  strncat(exch_buf, radio->recv_exch + 2, len);
  for (int i = 0; i < multi_list.n_multi[radio->bandid-1]; i++) {
    //	plogw->ostream->print(i);
    //	plogw->ostream->print(":");
    //	plogw->ostream->print(multi_list.multi->mul[i]);
    //	plogw->ostream->print(":");
    //	plogw->ostream->println(plogw->recv_exch+2);
    if (strcmp(multi_list.multi[radio->bandid-1]->mul[i], exch_buf) == 0) {
      // hit
      return i;
    }
  }
  return -1;
}

void entry_multiplier(struct radio *radio) {
  //  struct radio *radio;
  //  radio = so2r.radio_selected();
  if (multi_list.multi[radio->bandid-1] == NULL) return;
  if (radio->multi < 0) return;
  if (radio->multi >= multi_list.n_multi[radio->bandid-1]) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("errortic multi id:");
      plogw->ostream->println(radio->multi);
    }
    return;
  }
  // new multi check ?
  if (!multi_worked_get(&multi_list, radio->bandid - 1, radio->multi)) {
    // new multi found
    // if (verbose & 1) {
    //	  plogw->ostream->print("new multi:");plogw->ostream->println(plogw->multi);
    // }
    score.nmulti[radio->bandid - 1]++;
  }
  if (verbose&4) {
    console->print("entry_multiplier() radio=");console->print((int)radio->rig_idx);
    console->print(" bandid=");console->print((int)radio->bandid);console->print(" multi=");console->println((int)radio->multi);
  }
  multi_worked_set(&multi_list, radio->bandid - 1, radio->multi, true);
}


// reverse search multi name and
// return with the index of multi if found name, otherwise return -1
void reverse_search_multi() {
  struct radio *radio;
  radio = so2r.radio_selected();
  
  if (multi_list.multi[radio->bandid-1] == NULL) return;
  // input is remarks until space
  // copy to buf
  char buf[128];
  int count;
  count = 0;
  char *p, *p1;
  p = radio->remarks + 2;
  p1 = buf;
  while (*p && count < 128) {
    if (*p == ' ') {
      break;
    }
    if (*p == '_') {
      *p1++ = ' '; // replace _ to space
      p++;
    } else {
      *p1++ = *p++;
    }
    count++;
  }
  *p1 = '\0';
  if (!plogw->f_console_emu) {
    plogw->ostream->print("reverse searching:");
    plogw->ostream->println(buf);
    sprintf(dp->lcdbuf, "reverse searching\n%s\n",p1);    
    upd_display_info_flash(dp->lcdbuf);    
  }
  int len;
  len=strlen(buf);

  for (int i = 0; i < multi_list.n_multi[radio->bandid-1]; i++) {
    if (strncasecmp(multi_list.multi[radio->bandid-1]->name[i], buf,len) == 0) {
      // hit
      //multi_list.multi->mul[i];
      //return i;
      if (!plogw->f_console_emu) {
        plogw->ostream->print("found ");
        plogw->ostream->println(multi_list.multi[radio->bandid-1]->mul[i]);
	sprintf(dp->lcdbuf, "found=%s\n%s\n",
		multi_list.multi[radio->bandid-1]->mul[i],
		multi_list.multi[radio->bandid-1]->name[i]);
	upd_display_info_flash(dp->lcdbuf);    
      }

      // replace recv_exch with searched multi
      strcpy(radio->recv_exch + 2, multi_list.multi[radio->bandid-1]->mul[i]);
      radio->recv_exch[1] = strlen(radio->recv_exch + 2);  // cursor to the end of multi
      radio->ptr_curr = 1;
      upd_display();
      return;
    }
  }
  sprintf(dp->lcdbuf, "not found\n");
  upd_display_info_flash(dp->lcdbuf);    
  // not found
  return;
}
