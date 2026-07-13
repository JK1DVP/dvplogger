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
// Copyright (c) 2021-2025 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later


// 24/7/29
// python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 summary
//  python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 set_flash_voltage 3.3V
// to disable selecting flash voltage from pin stage SPI interface

// memo cq sp frequency gets equal if quickly alt-q's pressed
// in band changing operation cq/s and p, and phone/cw should be remembered for each band
// radio->cq_modetype_bank  does that but not implemented anything 24/07/30
// save/recall routine would take care of that
// implemented above but cq/sp switch doesnot recall old frequency (may be saved overwriting)

#include "Arduino.h"
#include "user_contest_md.h"
#include "decl.h"
#include "hardware.h"
#include "variables.h"
#include "multi.h"
#include <Wire.h>
#include "SPIFFS.h"
#include <WiFi.h>
#include "sd_files.h"
#include "usb_host.h"
#include "cw_keying.h"
#include "iambic_keyer.h"
#include "display.h"
#include "cat.h"
#include "log.h"
#include "qso.h"
#include "cluster.h"
#include "bandmap.h"
#include "multi_process.h"
#include "ui.h"
#include "so2r.h"
#include "processes.h"
#include "misc.h"
#include "settings.h"
#include "edit_buf.h"
#include "mcp.h"
#include "console.h"
#include "tcp_server.h"
#include "timekeep.h"
#include "network.h"
#include "zserver.h"
#include "satellite.h"
#include "dac-adc.h"
#include "web_server.h"
#include "mux_transport.h"
#include "adafruit_usbhost.h"
#include "AudioPlayer.h"

#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp32_flasher.h"
#include <SoftwareSerial.h>
#include "callhist.h"
#include "callhist_remote.h"
#include "callhist_mem.h"
#include "esp_heap_caps.h"
#include "morse_decoder_simple.h"
#include "esp_intr_alloc.h"
#include "cardkey.h"

void usb_loop_task(void *arg)
{
    while(1){
      loop_usb(); // for older USB host library
      ACMprocess(); // test just receiving      
      vTaskDelay(10);
    }
}


TaskHandle_t gxHandle_USBloop;

// usb polling process in separate task 
void usb_loop_setup()
{
  xTaskCreate(usb_loop_task, "usb_loop", 4096, NULL, 2, &gxHandle_USBloop);
}

Stream *console; 


void request_memstat_main_subcpu()
{
  if (f_mux_transport) {
    const char *request = "memstat";
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                           (unsigned char *)request, strlen(request));
    console->println("requested main/sub CPU memory status");
  } else {
    console->println("MUXTRANS is not active");
    sprintf(dp->lcdbuf, "MEMSTAT\nMUXTRANS inactive\n");
    upd_display_info_flash(dp->lcdbuf);
  }
}

// main board message received
void receive_pkt_handler_main_brd(struct mux_packet *packet)
{
  // packet->buf: data
  // packet->idx: number of data
  // message from ext board
  char buf[256];
  if (verbose &4) console->println("receive_pkt_handler_main_brd()");  
  if (strncmp(packet->buf,"chdone:",7)==0 ||
      strncmp(packet->buf,"chack:",6)==0) {
    size_t n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, n); buf[n] = '\0';
    process_callhist_control_response_main(buf);
  } else if (strncmp(packet->buf,"dupepr:",7)==0) {
    size_t n = min((size_t)(packet->idx - 7), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 7, n);
    buf[n] = '\0';
    process_dupechk_partial_response_maincpu(buf);
  } else if (strncmp(packet->buf, "memstat:", 8) == 0) {
    unsigned int sub_free8, sub_min8, sub_largest8;
    unsigned int sub_internal, sub_spiram;
    unsigned int sub_nmaxqso, sub_ncallsign;

    if (packet->idx < 8) {
      console->println("invalid subcpu memstat packet");
      return;
    }

    size_t len = min((size_t)(packet->idx - 8), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 8, len);
    buf[len] = '\0';

    int n = sscanf(buf,
                   "%u|%u|%u|%u|%u|%u|%u",
                   &sub_free8, &sub_min8, &sub_largest8,
                   &sub_internal, &sub_spiram,
                   &sub_nmaxqso, &sub_ncallsign);

    if (n == 7) {
      unsigned int main_free8 =
          heap_caps_get_free_size(MALLOC_CAP_8BIT);
      unsigned int main_min8 =
          heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      unsigned int main_largest8 =
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      unsigned int main_internal =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      unsigned int main_spiram =
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

      console->printf(
          "maincpu heap: free=%u min=%u largest=%u "
          "internal=%u spiram=%u\n",
          main_free8, main_min8, main_largest8,
          main_internal, main_spiram);
      console->printf(
          "subcpu heap: free=%u min=%u largest=%u "
          "internal=%u spiram=%u dupe=%u/%u\n",
          sub_free8, sub_min8, sub_largest8,
          sub_internal, sub_spiram,
          sub_ncallsign, sub_nmaxqso);

      sprintf(dp->lcdbuf,
              "MEM M%u S%u D%u\n"
              "CPU  Free Min Max\n"
              "MAIN %u %u %u\n"
              "SUB  %u %u %u\n"
              "INT  %u     %u\n"
              "PS   %u     %u\n",
              (unsigned int)dupechk->nmaxqso,
              sub_nmaxqso,
              sub_ncallsign,
              main_free8 / 1024,
              main_min8 / 1024,
              main_largest8 / 1024,
              sub_free8 / 1024,
              sub_min8 / 1024,
              sub_largest8 / 1024,
              main_internal / 1024,
              sub_internal / 1024,
              main_spiram / 1024,
              sub_spiram / 1024);

      upd_display_info_flash(dp->lcdbuf);
    } else {
      console->print("invalid subcpu memstat response: ");
      console->println(buf);
    }
    return;
  } else if (strncmp(packet->buf,"duper:",6)==0) {
    unsigned int query_id;
    int is_dupe, has_exch;
    char exch[LEN_EXCH + 1] = "";

    size_t n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, n);
    buf[n] = '\0';

    if (sscanf(buf + 6, "%u %d %d %10s",
               &query_id, &is_dupe, &has_exch, exch) == 4) {
      if (dupechk->dupechk_status == 1 &&
          query_id == dupechk->dupechk_query_id) {
        dupechk->dupechk_dupe = is_dupe ? 1 : 0;
        dupechk->dupechk_getexch = has_exch ? 1 : 0;
        if (has_exch && strcmp(exch, "-") != 0) {
          strncpy(dupechk->dupechk_exch, exch, LEN_EXCH);
          dupechk->dupechk_exch[LEN_EXCH] = '\0';
        } else {
          dupechk->dupechk_exch[0] = '\0';
        }
        dupechk->dupechk_status = 0;
      } else if (verbose & 4) {
        console->println("ignored stale duper response");
      }
    } else {
      console->println("invalid duper response");
    }
  } else if (strncmp(packet->buf,"dupebulk0:",10)==0 ||
             strncmp(packet->buf,"dupebulk1:",10)==0) {
    int group = packet->buf[8] - '0';
    size_t n = min((size_t)(packet->idx - 10), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 10, n);
    buf[n] = '\0';
    process_makedupe_score_maincpu(buf, group);
  } else if (strncmp(packet->buf,"dupereset:",10)==0) {
    notify_dupechk_subcpu_reset();
    console->println("subcpu dupe database cleared");
  } else if (strncmp(packet->buf,"duped:",6)==0) {
    // dupe database status
    size_t n = min((size_t)(packet->idx - 6), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 6, n);
    buf[n] = '\0';
    console->print("received duped=");
    console->println(buf);
  } else if (strncmp(packet->buf,"playq:",6)==0) {
    // response to 'playq' command, playq: with currently playing string
    console->print("Now playing:");
    strncpy(buf,packet->buf+6,packet->idx-6);
    *plogw->playing='\0';
    strncat(plogw->playing,packet->buf+6,min(packet->idx-6,50));
    if (strlen(plogw->playing)==0) {
      console->println("play queue len=0 finished playing?");
      plogw->f_playing=0;
      if (so2r.query_queue_monitor_status()==1) {
	so2r.set_queue_monitor_status(0);
	// clear cw_buf_display
	display_cw_buf_lcd("");
      }
      console->println("play finished.at playq");
      // ptt control
      struct radio *radio;
      radio=so2r.radio_tx();
      if (radio->ptt) {
	set_ptt_rig(radio,0);
	radio->ptt=0;
	console->println("ptt control off");
      } else {
	console->println("ptt already off !? no change");
      }
    }
    console->println(plogw->playing);
    display_cw_buf_lcd(plogw->playing);
  } else if (strncmp(packet->buf,"playc:",6)==0) {
    plogw->f_playing=0;
    if (so2r.query_queue_monitor_status()==1) {
      so2r.set_queue_monitor_status(0);
      // clear cw_buf_display
      display_cw_buf_lcd("");
    }
    console->println("play finished.");
    // ptt control
    struct radio *radio;
    radio=so2r.radio_tx();
    if (radio->ptt) {
      set_ptt_rig(radio,0);
      radio->ptt=0;
      console->println("ptt control off");
    } else {
      console->println("ptt already off !? no change");
    }
  }
}

void check_spiram()
{
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>0) f_spiram=1; else f_spiram=0;

}

void setup()
{

  init_logwindow();
  check_spiram();
  radio_list[0].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[1].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[2].bt_buf = (char *)malloc(sizeof(char) * 256);
  Wire.begin();

  init_mcp_port();

  Serial.println("mcp port init");

  init_mux_serial();
  init_cat_serialport();
  
  pinMode(21, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);

  init_callhist_list();
  
  // subcpu start

  loader_boot_init_func();
  loader_reset_init_func();
  loader_port_reset_target_func();
  console->println("subcpu reset");

  
  init_sat();

  init_qso();
  init_bandmap();
  init_info_display();

  init_multi(NULL,-1,-1);
  
  init_all_radio();
  init_settings_dict();

  init_display();


  /*
   * Create the CAT USB queues before starting the USB task.
   * usb_loop_task() calls ACMprocess(), which accesses these queues.
   */
  init_cat_usb();

  init_usb();
  usb_loop_setup(); // start USB polling task
  // adafruit_usbhost_setup();  
  
  so2r.set_rx(so2r.rx());
  so2r.set_tx(so2r.tx());  
  
  //  #ifdef notdef
  plogw->ostream->print("rig rigspec check");

  attach_interrupt_civ();

  init_cw_keying();

  init_iambic_keyer();
  
  init_sd();

  load_rigs("RIGS");
  
  load_settings("settings");

  init_network();
  init_timekeep();

  upd_display();


  so2r.set_status();
  open_qsolog();

  strcpy(plogw->grid_locator_set, plogw->grid_locator + 2);
  set_location_gl_calc(plogw->grid_locator_set);
  print_memory();

  //  btserial_init();
  plan13_test();

  adc_setup();
  
  init_webserver();

  init_cardkey();   

  // multiplexed data transport between boards through a serial port
  f_mux_transport=0;
  mux_transport=Mux_transport();
  mux_transport.mux_stream=&Serial2;
  mux_transport.debug_stream=console;
  mux_transport.debug=1;
  mux_transport.debug=0;  
  mux_transport.set_port_handler(MUX_PORT_CAT2_MAIN,receive_pkt_handler_cat2);
  mux_transport.set_port_handler(MUX_PORT_CAT3_MAIN,receive_pkt_handler_cat3);
  mux_transport.set_port_handler(MUX_PORT_MAIN_BRD_CTRL,receive_pkt_handler_main_brd);
  mux_transport.set_port_handler(MUX_PORT_BT_SERIAL_MAIN,receive_pkt_handler_btserial);
  mux_transport.set_port_handler(MUX_PORT_USB_KEYBOARD1_MAIN,receive_pkt_handler_keyboard1_main);

  // go mux
  Serial2.print("\r\ngo_mux\r\n");
  f_mux_transport=1;

  /*
   * callhist_at is restored by load_settings().  Load the configured
   * Call History after MUXTRANS is available, because SUBCPU mode needs
   * the inter-CPU control channel.
   */
  if (callhist_at != 0 && callhist_at != 1) {
    console->printf("invalid callhist_at=%d; using MAIN CPU\n", callhist_at);
    callhist_at = 0;
  }
  if (plogw->enable_callhist) {
    int n = 0;
    if (callhist_at == 1) {
      delay(100);  // allow the SUB CPU MUX command handler to become ready
      if (load_callhist_subcpu(callhistfn)) {
        n = get_callhist_subcpu_count();
      }
    } else {
      n = read_callhist_list(callhistfn);
    }
    console->printf("startup callhist: file=%s at=%s entries=%d\n",
                    callhistfn, callhist_at ? "SUBCPU" : "MAIN", n);
  }
}



void check_Serial2()
{
  int count1=0;
  char c;
    while (Serial2.available()) {
      c=Serial2.read();
      console->print("%");
      console->print(c);
      count1++;
    }
  
}



struct paddle_queue paddle_queue_recv;

int prev_n_adc_i2s_read=0;
void loop() {
  if (f_mux_transport) {
    mux_transport.recv_pkt();
  } else {
  }

  decoder.morse_decode_task(); // called in main loop
  decoder.monitor_task(); // morse decoder display task 


  // cardkey
  if (f_cardkey_present) {
    cardkey_process();
  }
  
  // Check the latest iambic-keyer diagnostic sample at a modest rate.
  // Paddle processing itself remains in iambic_keyer_handler().
  if ((verbose & 2048) && xQueuePaddle != NULL) {
    static uint32_t next_paddle_report_ms = 0;
    uint32_t now_ms = millis();
    if ((int32_t)(now_ms - next_paddle_report_ms) >= 0) {
      next_paddle_report_ms = now_ms + 20;
      if (xQueueReceive(xQueuePaddle, &paddle_queue_recv, 0) == pdTRUE) {
        printf("Paddle %d %d\n", paddle_queue_recv.paddle,
               paddle_queue_recv.voltage);
      }
    }
  }
  //  btserial_process();
  
  
  Prs.process_keyrpt_queue(); // process keyboard input
  Prs1.process_keyrpt_queue(); // process keyboard input from external

  process_qso_file_operation(); // advance one SD-backed QSO file operation step

  process_user_md_contest();

  Control_TX_process();  
  time_measure_start(1);    timekeep();  time_measure_stop(1);  
  time_measure_start(2);
  //  if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R) { // maybe need to run task in all radio_mode 25/8/13
  so2r.task();
  //  }
  time_measure_stop(2);  
  time_measure_start(3);   civ_process();time_measure_stop(3);
  

  
  time_measure_start(4);   
  if (wifi_timeout < millis()) {
    wifi_timeout = millis() + 2000;
    check_wifi();
  }
  time_measure_stop(4);  
  //  console->println(digitalRead(15));
  time_measure_start(5);   interval_process(); time_measure_stop(5);  
  time_measure_start(6);   signal_process();  time_measure_stop(6);  
  time_measure_start(7);   rotator_sweep_process();  time_measure_stop(7);  
  //  time_measure_start(8);   repeat_func_process();  time_measure_stop(8);
  //  time_measure_start(8);   sequence_manager();  time_measure_stop(8);
  time_measure_start(8);   so2r.task();  time_measure_stop(8);      

  if (plogw->sat) {
    sat_find_nextaos_sequence();
  }
  time_measure_start(9);   cluster_process();  time_measure_stop(9);  
  zserver_process();
  time_measure_start(10);   display_cwbuf();  time_measure_stop(10);  
  //  time_measure_start(11);   tcpserver_process();   time_measure_stop(11);  // replaced with AsyncTCP based process
  time_measure_start(12);   console_process();   time_measure_stop(12);  
  //  time_measure_start(13);   loop_usb();   time_measure_stop(13); --> moved to freertos task
  main_loop_revs++;
  //  #endif
  delay(1);
}

SoftwareSerial Serial3;

extern "C" void app_main(void)
{
  initArduino();
  digitalWrite(LED, 0);
  digitalWrite(CW_KEY1, 0);
  digitalWrite(CW_KEY2, 0);
  pinMode(LED, OUTPUT);
  pinMode(CW_KEY1, OUTPUT);
  pinMode(CW_KEY2, OUTPUT);
  
  Serial.begin(115200);

  while (!Serial) ;  
  console=&Serial;  
  console->println("start console port");
  console->flush();
  
  setup();
  
  while (1) {
    loop();
  }
}
