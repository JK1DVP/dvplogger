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
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioPlayer.h"
#include <stdlib.h>
#include <string>
#include "RingBuffer.h"
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <driver/i2s.h> 
#include "soc/i2s_struct.h"
#include "soc/i2s_reg.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif



#define ESP_LOGI(tag, format, ...)  ((void)0)

size_t AudioPlayer::utf16_to_utf8(uint16_t code, char *utf8) {
    if (code < 0x80) {
        utf8[0] = code;
        return 1;
    } else if (code < 0x800) {
        utf8[0] = 0xC0 | (code >> 6);
        utf8[1] = 0x80 | (code & 0x3F);
        return 2;
    } else {
        utf8[0] = 0xE0 | (code >> 12);
        utf8[1] = 0x80 | ((code >> 6) & 0x3F);
        utf8[2] = 0x80 | (code & 0x3F);
        return 3;
    }
}

// UTF-8 → UTF-16 (簡易版、最大len文字)
size_t utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t max_chars) {
  size_t i = 0, j = 0;
  while (utf8[i] && j < max_chars) {
    uint8_t c = utf8[i];
    if (c < 0x80) {
      utf16[j++] = c;
      i++;
    } else if ((c & 0xE0) == 0xC0) {
      utf16[j++] = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      uint16_t u = ((c & 0x0F) << 12) |
	((utf8[i + 1] & 0x3F) << 6) |
	(utf8[i + 2] & 0x3F);
      
      // 全角括弧への置換
      if (u == 0x005B) u = 0xFF3B;
      else if (u == 0x005D) u = 0xFF3D;
      
      utf16[j++] = u;

      i += 3;
    } else {
      // 4バイトUTF-8はサポート外
      i++;
    }
    ESP_LOGI(AudioPlayer::TAG, "utf8_to_utf16: %s → U+%04X", utf8 + i, utf16[j - 1]);
  }

  return j; // 変換したUTF-16文字数
}


#ifdef USE_SD

bool AudioPlayer::open_file(const char* path) {
  std::string fullpath = path;
  if (fullpath[0] != '/') fullpath = "/" + fullpath;  // ←先頭に / が無ければ付ける
  if (file_) file_.close();
  file_ = SD.open(fullpath.c_str(), FILE_READ);
  if (!file_) {
    ESP_LOGE(TAG, "SD.open failed: %s", fullpath.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Opened file (SD): %s", fullpath.c_str());
  return true;
}

#else

bool AudioPlayer::open_file(const char* path) {
  // SPIFFSは /spiffs/ を先頭に付ける
  std::string full_path = std::string("/spiffs/") + path;
  if (fp_) fclose(fp_);
  fp_ = fopen(full_path.c_str(), "rb");
  if (!fp_) {
    ESP_LOGE(TAG, "fopen failed: %s", full_path.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Opened file (SPIFFS): %s", full_path.c_str());
  return true;
}
#endif

#ifdef USE_SD
//bool AudioPlayer::open_file(const char* path) {
//    file_ = SD.open(path, FILE_READ);
//    return file_;
//}

void AudioPlayer::close_file() {
  if (file_) file_.close();
}


bool AudioPlayer::seek_file(long offset, int whence) {
  if (whence == SEEK_SET)
    return file_.seek(offset, SeekSet);
  if (whence == SEEK_CUR)
    return file_.seek(file_.position() + offset);
  if (whence == SEEK_END)
    return file_.seek(file_.size() + offset);
  return false;
}
#else
//bool AudioPlayer::open_file(const char* path) {
//    fp_ = fopen(path, "rb");
//    return fp_ != nullptr;
//}

void AudioPlayer::close_file() {
  if (fp_) fclose(fp_);
  fp_ = nullptr;
}


bool AudioPlayer::seek_file(long offset, int whence) {
  return fseek(fp_, offset, whence) == 0;
}
#endif

size_t AudioPlayer::read_file(void* buf, size_t size, size_t count) {
#ifdef USE_SD
  if (!file_) return 0;
  size_t total_bytes = size * count;
  size_t read_bytes = file_.read((uint8_t*)buf, total_bytes);
  size_t read_elems = read_bytes / size; // 要素数に変換
  //    ESP_LOGI(TAG, "SD.read: requested=%u bytes, read=%u bytes (%u elems)", total_bytes, read_bytes, read_elems);
  return read_elems;
#else
  if (!fp_) return 0;
  size_t read_elems = fread(buf, size, count, fp_);
  //    ESP_LOGI(TAG, "fread: size=%u count=%u => read=%u elems", size, count, read_elems);
  return read_elems;
#endif
}

int AudioPlayer::read_char() {
#ifdef USE_SD
  if (!file_) return -1;             // file_未オープン
  int c = file_.read();              // SD.h File::read() は1バイト読んでint返す（-1=EOF）
  return (c < 0) ? -1 : c;
#else
  if (!fp_) return -1;               // fp_未オープン
  int c = fgetc(fp_);                // freadの1バイト版
  return (c == EOF) ? -1 : c;
#endif
}


bool AudioPlayer::is_file_open() {
#ifdef USE_SD
  return file_;
#else
  return fp_ != nullptr;
#endif
}


AudioPlayer::AudioPlayer(const Params& p) : dac_ofs_(32768), fifo(300),  dot_buf(nullptr), dash_buf(nullptr), silence_buf(nullptr), wpm(20),default_wpm_(20),params_(p)
{
  ESP_LOGI(TAG, "AudioPlayer constructor");
  set_wpm(wpm);
  fifo_was_empty_ = true;  // 初期状態は空
  q_ = xQueueCreate(4, sizeof(int));
  terminate_task_ = false;
}

AudioPlayer::~AudioPlayer() {
  ESP_LOGI(TAG, "AudioPlayer destructor");
  terminate_task_ = true;
  if (th_) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    th_ = nullptr;
  }

  if (callback_task_handle_) {
    vTaskDelete(callback_task_handle_);
    callback_task_handle_ = nullptr;
  }    
  if (q_) {
    vQueueDelete(q_);
    q_ = nullptr;
  }
  delete[] dot_buf;
  delete[] dash_buf;
  delete[] silence_buf;
  close_file();
}


bool AudioPlayer::read_line(char *buf, size_t len) {
  size_t idx = 0;
  int ch;

  while (idx < len - 1) {
    ch = read_char();
    if (ch < 0) break;           // EOF
    if (ch == '\r') continue;    // CR無視（CRLF対応）
    if (ch == '\n') break;       // LFで行終わり

    buf[idx++] = static_cast<char>(ch);
  }

  buf[idx] = '\0';
  return idx > 0;                  // 1文字以上読めたか
}

void AudioPlayer::set_default_wpm(int new_wpm) {
    ESP_LOGI(TAG, "set_default_wpm(): default WPM set to %d", new_wpm);
    default_wpm_ = new_wpm; // 新メンバ変数
}

void AudioPlayer::set_wpm(int new_wpm) {
  wpm = new_wpm;
  ESP_LOGI(TAG, "WPM set to %d", wpm);

  delete[] dot_buf;
  delete[] dash_buf;
  delete[] silence_buf;

  dot_samples = params_.rate * unit_time() / 1000;
  dash_samples = dot_samples * 3;
  silence_samples = dot_samples;

  dot_buf = new int16_t[dot_samples];
  dash_buf = new int16_t[dash_samples];
  silence_buf = new int16_t[silence_samples];

  generate_tone_buffer(dot_buf, dot_samples, TONE_FREQ);
  generate_tone_buffer(dash_buf, dash_samples, TONE_FREQ);
  generate_silence_buffer(silence_buf, silence_samples);
}

void AudioPlayer::generate_tone_buffer(int16_t *buf, int samples, float freq) {
    int taper_samples = params_.rate * 0.005; // 5ms taper
    if (taper_samples * 2 > samples) taper_samples = samples / 2;

    for (int i = 0; i < samples; ++i) {
        float env = 1.0f;

        if (i < taper_samples) {
            env = (float)i / taper_samples; // Fade in
        } else if (i >= samples - taper_samples) {
            env = (float)(samples - i) / taper_samples; // Fade out
        }

        float sample = sinf(2 * M_PI * freq * i / params_.rate);
        // フルスケール：16bit DAC用に ±32767
        buf[i] = (int16_t)(32760.0f * sample * env);
    }
}


void AudioPlayer::generate_silence_buffer(int16_t *buf, int samples) {
  std::fill_n(buf, samples, 0); 
}


esp_err_t AudioPlayer::begin() {
  ESP_LOGI(TAG, "AudioPlayer begin()");
  ESP_ERROR_CHECK(setupI2S(params_.rate));
  xTaskCreate(task, "audio_task", 4096, this, 5, &th_);
  ESP_LOGI(TAG, "AudioPlayer task created");


  xTaskCreatePinnedToCore(
			  AudioPlayer::callback_task,
			  "AudioPlayerCallbackTask",
			  2048,        // スタックサイズ
			  this,        // 引数: AudioPlayer インスタンス
			  5,           // 優先度（I2Sより低ければOK）
			  &callback_task_handle_,
			  1            // CPUコア (I2Sタスクと別ならなお安心)
			  );    
  return ESP_OK;
}

void AudioPlayer::stop() {
  ESP_LOGI(TAG, "AudioPlayer stop()");
  if (!th_ || !playing_) return;
  int cmd = static_cast<int>(Cmd::STOP);
  xQueueSend(q_, &cmd, portMAX_DELAY);
}

bool AudioPlayer::seek(uint32_t s, uint32_t l) {
  ESP_LOGI(TAG, "AudioPlayer seek(): start=%u length=%u", s, l);
  if (!th_) return false;
  Range r{s, l};
  int cmd = static_cast<int>(Cmd::SET_RANGE);
  if (xQueueSend(q_, &cmd, 0) != pdPASS) return false;
  xQueueSend(q_, &r, 0);
  return true;
}

bool AudioPlayer::seek_sec(float startSec, float lengthSec) {
  uint32_t s = static_cast<uint32_t>(startSec * params_.rate + 0.5f);
  uint32_t l = static_cast<uint32_t>(lengthSec * params_.rate + 0.5f);
  return seek(s, l);
}

bool AudioPlayer::play_morse(const char *s) {
    uint16_t utf16_buf[128];
    size_t num_chars = utf8_to_utf16(s, utf16_buf, 128);
    int push_wpm = default_wpm_; // 呼び出し時のWPM固定
    
    for (size_t i = 0; i < num_chars; ++i) {
        FifoEntry entry;
        entry.symbol = (utf16_buf[i] <= 0x007F)
                        ? utf16_buf[i] | 0x8000
                        : utf16_buf[i];
	//        entry.wpm = this->wpm;
	entry.wpm = push_wpm;
        while (!fifo.push(entry)) {
            ESP_LOGW(TAG, "FIFO full, waiting to push U+%04X", utf16_buf[i]);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return true;
}

bool AudioPlayer::play_string(const char *s) {
    if (!s) return false;
    fifo_was_empty_ = false;
    while (*s) {
        FifoEntry entry;
        entry.symbol = *s;
        entry.wpm = this->wpm;
        while (!fifo.push(entry)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s++;
    }
    return true;
}

void AudioPlayer::set_on_empty_callback(std::function<void()> cb) {
  on_empty_callback_ = cb;
}

void AudioPlayer::callback_task(void *param) {
    auto *player = static_cast<AudioPlayer *>(param);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 通知が来るまで待機


	int delay_ms = player->i2s_buffer_delay_ms();
	if (delay_ms > 0) {
	  vTaskDelay(pdMS_TO_TICKS(delay_ms/2));
	}
	
        // コールバック実行
        if (player->on_empty_callback_) {
            player->on_empty_callback_();
        }
    }
}


bool AudioPlayer::clear_string_fifo() {
  if (!th_) return false;
  //  int cmd = static_cast<int>(Cmd::CLEAR_FIFO);
  //  xQueueSend(q_, &cmd, 0);
  //  int stop_cmd = static_cast<int>(Cmd::STOP);
  //  xQueueSend(q_, &stop_cmd, 0);
  fifo.clear(); // just clear the buffer  
  playing_=false;
  silence_mode=true;
  silence_remain = static_cast<size_t>(params_.rate * 0.01f);
  return true;
}

int AudioPlayer::get_string_fifo(char *buf, size_t max_len, int flags) {
    size_t fifo_count = fifo.count();
    size_t i = 0;

    for (size_t idx = 0; idx < fifo_count && i < max_len - 1; ++idx) {
        FifoEntry entry;
      
	//        uint16_t value;
	//        if (!fifo.peek(value, idx)) break;
	if (!fifo.peek(entry, idx)) break;	

        // 欧文モールス
        if (entry.symbol & 0x8000) {
            if (flags & FIFO_MORSE_ONLY) {
                char c = static_cast<char>(entry.symbol & 0x7F);
                if (i + 1 < max_len - 1) {
                    buf[i++] = c;
                }
            }
            continue;
        }

        // 和文モールス (カタカナ/全角記号)
        if ((entry.symbol >= 0x30A0 && entry.symbol <= 0x30FF) || entry.symbol == 0xFF3B || entry.symbol == 0xFF3D) {
            if (flags & FIFO_MORSE_ONLY) {
                if (i + 3 < max_len - 1) {
                    buf[i++] = 0xE3; // UTF-8 3バイト目頭
                    buf[i++] = 0x82 + ((entry.symbol - 0x30A0) >> 6);
                    buf[i++] = 0xA0 + ((entry.symbol - 0x30A0) & 0x3F);
                }
            }
            continue;
        }

        // 音声ラベル (ASCII)
        if (flags & FIFO_TEXT_ONLY) {
            char c = static_cast<char>(entry.symbol & 0xFF);
            if (i + 1 < max_len - 1) {
                buf[i++] = c;
            }
        }
    }

    buf[i] = '\0'; // 終端
    return static_cast<int>(i);
}


bool AudioPlayer::load_labels(const char* path) {
  ESP_LOGI(TAG, "AudioPlayer load_labels(): %s", path);
  if (!open_file(path)) return false;

  char line[128];

  size_t count = 0;

  while (read_line(line, sizeof(line))) {
    double start_sec, end_sec;
    char c;
    char labelbuf[10] ={0};    
    if (sscanf(line, "%lf %lf %9s", &start_sec, &end_sec, labelbuf) == 3) {
      sound_labels[count].start = static_cast<int32_t>(start_sec * params_.rate + 0.5f);
      sound_labels[count].length = static_cast<int32_t>((end_sec - start_sec) * params_.rate + 0.5f);
      strncpy(sound_labels[count].labelstr,labelbuf,sizeof(sound_labels[count].labelstr)-1);
      sound_labels[count].labelstr[ sizeof(sound_labels[count].labelstr) - 1 ] = '\0';
      ESP_LOGI(TAG, "Label: %s start=%d length=%d", sound_labels[count].labelstr, sound_labels[count].start, sound_labels[count].length);
      count++;
    }
  }
  close_file();
  num_labels = count;
  ESP_LOGI(TAG, "Total labels loaded: %zu", num_labels);
  return true;
}

bool AudioPlayer::find_label(char *s, uint32_t& start, uint32_t& length) {
  for (size_t i = 0; i < num_labels; ++i) {
    if (strcmp(sound_labels[i].labelstr,s)==0) {
      start = sound_labels[i].start;
      length = sound_labels[i].length;
      return true;
    }
  }
  return false;
}

int16_t AudioPlayer::inbuf_[AudioPlayer::BUF_SIZE];
uint16_t AudioPlayer::outbuf_[AudioPlayer::OUTBUF_SIZE];

bool AudioPlayer::set_file(const char* path) {
  ESP_LOGI(TAG, "set_file(): Changing to %s", path);
  taskENTER_CRITICAL(&lock_);
  params_.path = path;
  close_file();
  bool ok = open_file(path);
  taskEXIT_CRITICAL(&lock_);
  return ok;
}

void AudioPlayer::interpolate_and_write_4x(int16_t* inbuf, size_t inlen) {
  //  static uint16_t outbuf[BUF_SIZE * 4]; // 4倍補間の出力バッファ
  for (size_t offset = 0; offset < inlen; offset += CHUNK_SIZE) {
    size_t chunk_len = (offset + CHUNK_SIZE <= inlen) ? CHUNK_SIZE : (inlen - offset);

    for (size_t j = 0; j < chunk_len; ++j) {
      int a = prev_data, b = inbuf[offset + j];
      int dv = (b - a) >> 2;
      prev_data = b;

      a += dv;      outbuf_[j * 4 + 1] = clip(a+dac_ofs_,0,65535);
      a += dv;      outbuf_[j * 4 + 0] = clip(a+dac_ofs_,0,65535);
      a += dv;      outbuf_[j * 4 + 3] = clip(a+dac_ofs_,0,65535);
      a += dv;      outbuf_[j * 4 + 2] = clip(a+dac_ofs_,0,65535);
    }

    size_t written;
    i2s_write(I2S_NUM_0, outbuf_, chunk_len * 4 * sizeof(uint16_t), &written, portMAX_DELAY);
  }

}

int AudioPlayer::i2s_buffer_delay_ms() const {
    size_t total_samples = dma_buf_len * dma_buf_count;
    size_t rate = params_.rate * 4; // 4倍補間後のレート
    return (total_samples * 1000) / rate; // ms
}

esp_err_t AudioPlayer::setupI2S(uint32_t sample_rate) {
  dma_buf_len = 1024;     // ←ここを保存
  dma_buf_count = 8;      // ←ここも保存
  i2s_config_t cfg{
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = sample_rate * 4,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    //        .communication_format = I2S_COMM_FORMAT_I2S_LSB,
    .communication_format =	static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_I2S |  I2S_COMM_FORMAT_I2S_LSB),
    .intr_alloc_flags = 0,
    .dma_buf_count = dma_buf_count,
    .dma_buf_len = dma_buf_len,
    //    .dma_buf_count = 8,
    //    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
    .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT, // ←追加
    .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT  // ←追加
	
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  return i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);
}

void AudioPlayer::task(void *arg) {
  ESP_LOGI(TAG, "AudioPlayer task started");
  auto* self = static_cast<AudioPlayer*>(arg);
  constexpr size_t BYTES_PER_SAMPLE = 2;
    
  uint16_t c;

  //  bool silence_mode = false;
  //  size_t silence_remain = 0;
  FifoEntry entry;
  while (!self->terminate_task_) {
    if (!self->is_file_open()) {
      if (!self->open_file(self->params_.path)) {
	ESP_LOGE(TAG, "Failed to open file: %s", self->params_.path);	  
	vTaskDelay(pdMS_TO_TICKS(500));
	continue;
      }
    }
    if (!self->playing_ && !self->silence_mode) {

      if (self->fifo.peek(entry)) {
	self->fifo_was_empty_ = false;
	// PUSH時WPMで波形生成
	if (entry.wpm != self->wpm) {
	  ESP_LOGI(TAG, "WPM changed: %d -> %d (for symbol U+%04X)", self->wpm, entry.wpm, entry.symbol);
	  self->set_wpm(entry.wpm);
	}

	if ((entry.symbol & 0x7FFF) == ' ') {
	  self->silence_mode = true;

	  if (entry.symbol & 0x8000 || (entry.symbol >= 0x30A0 && entry.symbol <= 0x30FF)) {
	    // モールス再生の場合：単語間7単位無音
	    self->silence_remain = static_cast<size_t>(
						 self->params_.rate * self->unit_time() * 3 / 1000.0f
						 );
	  } else {
	    // 音声再生の場合：0.1秒無音
	    self->silence_remain = static_cast<size_t>(
						 self->params_.rate * 0.1f
						 );
	  }

	  self->fifo.pop(entry);
	  vTaskDelay(pdMS_TO_TICKS(1));
	  continue;
	}

	if (entry.symbol & 0x8000) {
	  ESP_LOGI(TAG, "Detected 欧文モールス: %c", entry.symbol & 0x7F);
	  char ascii = entry.symbol & 0x7F;
	  self->play_morse_char(ascii);
	  self->fifo.pop(entry);
	  continue;
	} else if (entry.symbol >= 0x30A0 && entry.symbol <= 0x30FF) {
	  ESP_LOGI(TAG, "Detected 和文モールス: U+%04X", entry.symbol);
	  self->play_wabun_morse_char(entry.symbol);
	  self->fifo.pop(entry);
	  continue;
	} else if (entry.symbol == 0xFF3B || entry.symbol == 0xFF3D) {
	  ESP_LOGI(TAG, "Detected 和文プロシグナル: U+%04X", entry.symbol);
	  self->play_wabun_morse_char(entry.symbol);
	  self->fifo.pop(entry);
	  continue;
	} else {
	  ESP_LOGI(TAG, "Detected 音声ラベル: %c", entry.symbol);
	  // 音声ラベル (この部分はあなたの find_label ロジックを使う)
	  //	  ESP_LOGI(TAG, "Playing voice char '%c'", static_cast<char>(c));
	  //	  self->fifo.pop(c);
	  uint32_t start, length;
	  char labelstr[10];
	  labelstr[0]=entry.symbol;
	  labelstr[1]='\0';
	  if (entry.symbol=='_') {
	    self->fifo.pop(entry);
	    if (self->fifo.peek(entry)) {
	      self->fifo_was_empty_ = false;
	      labelstr[1]=entry.symbol;
	      labelstr[2]='\0';
	    } 
	  }
	  if (self->find_label(labelstr, start, length)) {
	    //	      ESP_LOGI(TAG, "Playing label '%c' start=%u length=%u", c, start, length);	      
	    self->params_.range = {start, length};
	    self->played_ = 0;
	    //                fseek(self->fp_, start * BYTES_PER_SAMPLE, SEEK_SET);
	    self->seek_file(start*BYTES_PER_SAMPLE,SEEK_SET);
	    self->playing_ = true;
	    //		vTaskDelay(pdMS_TO_TICKS(1));  // ←追加
	  } else {
	    ESP_LOGW(TAG, "Label '%c' not found: skipping", entry.symbol);	      
	    //	  self->string_buf.pop(); // skip
	    self->fifo.pop(entry);
	  }
	  
	}
      } else { //      if (self->fifo.peek(c)) {
	self->silence_mode = true;
	self->silence_remain = static_cast<size_t>(self->params_.rate * 0.01f);

	if (!self->fifo_was_empty_) {
	  self->fifo_was_empty_ = true;
	  if (self->callback_task_handle_) {
	    xTaskNotifyGive(self->callback_task_handle_);
	  }
	}
	continue;
      } //      if (self->fifo.peek(c)) {

	    
    } //    if (!self->playing_ && !silence_mode) {

    if (self->silence_mode) {
      size_t chunk = (self->silence_remain < BUF_SIZE) ? self->silence_remain : BUF_SIZE;
      if (chunk > 0) {
	std::fill_n(inbuf_, chunk, 0); 
	self->interpolate_and_write_4x(inbuf_, chunk);
	vTaskDelay(pdMS_TO_TICKS(1));
      } else {
	ESP_LOGE(TAG, "silence_buf is NULL or chunk=0!");
      }
	    
      if (chunk >= self->silence_remain) {
	self->silence_remain = 0;
	self->silence_mode = false;
      } else {
	self->silence_remain -= chunk;
      }
      continue;
    }

    if (self->playing_) {
      uint32_t remain = self->params_.range.length - self->played_;
      if (remain == 0) {
	self->playing_ = false;
	//	  self->string_buf.pop();
	self->fifo.pop(entry);
	continue;
      }
      size_t toRead = (remain < BUF_SIZE) ? remain : BUF_SIZE;
      //            size_t read = fread(inbuf, sizeof(int16_t), toRead, self->fp_);
      //	    size_t read = self->read_file(inbuf,sizeof(int16_t)*toRead);

      size_t read = self->read_file(inbuf_, sizeof(int16_t), toRead);
      vTaskDelay(pdMS_TO_TICKS(1));  // ←追加	
      if (read == 0) {
	ESP_LOGE(TAG, "read_file returned 0 bytes for playback");
	self->playing_ = false;
	continue;
      }
      if (read != toRead) {
	ESP_LOGW(TAG, "read_file() returned %zu (expected %zu): skipping", read, toRead);
	self->playing_ = false;
	continue;
      }
 
      self->interpolate_and_write_4x(inbuf_, read);
      //	    vTaskDelay(pdMS_TO_TICKS(1));  // ←追加
      self->played_ += read;
    } else {    // if (self->playing_) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  } // while (!self->terminate_task_) {
  if (self->is_file_open()) {
    //        fclose(self->fp_);
    //self->fp_ = nullptr;
    self->close_file();
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  ESP_LOGI(TAG, "AudioPlayer task exiting");
  vTaskDelete(nullptr);

}

uint32_t AudioPlayer::currentSample() const {
    return params_.range.start + played_;
}

uint32_t AudioPlayer::playedInRange() const {
    return played_;
}



void AudioPlayer::play_morse_char(char c) {
    const char *morse = get_morse_code(c);
    if (!morse) return;

    for (const char *p = morse; *p; ++p) {
        if (*p == '.') play_dot();
        else if (*p == '-') play_dash();
        play_silence(1);
    }
    play_silence(3);
}

void AudioPlayer::play_wabun_morse_char(uint16_t c) {
    ESP_LOGI(TAG, "play_wabun_morse_char: U+%04X", c);
    //    const char *morse = get_wabun_morse_code(c);
    //    if (!morse) {
    //      ESP_LOGW(TAG, "Unknown 和文モールス U+%04X (スキップ)", c);
    //      return;
    //    }

    bool dakuten = false, handakuten = false;
    const char *base_morse = get_base_kana(c, dakuten, handakuten);
    if (!base_morse) {
        ESP_LOGW(TAG, "Unknown 和文モールス U+%04X (スキップ)", c);
        return;
    }
    ESP_LOGI(TAG, "Base kana for U+%04X: %s", c, base_morse);

    const char *morse = base_morse;
    
    ESP_LOGI(TAG, "Morse code for U+%04X: %s", c, morse);

    for (const char *p = morse; *p; ++p) {
        if (*p == '.') play_dot();
        else if (*p == '-') play_dash();
        play_silence(1);
    }

    // 濁点/半濁点の追加符号
    if (dakuten) {
        ESP_LOGI(TAG, "Adding 濁点");
        play_dot(); play_dot();
        play_silence(1);
    }
    if (handakuten) {
        ESP_LOGI(TAG, "Adding 半濁点");
        play_dash(); play_dot();
        play_silence(1);
    }

    play_silence(3);
}

// 濁点/半濁点処理
const char* AudioPlayer::get_base_kana(uint16_t c, bool &has_dakuten, bool &has_handakuten) {
    has_dakuten = false;
    has_handakuten = false;
    switch (c) {
        case 0x30AC: c = 0x30AB; has_dakuten = true; break; // ガ -> カ
        case 0x30AE: c = 0x30AD; has_dakuten = true; break; // ギ -> キ
        case 0x30B0: c = 0x30AF; has_dakuten = true; break; // グ -> ク
        case 0x30B2: c = 0x30B1; has_dakuten = true; break; // ゲ -> ケ
        case 0x30B4: c = 0x30B3; has_dakuten = true; break; // ゴ -> コ
        case 0x30B6: c = 0x30B5; has_dakuten = true; break; // ザ -> サ
        case 0x30B8: c = 0x30B7; has_dakuten = true; break; // ジ -> シ
        case 0x30BA: c = 0x30B9; has_dakuten = true; break; // ズ -> ス
        case 0x30BC: c = 0x30BB; has_dakuten = true; break; // ゼ -> セ
        case 0x30BE: c = 0x30BD; has_dakuten = true; break; // ゾ -> ソ
        case 0x30C0: c = 0x30BF; has_dakuten = true; break; // ダ -> タ
        case 0x30C2: c = 0x30C1; has_dakuten = true; break; // ヂ -> チ
        case 0x30C5: c = 0x30C4; has_dakuten = true; break; // ヅ -> ツ
        case 0x30C7: c = 0x30C6; has_dakuten = true; break; // デ -> テ
        case 0x30C9: c = 0x30C8; has_dakuten = true; break; // ド -> ト
        case 0x30D0: c = 0x30CF; has_dakuten = true; break; // バ -> ハ
        case 0x30D3: c = 0x30D2; has_dakuten = true; break; // ビ -> ヒ
        case 0x30D6: c = 0x30D5; has_dakuten = true; break; // ブ -> フ
        case 0x30D9: c = 0x30D8; has_dakuten = true; break; // ベ -> ヘ
        case 0x30DC: c = 0x30DB; has_dakuten = true; break; // ボ -> ホ
        case 0x30D1: c = 0x30CF; has_handakuten = true; break; // パ -> ハ
        case 0x30D4: c = 0x30D2; has_handakuten = true; break; // ピ -> ヒ
        case 0x30D7: c = 0x30D5; has_handakuten = true; break; // プ -> フ
        case 0x30DA: c = 0x30D8; has_handakuten = true; break; // ペ -> ヘ
        case 0x30DD: c = 0x30DB; has_handakuten = true; break; // ポ -> ホ
    }
    return get_wabun_morse_code(c);
}

void AudioPlayer::play_dot() {
  interpolate_and_write_4x(dot_buf, dot_samples);
  vTaskDelay(pdMS_TO_TICKS(1));  
}

void AudioPlayer::play_dash() {
  interpolate_and_write_4x(dash_buf, dash_samples);
  vTaskDelay(pdMS_TO_TICKS(1));  
}

void AudioPlayer::play_silence(int duration_dots) {
    for (int i = 0; i < duration_dots; ++i) {
      interpolate_and_write_4x(silence_buf, silence_samples);
      vTaskDelay(pdMS_TO_TICKS(1));      
    }
}


const char* AudioPlayer::get_morse_code(char c) {
    static const struct { char ch; const char *code; } table[] = {
        {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},  {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."}, {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"}, {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"},{'R', ".-."}, {'S', "..."},  {'T', "-"},
        {'U', "..-"},{'V', "...-"},{'W', ".--"},   {'X', "-..-"},
        {'Y', "-.--"},{'Z', "--.."},
        {'0', "-----"},{'1', ".----"},{'2', "..---"},{'3', "...--"},
        {'4', "....-"},{'5', "....."},{'6', "-...."},{'7', "--..."},
        {'8', "---.."},{'9', "----."},
	{'/', "-..-."},	{'?',"..--.."},{'<',"-...-"},{'>',".-.-."},
        {'=', "-...-"},{'+', ".-.-."},{'~', "...-.-"},{'*', "...---..."},
        {' ', " "}
    };
    for (auto &entry : table) {
        if (toupper(c) == entry.ch) return entry.code;
    }
    return nullptr;
}
int AudioPlayer::unit_time() const {
    return 1200 / wpm;
}

const char* AudioPlayer::get_wabun_morse_code(uint16_t c) {
    static const struct { uint16_t ch; const char *code; } table[] = {
        {0x30A2, "--.--"},    // ア
        {0x30A4, ".-"},      // イ
        {0x30A6, "..-"},    // ウ
        {0x30A8, "-.---"},// エ
        {0x30AA, ".--."},  // オ
        {0x30AB, ".-.."},  // カ
        {0x30AD, "-.-.."},// キ
        {0x30AF, "...-"},  // ク
        {0x30B1, "-.--"},  // ケ
        {0x30B3, "----"},  // コ
        {0x30B5, "-.-.-"},// サ
        {0x30B7, "--.-."},// シ
        {0x30B9, "---.-"},// ス
        {0x30BB, ".---."},// セ
        {0x30BD, "---."},  // ソ
        {0x30BF, ".-."},    // タ
        {0x30C1, "..-."},  // チ
        {0x30C4, ".--.-"},// ツ
        {0x30C6, ".-.--"},// テ
        {0x30C8, "..-.."},// ト
        {0x30CA, ".-.-"},  // ナ
        {0x30CB, "-.-."},  // ニ
        {0x30CC, "....-"},// ヌ
        {0x30CD, "-.--."},// ネ
        {0x30CE, "..--"},  // ノ
        {0x30CF, "-..."},  // ハ
        {0x30D2, "--..-"},// ヒ
        {0x30D5, "--.."},  // フ
        {0x30D8, "."},        // ヘ
        {0x30DB, "-.."},    // ホ
        {0x30DE, "-..-"},  // マ
        {0x30DF, "..-.-"},// ミ
        {0x30E0, "-"},        // ム
        {0x30E1, "-...-"},// メ
        {0x30E2, "-..-."},// モ
        {0x30E4, ".--"},    // ヤ
        {0x30E6, "-..--"},// ユ
        {0x30E8, "--"},      // ヨ
        {0x30E9, "..."},    // ラ
        {0x30EA, "--."},    // リ
        {0x30EB, "-.---"},// ル
        {0x30EC, "---"},    // レ
        {0x30ED, ".-.-."},// ロ
        {0x30EF, "-.-"},    // ワ
        {0x30F0, ".-..-"},// ヰ
        {0x30F1, "-.-.."},// ヱ
        {0x30F2, ".---"},  // ヲ
        {0x30F3, ".-.-."},// ン
        // 和文プロシグナル
        {0xFF3B, "-..--"},// ホレ ([)
        {0xFF3D, "...-."} // ラタ (])

    };

    for (auto &entry : table) {
        if (c == entry.ch) return entry.code;
    }
    return nullptr; // 未対応はスキップ
}


#include "AudioPlayer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"

//#include "esp_vfs_spiffs.h"
#include "esp_log.h"


#include "mux_transport.h"
//AudioPlayer::Params prm{"dvp8k.raw", 8000, {0, 8000}};
AudioPlayer::Params prm{"metan.wav", 8000, {0, 8000}};
AudioPlayer player(prm);

extern "C" void init_AudioPlayer(void) {
#ifdef USE_SD
  
#else
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/spiffs",
        .partition_label = nullptr,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    if (esp_vfs_spiffs_register(&spiffs_cfg) != ESP_OK) {
        ESP_LOGE("MAIN", "SPIFFS mount failed");
        return;
    }

#endif

    if (player.begin() != ESP_OK) {
        ESP_LOGE("MAIN", "AudioPlayer begin failed");
        return;
    }


    player.set_on_empty_callback([]() {
      //      ESP_LOGI("AudioPlayer", "Playback finished (FIFO empty)");
      char buf[20];
      sprintf(buf,"playc:"); // play completed
      mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL,MUX_PORT_MAIN_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    });

    // ラベルファイルを読み込む
    //    if (!player.load_labels("dvp8k_la.txt")) {
    if (!player.load_labels("metanidx.txt")) {    
        ESP_LOGE("MAIN", "Failed to load labels");
        return;
    }


    return;
    // 再生する文字列を設定
    
    player.play_string("0123456789 ijklmnopqr JK1DVP/1 jk1dvp/1 ja1zlo JA1ZLO jf3tbl/1 JF3TBL/1 ji1upl/3 JI1UPL/3 abcde");
    //    player.set_wpm(25); 

    
    //    return; 
    // 再生中のFIFO状態を定期的に表示
    /*    while (true) {
      char buf[100];
      if (!player.get_string_fifo(buf,100,AudioPlayer::FIFO_BOTH)) {
            ESP_LOGI("MAIN", "FIFO empty, playback finished");
            break;
      } else {
        ESP_LOGI("MAIN", "Current FIFO: %s", buf);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      }*/
    player.set_default_wpm(25);
    player.play_morse("CQ CQ DE JA1ZLO");
    player.set_default_wpm(35);    
    player.play_morse("JA1ZLO = アリガトウ［アラキ］ ?+K~ *");
    /*    while (true) {
      char buf[100];
      if (!player.get_string_fifo(buf,100,AudioPlayer::FIFO_BOTH)) {
            ESP_LOGI("MAIN", "FIFO empty, playback finished");
            break;
      } else {
        ESP_LOGI("MAIN", "Current FIFO: %s", buf);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
    */
    ESP_LOGI("MAIN", "All playback finished");
    //    player.stop();
    //    while (1) {
    //        vTaskDelay(pdMS_TO_TICKS(1));
    //    }
}

void play_sound(char *cmd) {
  char buf[100];
  int wpm;
  // 1st char subcommand from 2nd charcter parameters
  switch(cmd[0]) {
  case 'p': // pstring_to_play
    player.play_string(cmd+1);
    break;
  case 'c': // cstring_cw
    player.play_morse(cmd+1);
    break;
  case 'w': // wWPM
    if (sscanf(cmd+1,"%d",&wpm)!=1) wpm=25;
    player.set_default_wpm(wpm);
    break;
  case 'q': // query current status of play
    //    std::string playbuf=player.get_string_fifo();
    sprintf(buf,"playq:");
    if (!player.get_string_fifo(buf+6,100-6,AudioPlayer::FIFO_BOTH)) {
      //      ESP_LOGI("MAIN", "FIFO empty, playback finished");
      buf[6]='\0';
    } 
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL,MUX_PORT_MAIN_BRD_CTRL,(unsigned char *)buf,strlen(buf));
    break;
  case 'l': // llabelfn load labels
    player.load_labels(cmd+1);
    break;
  case 'f': // fsound_file set sound file
    player.set_file(cmd+1);
    break;
  case 's': // s stop playing stop playing 
    player.clear_string_fifo();
    break;
  }
}


