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
#include "multi_process.h"
#include "dupechk.h"
#include "multi.h"
#include "display.h"
#include "contest.h"
#include "so2r.h"


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
  { 7,"AllJA"      ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL  ,&multi_allja,-1,-1,NULL,-1,-1 },
  {18,"ACAG"       ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL  ,&multi_acag, 1,13,NULL,-1,-1 },
  {19,"FD"         ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL  ,&multi_allja, 1,10,&multi_acag,11,-1 },
  { 3,"CQWW"       ,CW_PH_DUPE_NG,1,MULTI_TYPE_CQWW,&multi_cqzones,-1,-1,NULL,-1,-1 },
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
  {14,"Ja8Int"     ,CW_PH_DUPE_NG,1,MULTI_TYPE_NOCHK_LASTCHR,&multi_ja8int, -1,-1,NULL,-1,-1 },
  { 9,"ACAGnochk"  ,CW_PH_DUPE_NG,1,MULTI_TYPE_JARL_PWR_NOMULTICHK  ,NULL,-1,-1,NULL,-1,-1 },
  { -1,""         ,0   ,0,0,NULL,-1,-1,NULL,-1,-1  }
};
//  { 0,"NOMULTI"   ,CW_PH_DUPE_NG,1,0,&multi_test_line,-1,-1,NULL,-1,-1 }, 
  
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
      plogw->multi_type=contest_defs[i].multi_type;
      plogw->cw_pts=contest_defs[i].cw_pts;
      // set multi
      if (contest_defs[i].multi1!=NULL) {
	init_multi(contest_defs[i].multi1,contest_defs[i].multi1_start_band,contest_defs[i].multi1_stop_band);
      }
      if (contest_defs[i].multi2!=NULL) {
	init_multi(contest_defs[i].multi2,contest_defs[i].multi2_start_band,contest_defs[i].multi2_stop_band);
      }
      upd_display_info_contest_settings(so2r.radio_selected());      
      return;
    }
  }
  plogw->contest_id=0;
  plogw->ostream->println("contest_id not found -> NOMULTI");
}

