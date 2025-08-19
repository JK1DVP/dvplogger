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

#ifndef FILE_DAC_ADC_H
#define FILE_DAC_ADC_H
#include "driver/dac.h"
void dac_cosine(dac_channel_t channel, int enable);
void dac_frequency_set(int clk_8m_div, int frequency_step);
void dac_scale_set(dac_channel_t channel, int scale);
void dac_offset_set(dac_channel_t channel, int offset);
void dac_invert_set(dac_channel_t channel, int invert);
void i2s_setup() ;
void adc_setup();
void adc_statistics();
void adc_process();
void adc_timer_config(void) ;

// definitions related to ADC and I2S
#define I2S_SAMPLE_RATE 22000 // success
//#define I2S_SAMPLE_RATE 16000  // fail
//#define I2S_SAMPLE_RATE 44100
#define EVAL_UNIT_MS 5
#define NSAMP_EVAL_UNIT ((I2S_SAMPLE_RATE*EVAL_UNIT_MS)/1000) // unit of amplitude evaluation = 110
#define NUNITS_TRANSFER 4
#define NSAMP_I2S_TRANSFER (NSAMP_EVAL_UNIT*NUNITS_TRANSFER) // i2s DMA transfer number of samples  = 440
#define ADC_RINGBUF_NBANK (2)  // bank of DMA transferred data ringbuf adc_ringbuf[][]
#define DMA_BUF_COUNT 8 // I2S DMA parameter
#define DMA_BUF_LEN (NSAMP_I2S_TRANSFER/DMA_BUF_COUNT)  // another I2S DMA paramter to transfer NSAMP_I2S_TRANSFER = 55
#define NUM_MEM_SECT (2*DMA_BUF_COUNT*DMA_BUF_LEN) // number of data counts for the i2S DMA transfer block = 4*8*55=1760
extern volatile int n_adc_i2s_read; // how many read completed
extern uint16_t adc_ringbuf[ADC_RINGBUF_NBANK][NUM_MEM_SECT]; // i2s read will store to this buffer in the background
extern volatile uint8_t adc_ridx_bank,adc_widx_bank;
extern long adc_i2s_read_time,adc_i2s_read_dt;
#endif
