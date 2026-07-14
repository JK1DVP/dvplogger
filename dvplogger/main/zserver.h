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

#ifndef FILE_ZSERVER_H
#define FILE_ZSERVER_H
// definitions on zserver (zlog) modulxses
//コード	バンド

// receive buffer size
#define NCHR_ZSERVER_RINGBUF 1024
// send buffer size
#define NCHR_ZSERVER_CMD 1024

struct zserver {
  int timeout; // control cluster info reception
  int timeout_alive; // if no information received for more than a minute, try to reconnect
  int timeout_count;
  int stat;
  struct ringbuf ringbuf;
  char cmdbuf[NCHR_ZSERVER_CMD + 1];
  int cmdbuf_ptr; // number of char written in cmdbuf
  int cmdbuf_len;
};

int connect_zserver() ;
void init_zserver_info();
void zserver_process() ;
void zserver_send(char *buf);
int opmode2zLogmode(char *opmode);
void reconnect_zserver();
void zserver_freq_notification();
extern int f_show_zserver;
extern const char *zserver_freqcodes[]; /*={"1.9M","3.5M","7M","10M","14M","18M","21M","24M","28M","50M","144M","430M","1200M","2400M","5600M","10G" };*/
extern int zserver_bandid_freqcodes_map[];/*={0,0,1,2,4,6,8,9,10,11,12,13,14,15};*/

//コード	モードa
extern const char *zserver_modecodes[]; /* ={"CW","SSB","FM","AM","RTTY","FT4","FT8","OTHER"};*/
//電力コード	
//コード	電力

extern const char *zserver_powcodes[];/*={"1W","2W","5W","10W","20W","25W","50W","100W","200W","500W","1000W" };*/

extern const char *zserver_client_commands[]; /*={ "FREQ","QSOIDS","ENDQSOIDS","PROMPTUPDATE","NEWPX","PUTMESSAGE","!","POSTWANTED",
			"DELWANTED","SPOT","SPOT2","SPOT3","BSDATA","SENDSPOT","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			"CONNECTCLUSTER","PUTQSO","DELQSO","EXDELQSO","INSQSOAT","LOCKQSO","UNLOCKQSO","EDITQSOTO",
			"INSQSO","PUTLOGEX","PUTLOG","RENEW","SENDLOG" };*/


extern const char *zserver_server_commands[];/*={"GETQSOIDS","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			       "BSDATA","POSTWANTED","DELWANTED","CONNECTCLUSTER",
			       "GETLOGQSOID","SENDRENEW","DELQSO","EXDELQSO","RENEW",
			       "LOCKQSO","UNLOCKQSO","BAND","OPERATOR","FREQ","SPOT",
			       "SENDSPOT","PUTQSO","PUTLOG","EDITQSOTO","INSQSO",
			       "EDITQSOTO","SENDLOG" };*/

#endif
