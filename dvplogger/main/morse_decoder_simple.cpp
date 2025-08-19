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

#include "morse_decoder_simple.h"
#include "Arduino.h"
#include <cmath>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "variables.h"
#include "display.h"

Morse_decoder decoder;
void Morse_decoder::monitor_task()
{
  const char *TAGM = "CW_MON";
  static int timeout_monitor_task=0;
  static unsigned int count=0;

  bool    task_running = decoder.s_run;
  float   mag      = decoder.magnitude;
  float   mlimit   = decoder.magnitudelimit;
  int     rs       = decoder.realstate;
  int     fs       = decoder.filteredstate;
  long    havg     = decoder.hightimesavg;
  long    ldur     = decoder.lowduration;
  long    hdur     = decoder.highduration;
  int     wpm      = decoder.wpm;
  uint16_t fill    = decoder.goertzel_fill;
  bool    upper12  = decoder.s_adc_upper12;
  uint32_t drops   = decoder.s_drop;
  float fs_eff_show = fs_eff;
  long goertzel_counter_print = decoder.goertzel_counter;
  //  unsigned rb_cnt  = Morse_decoder::g_rms_rb.count();

  if (task_running) {
    if (timeout_monitor_task < millis()) {
      timeout_monitor_task=millis()+1000;
      ESP_LOGI(TAGM,
	       "mag=%5.0f lim=%5.0f state=%d/%d havg=%3ld ldur=%3ld hdur=%3ld wpm=%2d decode=%s",
	       mag, mlimit, rs, fs, havg, ldur, hdur, wpm,
	       decoder.decoded);
    }
    // fs_eff_show,
    //		 goertzel_counter_print,      
    // show decoded character  on lcd
    if (decoder.f_disp_update) {
      sprintf(dp->lcdbuf, "CW decoder\n%s\n",decoder.decoded);
      upd_display_info_flash(dp->lcdbuf);
      decoder.f_disp_update=0;
    }
  }

}

RingBuffer<Morse_decoder::RmsBlock> Morse_decoder::g_rms_rb(512);
SemaphoreHandle_t Morse_decoder::g_rb_mtx = nullptr;
TaskHandle_t      Morse_decoder::s_i2s_task = nullptr;
volatile bool     Morse_decoder::s_run = false;
volatile uint32_t Morse_decoder::s_drop = 0;
volatile float Morse_decoder::fs_eff = 0.0f;
int64_t Morse_decoder::timeout_ticker1=0;
int64_t Morse_decoder::timeout_ticker2=0;
int64_t Morse_decoder::timeout_ticker3=0;
int64_t Morse_decoder::timeout_ticker4=0;
bool    Morse_decoder::s_adc_upper12 = false;
uint8_t Morse_decoder::s_adc_seen    = 0;
uint16_t Morse_decoder::adc12_from_i2s(uint16_t s)
{
  return  (s & 0x0FFF);  
}

void Morse_decoder::setup() {
  ////////////////////////////////////
  // The basic goertzel calculation //
  ////////////////////////////////////
  int	k;
  float	omega;
  k = (int) (0.5 + ((GOERTZEL_N * target_freq) / sampling_freq));
  omega = (2.0 * PI * k) / ((float)GOERTZEL_N);
  sine = sin(omega);
  cosine = cos(omega);
  coeff = 2.0 * cosine;
  goertzel_unit_ms = (1000.0f*GOERTZEL_N)/sampling_freq;

  f_disp_update=0;

}



void Morse_decoder::put_code(char *s)
{
  int len;
  len=strlen(code);
  strcat(code,s);

}

void Morse_decoder::printascii(int c)
{
  char tmpstr[4];
  tmpstr[0]=c;
  tmpstr[1]='\0';
  
  strcat(decoded,tmpstr);
  truncate_tail_utf8(decoded,20);
  
  f_disp_update=1;
}

void Morse_decoder::docode(){
  if (strcmp(code,".-") == 0) printascii(65);
  if (strcmp(code,"-...") == 0) printascii(66);
  if (strcmp(code,"-.-.") == 0) printascii(67);
  if (strcmp(code,"-..") == 0) printascii(68);
  if (strcmp(code,".") == 0) printascii(69);
  if (strcmp(code,"..-.") == 0) printascii(70);
  if (strcmp(code,"--.") == 0) printascii(71);
  if (strcmp(code,"....") == 0) printascii(72);
  if (strcmp(code,"..") == 0) printascii(73);
  if (strcmp(code,".---") == 0) printascii(74);
  if (strcmp(code,"-.-") == 0) printascii(75);
  if (strcmp(code,".-..") == 0) printascii(76);
  if (strcmp(code,"--") == 0) printascii(77);
  if (strcmp(code,"-.") == 0) printascii(78);
  if (strcmp(code,"---") == 0) printascii(79);
  if (strcmp(code,".--.") == 0) printascii(80);
  if (strcmp(code,"--.-") == 0) printascii(81);
  if (strcmp(code,".-.") == 0) printascii(82);
  if (strcmp(code,"...") == 0) printascii(83);
  if (strcmp(code,"-") == 0) printascii(84);
  if (strcmp(code,"..-") == 0) printascii(85);
  if (strcmp(code,"...-") == 0) printascii(86);
  if (strcmp(code,".--") == 0) printascii(87);
  if (strcmp(code,"-..-") == 0) printascii(88);
  if (strcmp(code,"-.--") == 0) printascii(89);
  if (strcmp(code,"--..") == 0) printascii(90);

  if (strcmp(code,".----") == 0) printascii(49);
  if (strcmp(code,"..---") == 0) printascii(50);
  if (strcmp(code,"...--") == 0) printascii(51);
  if (strcmp(code,"....-") == 0) printascii(52);
  if (strcmp(code,".....") == 0) printascii(53);
  if (strcmp(code,"-....") == 0) printascii(54);
  if (strcmp(code,"--...") == 0) printascii(55);
  if (strcmp(code,"---..") == 0) printascii(56);
  if (strcmp(code,"----.") == 0) printascii(57);
  if (strcmp(code,"-----") == 0) printascii(48);

  if (strcmp(code,"..--..") == 0) printascii(63);
  if (strcmp(code,".-.-.-") == 0) printascii(46);
  if (strcmp(code,"--..--") == 0) printascii(44);
  if (strcmp(code,"-.-.--") == 0) printascii(33);
  if (strcmp(code,".--.-.") == 0) printascii(64);
  if (strcmp(code,"---...") == 0) printascii(58);
  if (strcmp(code,"-....-") == 0) printascii(45);
  if (strcmp(code,"-..-.") == 0) printascii(47);

  if (strcmp(code,"-.--.") == 0) printascii(40);
  if (strcmp(code,"-.--.-") == 0) printascii(41);
  if (strcmp(code,".-...") == 0) printascii(95);
  if (strcmp(code,"...-..-") == 0) printascii(36);
  if (strcmp(code,"...-.-") == 0) printascii(62);
  if (strcmp(code,".-.-.") == 0) printascii(60);
  if (strcmp(code,"...-.") == 0) printascii(126);

}




static const char *TAG = "I2S_ADC_24k_RMS";


#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "cat.h"
#include "cw_keying.h"
#include "iambic_keyer.h"
#include "timekeep.h"

// UTF-8 の1バイトが「先頭バイト」かどうか判定
bool Morse_decoder::is_utf8_lead_byte(unsigned char c) {
    return (c & 0xC0) != 0x80; // 10xxxxxx でない = 先頭バイト
}

// 末尾の keep_chars 文字を残す
void Morse_decoder::truncate_tail_utf8(char *str, size_t keep_chars) {
    size_t len = strlen(str);
    if (keep_chars == 0) {
        str[0] = '\0';
        return;
    }

    // 末尾から UTF-8 文字数を数える
    size_t chars = 0;
    size_t pos = len;  // '\0' の位置
    while (pos > 0) {
        pos--;
        if (is_utf8_lead_byte((unsigned char)str[pos])) {
            chars++;
            if (chars == keep_chars) {
                // この位置から文字列を残す
                memmove(str, str + pos, len - pos + 1); // +1で終端もコピー
                return;
            }
        }
    }

    // 文字数が keep_chars 未満ならそのまま
}


bool Morse_decoder::rb_push(const RmsBlock &blk) {
  bool ok = false;
  if (xSemaphoreTake(g_rb_mtx, portMAX_DELAY) == pdTRUE) {
    ok = g_rms_rb.push(blk);
    xSemaphoreGive(g_rb_mtx);
  }
  if (!ok) s_drop++;
  return ok;
}

bool Morse_decoder::pop_rms_block(RmsBlock &out) {
  bool ok = false;
  if (xSemaphoreTake(g_rb_mtx, 0) == pdTRUE) {
    ok = g_rms_rb.pop(out);
    xSemaphoreGive(g_rb_mtx);
  }
  return ok;
}


void Morse_decoder::i2s_adc_task_i2sread()
{
  static uint16_t buf[DMA_BUF_LEN];
  static size_t   br = 0;
  static uint64_t t0 = esp_timer_get_time();
  static uint32_t nsamp_acc = 0;
  
  esp_err_t err = i2s_read(I2S_PORT, buf, sizeof(buf), &br, 0); // 非ブロッキング
  
  if (err == ESP_OK && br > 0) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      for (int i = 0; i < 16 && i < sizeof(buf); ++i) {
	ESP_LOGI(TAG, "raw16[%02d]=0x%04X", i, buf[i]);
      }
    }

    // ブロック処理部の置き換え
    const uint16_t n = (uint16_t)(br / sizeof(uint16_t));
    nsamp_acc += n;
    decoder.feed_i2s_block_for_goertzel(buf, n);  // ★ インスタンス経由で呼ぶ
    uint64_t dt = esp_timer_get_time() - t0;
    if (dt >= 1000000ULL) {
      fs_eff = (float)nsamp_acc * 1e6f / (float)dt;	      
      t0 = esp_timer_get_time();
      nsamp_acc = 0;
    }
  } else {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void Morse_decoder::i2s_adc_rms_task_adcinit()
{
  // ADC設定
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_CH, ADC_ATTEN_SET);

  // I2S設定
  i2s_config_t i2s_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
    .sample_rate = FS_TARGET_HZ,                 // 後で set_clk で確定
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // MONO
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN,
    .use_apll = false                   
  };
  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_cfg, 0, NULL));

  ESP_ERROR_CHECK(i2s_set_adc_mode(ADC_UNIT_USE, ADC_CH));
  ESP_ERROR_CHECK(i2s_set_clk(I2S_PORT, FS_TARGET_HZ,
			      I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO));
  ESP_ERROR_CHECK(i2s_adc_enable(I2S_PORT));

  ESP_LOGI(TAG, "I2S-ADC started (%.1f kHz), buf=%d x %d",
	   FS_TARGET_HZ/1000.0, DMA_BUF_COUNT, DMA_BUF_LEN);
  
}

void Morse_decoder::i2s_adc_rms_task(void *arg)
{


  i2s_adc_rms_task_adcinit();
  
  s_run = true;
  while (s_run) {
    i2s_adc_task_i2sread();
    /*
      main/cat.cpp:  civ_reader.attach_ms(1, receive_civport_1);
      main/cw_keying.cpp:  cw_sender.attach_ms(1, interrupt_cw_send);
      main/iambic_keyer.cpp:  iambic_keyer.attach_ms(1, iambic_keyer_handler);
      main/timekeep.cpp:  my_rtc_ticker.attach_ms(100,interrupt_my_rtc);
    */
    // ticker service

    int64_t t1=esp_timer_get_time();   // マイクロ秒単位で返す (64bit)
    /*
    if (timeout_ticker1<=t1) {
      timeout_ticker1=t1+1000;
      receive_civport_1();
    }
    */
    if (timeout_ticker2<=t1) {
      timeout_ticker2=t1+1000;
      interrupt_cw_send();
    }
    /*
    if (timeout_ticker3<=t1) {
      timeout_ticker3=t1+1000;
      iambic_keyer_handler();
    }
    */
    if (timeout_ticker4<=t1) {
      timeout_ticker4=t1+100000;
      interrupt_my_rtc();
    }
  }

  // 後始末
  ESP_ERROR_CHECK(i2s_adc_disable(I2S_PORT));
  i2s_driver_uninstall(I2S_PORT);
  ESP_LOGI(TAG, "I2S-ADC task stopped.");
  s_i2s_task = nullptr;
  vTaskDelete(NULL);
}

void Morse_decoder::morse_decode_task() // called in main loop
{
  if (!g_rb_mtx) return;
    RmsBlock blk;
    while (decoder.pop_rms_block(blk)) {
      decoder.process_magnitude(blk.mag);
    }
}
void Morse_decoder::process_magnitude(float magnitude_in)
{
  magnitude = magnitude_in; // 既存メンバを更新（ログなどで使う想定）
  goertzel_counter++;
  
  // --- しきい値の自動追従（移動平均）---
  if (magnitude > magnitudelimit_low) {
    magnitudelimit = (magnitudelimit + ((magnitude - magnitudelimit)/6));
  }
  if (magnitudelimit < magnitudelimit_low) magnitudelimit = magnitudelimit_low;

  // --- on/off 実測状態 ---
  realstate = (magnitude > magnitudelimit * 0.6f) ? 1 : 0;

  // --- ノイズブランカ ---
  if (realstate != realstatebefore) {
    laststarttime = goertzel_counter;
  }
  if ((goertzel_counter - laststarttime) > nbtime/goertzel_unit_ms) { // nbtime in ms  goertzel_counter 24000/80
    if (realstate != filteredstate) {
      filteredstate = realstate;
    }
  }

  // --- high/low 長さ更新 ---
  if (filteredstate != filteredstatebefore) {
    if (filteredstate == 1) { // 上がった：low終了
      starttimehigh = goertzel_counter;
      lowduration   = (goertzel_counter - startttimelow);
    }
    if (filteredstate == 0) { // 下がった：high終了
      startttimelow = goertzel_counter;
      highduration  = (goertzel_counter - starttimehigh);
      if (highduration < (2*hightimesavg) || hightimesavg == 0) {
	hightimesavg = (highduration + hightimesavg + hightimesavg)/3;
      }
      if (highduration > (5*hightimesavg)) {
	hightimesavg = highduration + hightimesavg; // if speed decrease fast
      }
    }
  }

  // --- dit/dah & スペース判定 ---
  if (filteredstate != filteredstatebefore) {
    stop = 0;
    if (filteredstate == 0) { // 1が終わった＝トーン終端
      if (highduration < (hightimesavg*2) && highduration > (hightimesavg*0.6)) {
	if (strlen(code) < sizeof(code)-2) {
	  put_code(".");
	}
      }
      if (highduration > (hightimesavg*2) && highduration < (hightimesavg*6)) {
	if (strlen(code) < sizeof(code)-2) {
	  put_code("-");

	}
	wpm =(wpm+(1200.0f /((highduration*goertzel_unit_ms)/3.0f)))/2;	
      }
    }
    if (filteredstate == 1) { // 0が終わった＝無音区間終端
      float lacktime = 1.0f;
      if (wpm > 25) lacktime = 1.0f;
      if (wpm > 30) lacktime = 1.2f;
      if (wpm > 35) lacktime = 1.5f;

      if (lowduration > (hightimesavg*(2*lacktime)) && lowduration < hightimesavg*(5*lacktime)) {
	docode(); code[0] = '\0';
      }
      if (lowduration >= (hightimesavg*(5*lacktime))) {
	docode(); code[0] = '\0';
	printascii(' '); 
      }
    }
  }

  if ((goertzel_counter - startttimelow) > (highduration * 6) && stop == 0) {
    docode(); code[0] = '\0'; 
    printascii(' ');
    stop = 1;    
  }

  realstatebefore      = realstate;
  lasthighduration     = highduration;
  filteredstatebefore  = filteredstate;
}

void Morse_decoder::feed_i2s_block_for_goertzel(const uint16_t* buf16, uint16_t n)
{
  for (uint16_t i=0; i<n; i+=GOERTZEL_DECIM) {
    float v = (float)adc12_from_i2s(buf16[i]) - adcmean;
    goertzel_in[goertzel_fill++] = v;
    if (goertzel_fill >= GOERTZEL_N) {
      float _Q1 = 0.0f, _Q2 = 0.0f;
      for (uint16_t k=0; k<GOERTZEL_N; ++k) {
	float Q0 = coeff * _Q1 - _Q2 + goertzel_in[k];
	_Q2 = _Q1;
	_Q1 = Q0;
      }
      float mag2 = (_Q1*_Q1) + (_Q2*_Q2) - _Q1*_Q2*coeff;
      float mag  = (mag2 > 0.0f) ? sqrtf(mag2) : 0.0f;

      RmsBlock blk{};
      blk.mag=mag;
      rb_push(blk);

      goertzel_fill = 0;
    }
  }
}


void Morse_decoder::start_i2s_adc_24k_rms_task()
{
  if (!g_rb_mtx) g_rb_mtx = xSemaphoreCreateMutex();
  if (!s_i2s_task) {
    xTaskCreatePinnedToCore(i2s_adc_rms_task, "i2s_adc_24k_rms",
			    TASK_STACK, nullptr, TASK_PRIO, &s_i2s_task, TASK_CORE);
  }
}
void Morse_decoder::stop_i2s_adc_24k_rms_task()
{
  if (!s_i2s_task) return;
  s_run = false;
  for (int i=0; i<100 && s_i2s_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}


void Morse_decoder::init_morse_decoder()
{

}
