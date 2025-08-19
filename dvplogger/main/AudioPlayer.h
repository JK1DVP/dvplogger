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

#ifndef FILE_AUDIO_PLAYER_H
#define FILE_AUDIO_PLAYER_H

#include <stdio.h>
#include <string.h>
#include <string>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/i2s.h>
#include <esp_log.h>
//#include "freertos/ringbuf.h"
//#include "freertos/queue.h"
#include "RingBuffer.h"
#include <functional>

#ifdef USE_SD
#include <SD.h>
#endif

class AudioPlayer {
public:
  enum GetStringMode {
    FIFO_TEXT_ONLY = 0x01,   // 音声ラベルのみ
    FIFO_MORSE_ONLY = 0x02,  // モールスのみ
    FIFO_BOTH = FIFO_TEXT_ONLY | FIFO_MORSE_ONLY // 両方
  };

  // FIFO用構造体定義
  struct FifoEntry {
    uint16_t symbol; // モールス文字 or 音声ラベル
    uint16_t wpm;    // この文字のWPM
  };

  enum class Cmd : int {
    STOP = 1,
    SET_RANGE,
    CLEAR_FIFO
  };

  struct Range {
    uint32_t start;
    uint32_t length;
    Range(uint32_t s = 0, uint32_t l = 8000) : start(s), length(l) {}
  };

  struct Params {
    const char* path;
    uint32_t    rate;
    Range       range;
    Params() : path("/spiffs/morse.wav"), rate(8000), range() {}
    Params(const char* p, uint32_t r, Range rng = Range())
      : path(p), rate(r), range(rng) {}
  };

  explicit AudioPlayer(const Params& p = Params());
  ~AudioPlayer();

  esp_err_t begin();
  void      stop();
  bool      seek(uint32_t startSample, uint32_t lengthSamples);
  bool      seek_sec(float startSec, float lengthSec);

  bool      play_string(const char* s);
  bool      clear_string_fifo();
  //  std::string get_string_fifo();
  int get_string_fifo(char *buf, size_t max_len, int flags);
  //  int get_string_fifo(char *buf, size_t max_len);

  bool      load_labels(const char* path);
  bool      set_file(const char* path);

  uint32_t  currentSample() const;
  uint32_t  playedInRange() const;
  //    bool      isPlaying() const { return playing_; }
  bool isPlaying() {
    struct FifoEntry dummy;
    return fifo.peek(dummy);
  }

  void set_on_empty_callback(std::function<void()> cb);
  void set_default_wpm(int new_wpm); // 次のpush用

  //  bool play_string(const char *s);
  bool play_morse(const char *s);
  static size_t utf16_to_utf8(uint16_t code, char *utf8);
  
  static constexpr const char* TAG = "AudioPlayer";
  inline void set_dac_ofs(uint16_t value) {
    dac_ofs_=value;
  }

  
private:
  int i2s_buffer_delay_ms() const ;
  int dma_buf_len = 0;
  int dma_buf_count = 0;
  uint16_t dac_ofs_;
  
#define TONE_FREQ 700
  //  RingBuffer<char> fifo;  // メンバとして内包
  //  RingBuffer<uint16_t> fifo;
  RingBuffer<FifoEntry> fifo;  // FIFO型を変更
  int16_t *dot_buf, *dash_buf, *silence_buf;
  int dot_samples, dash_samples, silence_samples;
  void set_wpm(int new_wpm);  
  int wpm;
  int default_wpm_ = 20; // デフォルトWPM  
  TaskHandle_t callback_task_handle_ = nullptr;
  bool fifo_was_empty_ = true;
  static void callback_task(void *param);
  // I2S送信残量取得 (低レベルレジスタ)
  int i2s_pending_samples() const;
  
  std::function<void()> on_empty_callback_;
  static constexpr size_t BUF_SIZE = 512;
  static constexpr size_t CHUNK_SIZE = 512;
  static constexpr size_t OUTBUF_SIZE = CHUNK_SIZE * 4;

  static int16_t inbuf_[BUF_SIZE];
  static uint16_t outbuf_[OUTBUF_SIZE];

  static const char* get_base_kana(uint16_t c, bool &has_dakuten, bool &has_handakuten);
  
  static inline int clip(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
  }
  int read_char();
  bool read_line(char *buf, size_t len);
  bool is_file_open();
  bool open_file(const char* path);
  void close_file();
  size_t read_file(void* buf, size_t size, size_t count);
  bool seek_file(long offset, int whence);

  bool find_label(char *s, uint32_t& start, uint32_t& length);
  void interpolate_and_write_4x(int16_t* inbuf, size_t inlen);
  esp_err_t setupI2S(uint32_t sample_rate);
  static void task(void* arg);

  void generate_tone_buffer(int16_t *buf, int samples, float freq);
  void generate_silence_buffer(int16_t *buf, int samples);
  void play_morse_char(char c);
  void play_wabun_morse_char(uint16_t c);
  void play_dot();
  void play_dash();
  void play_silence(int duration_dots);
  //    void interpolate_4x(const uint8_t *buf, int len);
  const char* get_morse_code(char c);
  static const char* get_wabun_morse_code(uint16_t c);
  int unit_time() const;

  struct sound_label {
    int32_t start = 0, length = 0;
    char labelstr[10]="";
  } sound_labels[100];

  Params        params_;
  TaskHandle_t  th_{ nullptr };
  QueueHandle_t q_{ nullptr };

#ifdef USE_SD
  File file_;
#else
  FILE* fp_ = nullptr;
#endif

  
  size_t      num_labels = 0;
  int16_t     prev_data = 0;
  bool        terminate_task_ = false;

  volatile bool silence_mode{ false } ;
  volatile size_t silence_remain { 0 } ;
  
  volatile uint32_t played_{ 0 };
  volatile bool     playing_{ false };

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;


};

extern "C" void init_AudioPlayer(void);
extern void play_sound(char *cmd);
extern AudioPlayer player;
#endif // FILE_AUDIO_PLAYER_H

