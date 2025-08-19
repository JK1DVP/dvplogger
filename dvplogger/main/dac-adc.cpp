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
#include "driver/i2s.h" //https://github.com/espressif/arduino-esp32/blob/master/tools/sdk/include/driver/driver/i2s.h
#include "driver/adc.h"
#include "dac-adc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "soc/rtc_io_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include "soc/rtc.h"
#include "decl.h"
#include "variables.h"
#include "display.h"


#include "driver/dac.h"

int clk_8m_div = 7;      // RTC 8M clock divider (division is by clk_8m_div+1, i.e. 0 means 8MHz frequency)
int frequency_step = 40;  // Frequency step for CW generator
int scale = 3;           // 50% of the full scale
int offset=0;              // leave it default / 0 = no any offset
int invert = 2;          // invert MSB to get sine waveform


void dac_cosine(dac_channel_t channel, int enable)
{
    // Enable tone generator common to both channels
    switch(channel) {
        case DAC_CHANNEL_1:
          if (enable) {
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL1_REG, SENS_SW_TONE_EN);
            // Enable / connect tone tone generator on / to this channel
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M);
            // Invert MSB, otherwise part of waveform will have inverted
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV1, 2, SENS_DAC_INV1_S);
          } else {
            CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M);

          }
            break;
        case DAC_CHANNEL_2:
          if (enable) {
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL1_REG, SENS_SW_TONE_EN);
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN2_M);
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV2, 2, SENS_DAC_INV2_S);

          } else {
            CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN2_M);
          }
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


void dac_frequency_set(int clk_8m_div, int frequency_step)
{
    REG_SET_FIELD(RTC_CNTL_CLK_CONF_REG, RTC_CNTL_CK8M_DIV_SEL, clk_8m_div);
    SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL1_REG, SENS_SW_FSTEP, frequency_step, SENS_SW_FSTEP_S);
}


void dac_scale_set(dac_channel_t channel, int scale)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_SCALE1, scale, SENS_DAC_SCALE1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_SCALE2, scale, SENS_DAC_SCALE2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


void dac_offset_set(dac_channel_t channel, int offset)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_DC1, offset, SENS_DAC_DC1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_DC2, offset, SENS_DAC_DC2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


void dac_invert_set(dac_channel_t channel, int invert)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV1, invert, SENS_DAC_INV1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV2, invert, SENS_DAC_INV2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}

void dactask(void* arg)
{
  float frequency;
    while(1){

        // frequency setting is common to both channels
      //        dac_frequency_set(clk_8m_div, frequency_step);

        /* Tune parameters of channel 2 only
         * to see and compare changes against channel 1
         */
      //        dac_scale_set(DAC_CHANNEL_2, scale);
      //        dac_offset_set(DAC_CHANNEL_2, offset);
      //        dac_invert_set(DAC_CHANNEL_2, invert);

        dac_scale_set(DAC_CHANNEL_1, 0);
        dac_offset_set(DAC_CHANNEL_1, offset);
        dac_invert_set(DAC_CHANNEL_1, invert);

        dac_frequency_set(7, 37);	
        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 7) * (float)37/ 65536;
	//	frequency_step = freq/8M*(1+clk_8m_div)*65536
	printf("frequency: %.0f Hz %d\n", frequency,RTC_FAST_CLK_FREQ_APPROX);
        printf("DAC2 scale: %d, offset %d, invert: %d\n", scale, offset, invert);
        vTaskDelay(2000/portTICK_PERIOD_MS);

        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 2) * (float) 49 / 65536;
	printf("frequency: %.0f Hz\n", frequency);	
        dac_frequency_set(2, 49);
        vTaskDelay(2000/portTICK_PERIOD_MS);
        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 2) * (float) 53 / 65536;
	printf("frequency: %.0f Hz\n", frequency);	
        dac_frequency_set(2, 53);
        vTaskDelay(2000/portTICK_PERIOD_MS);	
	dac_cosine(DAC_CHANNEL_1,0);
	//	dac_cosine(DAC_CHANNEL_2,0);
        vTaskDelay(2000/portTICK_PERIOD_MS);	
	dac_cosine(DAC_CHANNEL_1,1);
	//	dac_cosine(DAC_CHANNEL_2,1);
    }
}


#include "esp_adc_cal.h"



///////////////////////////////////////////////
// adc driver and i2s

// single ADC read routine
esp_adc_cal_characteristics_t adcChar;

void adc_setup(){
  //    Serial.begin(115200);
    // ADCを起動（ほかの部分で明示的にOFFにしてなければなくても大丈夫）
    //    adc_power_acquire();
    // ADC1_CH6を初期化
    //    adc_gpio_init(ADC_UNIT_1, ADC_CHANNEL_0);
    // ADC1の解像度を12bit（0~4095）に設定
    adc1_config_width(ADC_WIDTH_BIT_12);
    // ADC1の減衰を11dBに設定
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
    // 電圧値に変換するための情報をaddCharに格納
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChar);
}

void adc_process(){
    uint32_t voltage;
    // ADC1_CH6の電圧値を取得
    esp_adc_cal_get_voltage(ADC_CHANNEL_0, &adcChar, &voltage);
    Serial.println(String(voltage));
}


void adc_statistics() {
  // obain adc statistics
  uint32_t voltage_array[100];
  console->print("getting adc data");
  uint32_t t0,t1;

  for (int j=0;j<50;j++) {
    t0=millis();
    //    console->println(t0);
    for (int i=0;i<100;i++) {
      esp_adc_cal_get_voltage(ADC_CHANNEL_0, &adcChar, voltage_array+i);
      delayMicroseconds(500);
    }
    t1=millis();
    //  console->print("got 100 data in ");  console->print(t1-t0);  console->print("ms. ");
    float avg;
    avg=0;
    for (int i=0;i<100;i++) {
      avg=avg+voltage_array[i];
    }
    avg=avg/100;
    float rms,tmp;
    rms=0;
    for (int i=0;i<100;i++) {
      tmp=voltage_array[i]-avg;
      rms=rms+tmp*tmp;
    }
    rms=sqrt(rms);
    
    console->print(" data avg=");console->print(avg);console->print(" rms=");console->println(rms);    
    sprintf(dp->lcdbuf,"ADC N=%2d/50\n    AVG=%4.0f\n    RMS=%4.0f",j,avg,rms);
    upd_display_info_flash(dp->lcdbuf);

  }
}

#include "driver/timer.h"

#define TIMER_DIVIDER         (16)  // タイマーのクロック分周
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // タイマースケール
#define TIMER_INTERVAL_8KHZ   (1.0 / 22000)  // 8kHzで割り込み

void adc_timer_config(void) {
    timer_config_t config = {
        .alarm_en = TIMER_ALARM_EN,
        .counter_en = TIMER_PAUSE,
        .intr_type = TIMER_INTR_LEVEL,
        .counter_dir = TIMER_COUNT_UP,
        .auto_reload = TIMER_AUTORELOAD_EN,
        .divider = TIMER_DIVIDER
    };

    timer_init(TIMER_GROUP_0, TIMER_0, &config);
    timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0x00000000ULL);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, TIMER_INTERVAL_8KHZ * TIMER_SCALE);
    timer_enable_intr(TIMER_GROUP_0, TIMER_0);
    timer_start(TIMER_GROUP_0, TIMER_0);
}


#define PIN_MIC_IN 36
#define ADC_INPUT ADC1_CHANNEL_0 //pin 36


uint16_t adc_ringbuf[ADC_RINGBUF_NBANK][NUM_MEM_SECT]; // i2s read will store to this buffer in the background

volatile uint8_t adc_ridx_bank=0,adc_widx_bank=0;
uint16_t adc_ridx_sect=0; // in the ridx_bank where to start read the next sample.

volatile uint8_t f_adc_i2s_read=0; // flag single i2s read completed
volatile int n_adc_i2s_read=0; // how many read completed
long adc_i2s_read_time=0;long adc_i2s_read_dt=0;


TaskHandle_t gxHandle;

void i2sInit() //ref to samplecode HiFreq_ADC.ino
{
  // ADC1の解像度を12bit（0~4095）に設定
  //    adc1_config_width(ADC_WIDTH_BIT_12);
    // ADC1の減衰を11dBに設定
  //    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_12);
    // 電圧値に変換するための情報をaddCharに格納
  //    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChar);
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
      .sample_rate =  I2S_SAMPLE_RATE,              // The format of the signal using ADC_BUILT_IN
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // is fixed at 12bit, stereo, MSB
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      //      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // shows spike every transfer unit
      //.channel_format = I2S_CHANNEL_FMT_ALL_LEFT,
      //.communication_format = I2S_COMM_FORMAT_I2S_MSB,
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,  // 非推奨の設定を置き換え
      //    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB), 
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
    .dma_buf_count = DMA_BUF_COUNT, //num of Bytes, from 2 upto 128
    //    .dma_buf_count = 8,  // DMAバッファの個数
    //    .dma_buf_len = 64,   // DMAバッファの長さ
    .dma_buf_len = DMA_BUF_LEN, //num of sample not Bytes, from 8 upto 1024  //https://github.com/espressif/esp-idf/blob/master/components/driver/i2s.c#L922    
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
      .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,  // 新しいメンバーを初期化
     .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT  // 'bits_per_chan' の初期化  
  };

  // DMA buffer size = (bits_per_sample/8)*num_chan*dma_buf_count*dma_buf_len
  // (16/8)*2*dma_buf_count*dma_buf_len
  // (2)*2*2(count)*20(buf_len) ... 40 samples = 5ms data  buffer= 160 bytes
  
  //  adc1_config_channel_atten(ADC_INPUT, ADC_ATTEN_DB_12);
  //  adc1_config_width(ADC_WIDTH_BIT_12);

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    printf("Failed to install I2S driver: %s\n", esp_err_to_name(err));
    return;
  }


  err = i2s_set_adc_mode(ADC_UNIT_1, ADC_INPUT);

  i2s_adc_enable(I2S_NUM_0);
}

void taskI2sReading(void *arg){
  size_t uiGotLen=0;
  while(1){
      esp_err_t erReturns = i2s_read(I2S_NUM_0, (char *)adc_ringbuf[adc_widx_bank], NUM_MEM_SECT*sizeof(uint16_t), &uiGotLen, portMAX_DELAY);

      adc_widx_bank=(adc_widx_bank+1)%ADC_RINGBUF_NBANK;
      f_adc_i2s_read =1;
      n_adc_i2s_read++;
      long tmp;
      tmp=millis();
      adc_i2s_read_dt=tmp-adc_i2s_read_time;
      adc_i2s_read_time=tmp;

      // eval gotzel
      for (int i=0;i<NUNITS_TRANSFER;i++ ) {
	//	eval_goetzel_amplitude(&adc_ringbuf[adc_ridx_bank][NSAMP_EVAL_UNIT*i*2],NSAMP_EVAL_UNIT,2);
      }
      adc_ridx_bank=(adc_ridx_bank+1)%ADC_RINGBUF_NBANK;
  }
}


void i2s_setup() {

  pinMode(PIN_MIC_IN, INPUT);
  i2sInit();
  xTaskCreate(taskI2sReading, "taskI2sReading", 2048, NULL, 1, &gxHandle);

}

int adc_buffer_count()
{
  // return with number of samples readable in the buffer
  return 0;
}
// read count samples into the storage return 
int adc_read_i2s(uint16_t *storage,int ncount)
{
  int count=0;
  return count;
}


// read adc by i2s
void i2s_loop()
{
}

