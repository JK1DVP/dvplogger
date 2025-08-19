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

// this program implements multi-port transport between BT serial, and CH9350 keyboard/mouse, and the main board serial using a single serial connection to the main board
// packeting rule as follows;


// BOP bytes from to data(bytes-3) CKSUM(from bytes to the end of data) EOP
// bytes max 128
// from port#
// BOP 0xFC
// EOP 0xFB
// port # 0 main board control 
//        1 extension control 
//        2 BT serial source
//        3 BT serial receiver
//        4 USB keyboard1 source
//        5 USB keyboard1 receiver
//        6 USB keyboard2 source
//        7 USB keyboard2 receiver

// extension control receive command to switch modes BT I/F only and normal multiplexed
// "BTONLY"
// "MUX"

#include <Arduino.h>

#include "mux_transport.h"
// handler for each data packet defined in
Mux_transport mux_transport;
int f_mux_transport=0;
int f_mux_transport_cmd=0; // 1: transition to f_mux_transport=1 2: transition to f_mux_transport=0

void Mux_transport::sync_transport_modes_master() {
  // send transport mode change commands to extension board
  switch(f_mux_transport_cmd) {
    //  case 0: // raw transport
    //    break;
  case 1: // to mux transport
    mux_stream->print("\r\ngo_mux\r\n");
    debug_print("mux_transport:->mux command send\r\n");    
    f_mux_transport=1;
    f_mux_transport_cmd=0;
    break;
  case 2: // transition to raw transport
    debug_print("mux_transport:->no mux command send\r\n");    
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)"go_no_mux",strlen("go_no_mux")+1);
    f_mux_transport=0;    
    f_mux_transport_cmd=0;
    break;
  }
}

Mux_transport::Mux_transport() {
  //    mux_stream=stream;
    for (int i =0;i<N_PORT;i++) {
      port_handler[i]=NULL;
    }
    for (int i=0;i<N_PACKET_POOL;i++) {
      packet_pool[i].size=100;      
      packet_pool[i].buf=(char *)malloc(sizeof(unsigned char)*packet_pool[i].size);
      packet_pool[i].status=0;
      packet_pool[i].idx=0;
      packet_pool[i].cksum=0;      
    }
  };

struct mux_packet *Mux_transport::get_free_packet()
  {
    int i;
    for (i=0;i<N_PACKET_POOL;i++) {
      if(packet_pool[i].status==0) {
	// free packet found
	break;
      }
    }
    if (i==N_PACKET_POOL) {
      return NULL;
    }
    return &packet_pool[i];
  }


void Mux_transport::set_port_handler(int to_idx,Mux_transport::callback_func handler)
{
  if (to_idx>=0 && to_idx<N_PORT) {
    port_handler[to_idx]=handler;
  }
}

void Mux_transport::recv_pkt() {
  //  int frm, int to, char *data,int ndata)
    if (mux_stream==NULL) return;
    uint8_t c;
    while (mux_stream->available()) {
      c=mux_stream->read();
      if (debug) {
	debug_print("m:");
	debug_print(pkt_status);
	debug_print(":");	
	debug_print((char)c);
	
	debug_print(" ");	
	//	debug_print("\r\n");
      }
      switch(pkt_status) {
      case pkt_wait_bop:
	if (c==bop) {
	  pkt_status=pkt_bytes;
	}
	break;
      case pkt_bytes:
	data_bytes=c-3;

	// check data_bytes not exceed the buffer size
	if (NULL==(packet_reading=get_free_packet())) {
	  pkt_status=pkt_wait_bop;
	  break;
	}
	packet_reading->status=1;
	if (packet_reading->size < data_bytes) {
	  //
	  packet_reading->buf=(char *)realloc(packet_reading->buf,sizeof(char)*data_bytes);
	  packet_reading->size=data_bytes;
	}
	packet_reading->idx=0;

	packet_reading->cksum=c;
	
	pkt_status=pkt_from;
	debug_print("will read packet bytes");
	debug_print(data_bytes);
	debug_print(" bytes\r\n");	
	break;
      case pkt_from:
	packet_reading->from=c;
	debug_print("from: ");
	debug_print(packet_reading->from);
	debug_print("\r\n");
	packet_reading->cksum+=c;	
	pkt_status=pkt_to;
	break;
      case pkt_to:
	packet_reading->to=c;
	packet_reading->cksum+=c;		
	if (packet_reading->to>=N_PORT) {
	  // invalid to
	  packet_reading->status=0;	  
	  pkt_status=pkt_wait_bop;
	  break;
	}
	debug_print("to: ");
	debug_print(packet_reading->to);
	debug_print("\r\n");	

	pkt_status=pkt_data;
	break;
      case pkt_data:
	packet_reading->buf[packet_reading->idx++]=c;
	packet_reading->cksum+=c;
	if (packet_reading->idx>256) {
	  // buffer full -> discard the packet
	  packet_reading->status=0;
	  pkt_status=pkt_wait_bop;
	  break;
	}
	if (packet_reading->idx==data_bytes) {
	  // end of reading packet
	  debug_print("end of data reached\r\n");
	  
	  pkt_status=pkt_cksum;
	}
	break;
      case pkt_cksum:
	if (c!=packet_reading->cksum) {
	  // checksum not match
	  debug_print ("cksum not match");
	  debug_print(packet_reading->cksum);
	  debug_print (" ");
	  debug_print(c);
	  debug_print ("\r\n");	  
	  packet_reading->status=0;
	  pkt_status=pkt_wait_bop;
	  break;
	} else {
	  // complete packet read if the next is eop
	  debug_print("cksum match\r\n");	  
	}
	pkt_status=pkt_eop;
	break;
      case pkt_eop:
	if (c==eop) {
	  debug_print("eop ok\r\n");	  
	  // complete packet received and process by handler
	  packet_reading->status=2;
	  if (port_handler[packet_reading->to]!= NULL) {
	    port_handler[packet_reading->to](packet_reading);
	    // may need to use queue to buffer packets without blocking
	  } else {
	    // just print information : default handler
	    if (debug_stream!=NULL) {
	      debug_stream->print("pkt received status:");
	      debug_stream->print(packet_reading->status);
	      debug_stream->print(" from:");
	      debug_stream->print(packet_reading->from);
	      debug_stream->print(" to:");
	      debug_stream->print(packet_reading->to);
	      debug_stream->print(" size:");
	      debug_stream->print(packet_reading->size);
	      debug_stream->print(" buf:");	      
	      for (int i=0;i<packet_reading->idx;i++) {
		debug_stream->print(packet_reading->buf[i],HEX);
	      }
	      debug_stream->println("");
	    }
	  }
	  packet_reading->status=0; // free packet 
	  pkt_status=pkt_wait_bop;
	}
	break;
      }
    }
  }



int Mux_transport::send_pkt(int frm, int to, unsigned char *data,int ndata)
  {
    // construct a full packet into wbuf[]
    unsigned char *p;uint8_t cksum1;
    if (ndata>256) return 0;
    if (to>=N_PORT) return 0;
    p=sbuf;
    *p++=bop;
    cksum1=0;
    *p++=ndata+3; cksum1+=ndata+3;
    *p++=frm; cksum1+=frm;
    *p++=to; cksum1+=to;
    for (int i=0;i<ndata;i++) {
      *p++=data[i];cksum1+=data[i];
    }
    *p++=cksum1;
    *p++=eop;
    // write
    if (mux_stream!=NULL)  mux_stream->write(sbuf,ndata+6);
    return 1;
    
  }

