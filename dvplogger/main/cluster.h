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

#ifndef FILE_CLUSTER_H
#define FILE_CLUSTER_H
#include "Arduino.h"
//#include <HTTPClient.h>

//extern WiFiClient client;
extern char cluster_server[40];
extern int cluster_port ;
#define NCHR_CLUSTER_RINGBUF 1024
extern char cluster_buf[NCHR_CLUSTER_RINGBUF];
extern struct cluster cluster;
void print_cluster_info(struct bandmap_entry *entry, int bandid, int idx );
void get_info_cluster(const char *s) ;
void upd_bandmap_cluster(const char *s);
void cluster_process() ;
void init_cluster_info() ;
void disconnect_cluster() ;
void disconnect_cluster_temp() ;
int connect_cluster() ;
extern const char * callsign ;
extern const char * cluster_cmd[3] ;
void set_cluster() ;
void send_cluster_cmd() ;
extern int f_show_cluster;


#endif
