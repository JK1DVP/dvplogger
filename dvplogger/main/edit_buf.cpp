/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
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
#include "edit_buf.h"
/// each log entry is fixed length text format
//Q,seqnr,yymmdd_hhmmss,band_code(1byte),mode_code(1byte),HISCALL(10b),SENTRST(3),SENTEXCH(10),RCVRST(3),RCVEXCH(10),freq_Hz(10)
// deleted qso will replace Q to D
// all space will be filled by 0x20 (SPC)
// remarks may be written to separate file  with sequence number


#include <stdint.h>
#include <string.h>
#include <stdbool.h>

static inline int utf8_charlen(uint8_t c) {
    if ((c & 0x80) == 0x00) return 1; // 0xxxxxxx
    if ((c & 0xE0) == 0xC0) return 2; // 110xxxxx
    if ((c & 0xF0) == 0xE0) return 3; // 1110xxxx
    if ((c & 0xF8) == 0xF0) return 4; // 11110xxx
    return 1; // 不正は保守的に1B扱い（壊れにくさ重視）
}

static inline bool is_cont(uint8_t c) { // 続きバイト 10xxxxxx
    return (c & 0xC0) == 0x80;
}

static int used_bytes(const uint8_t *buf) {      // 文字列の実バイト長
    return (int)strlen((const char*)&buf[2]);
}

static int payload_capacity(const uint8_t *buf){ // 使える最大バイト長（終端1Bは別）
  //    plogw->ostream->print("n of bytes usable:");
  //    plogw->ostream->println((int)buf[0]);
    return buf[0];
}

static int count_chars(const uint8_t *s) {       // コードポイント数
    int n = 0, off = 0;
    while (s[off]) {
        off += utf8_charlen(s[off]);
        n++;
    }
    return n;
}

// 文字位置 -> バイトオフセット（先頭から charpos 個 進む）
static int charpos_to_offset(const uint8_t *s, int charpos) {
    int i = 0, off = 0;
    while (s[off] && i < charpos) {
        off += utf8_charlen(s[off]);
        i++;
    }
    return off; // 範囲外なら末尾
}

// バイトオフセットを直前の“文字頭”にスナップ（途中バイトを指さないよう補正）
static int snap_to_char_start(const uint8_t *s, int off) {
    while (off > 0 && is_cont(s[off])) off--;
    return off;
}

void left_buf(char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    int p = buf[1];
    if (p > 0) buf[1] = (uint8_t)(p - 1);
}

void right_buf(char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s = &buf[2];
    int chars = count_chars(s);
    int p = buf[1];
    if (p < chars) buf[1] = (uint8_t)(p + 1);
}

void backspace_buf(char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s = &buf[2];

    int p = buf[1];
    if (p <= 0) return; // 左に文字なし

    int cur_off = charpos_to_offset(s, p);     // 現オフセット
    int prev_off = charpos_to_offset(s, p - 1);// 1文字左の頭
    int ubytes   = used_bytes(buf);

    int del_len = cur_off - prev_off;          // 削除バイト幅（1〜4）
    memmove(&s[prev_off], &s[cur_off], (size_t)(ubytes - cur_off + 1)); // +1は終端
    buf[1] = (uint8_t)(p - 1);
}
void delete_buf(char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s = &buf[2];

    int p = buf[1];
    int ubytes = used_bytes(buf);
    int cur_off = charpos_to_offset(s, p);
    if (cur_off >= ubytes) return; // 右に文字なし

    int del_len = utf8_charlen(s[cur_off]);
    memmove(&s[cur_off], &s[cur_off + del_len], (size_t)(ubytes - (cur_off + del_len) + 1));
    // カーソル位置（文字数）は不変
}

// cp は 1コードポイント（1〜4B）、cp_len はそのバイト数
bool insert_buf_utf8(const char *cp, int cp_len, char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s = &buf[2];

    if (cp_len <= 0 || cp_len > 4) return false;

    int cap    = payload_capacity(buf);
    int ubytes = used_bytes(buf);
    if (ubytes + cp_len > cap) return false; // 容量オーバー
    //    plogw->ostream->print("used bytes:");
    //    plogw->ostream->println(ubytes);

    int p = buf[1];
    int off = charpos_to_offset(s, p);

    // 右側を後ろへズラす
    memmove(&s[off + cp_len], &s[off], (size_t)(ubytes - off + 1)); // +1 は終端
    memcpy(&s[off], cp, (size_t)cp_len);

    buf[1] = (uint8_t)(p + 1);
    return true;
}

void insert_buf(char c, char *buf) { // c: ascii 前提

    (void)insert_buf_utf8((const char*)&c, 1, buf);
}

bool overwrite_buf_utf8(const char *cp, int cp_len, char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s = &buf[2];

    int p = buf[1];
    int ubytes = used_bytes(buf);
    int off = charpos_to_offset(s, p);

    if (off >= ubytes) {
        // 末尾なら挿入扱い
        return insert_buf_utf8(cp, cp_len, cbuf);
    } else {
        // 1文字削除 → 挿入
        delete_buf(cbuf);
        return insert_buf_utf8(cp, cp_len, cbuf);
    }
}

void overwrite_buf(char c, char *buf) {
    (void)overwrite_buf_utf8((const char*)&c, 1, buf);
}

void clear_buf(char *p) {
    // p[0] = capacity(bytes)
    // [1] cursor, [2..2+cap-1] payload, [2+cap] = '\0'（終端確保）
    int cap = (uint8_t)p[0];
    memset(p + 1, 0, cap + 2); // カーソル+ペイロード+終端までゼロ
}

void init_buf(char *p, int siz) {
    // 実際に確保したメモリは siz + 3 バイト以上必要（[0]=siz, [1]=cursor, [2..]payload）
    memset(p, 0, siz + 3);
    p[0] = (char)siz; // 使えるペイロード容量（バイト）
    // p[1]=0 (cursor), p[2]=0 (empty string)
}

void adjust_cursor_buf(char *cbuf) {
    uint8_t *buf = (uint8_t*)cbuf;
    uint8_t *s   = &buf[2];
    int p = buf[1];
    int chars = count_chars(s);
    if (p < 0) p = 0;
    if (p > chars) p = chars;
    buf[1] = (uint8_t)p;
}


typedef struct {
  const char* roma;
  const char* kana; // UTF-8
} Map;

static const Map MAP[] = {
  // --- 拗音・特殊（長いものを先） ---
  {"tsu", "つ"}, {"kyo", "きょ"}, {"kya", "きゃ"}, {"kyu", "きゅ"},
  {"shi", "し"}, {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
  {"chi", "ち"}, {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
  {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
  {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
  {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
  {"ja",  "じゃ"}, {"ju",  "じゅ"}, {"jo",  "じょ"},
  {"fa",  "ふぁ"}, {"fi",  "ふぃ"}, {"fe",  "ふぇ"}, {"fo", "ふぉ"},
  {"ga","が"},  {"gi","ぎ"},  {"gu","ぐ"},  {"ge","げ"},  {"go","ご"},
  {"da","だ"},  {"di","ぢ"},  {"du","づ"},  {"de","で"},  {"do","ど"},
  {"ba","ば"},  {"bi","び"},  {"bu","ぶ"},  {"be","べ"},  {"bo","ぼ"},
  {"pa","ぱ"},  {"pi","ぴ"},  {"pu","ぷ"},  {"pe","ぺ"},  {"po","ぽ"},  
  // --- 基本 ---
  {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
  {"sa","さ"},{"si","し"},{"su","す"},{"se","せ"},{"so","そ"},
  {"ta","た"},{"ti","ち"},{"tu","つ"},{"te","て"},{"to","と"},
  {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
  {"ha","は"},{"hi","ひ"},{"hu","ふ"},{"he","へ"},{"ho","ほ"},
  {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
  {"ya","や"},{"yu","ゆ"},{"yo","よ"},
  {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
  {"wa","わ"},{"wo","を"},{"nn","ん"},
  {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
  // 記号（例）
  {"-","ー"},
};

Preedit g_pre = { .s = {0}, .len = 0 };

// 先頭が子音・次も同じ子音か？（促音対象） n は除外
static bool is_ascii_letter(char c){ return (('a'<=c&&c<='z')||('A'<=c&&c<='Z')); }
static char lowerc(char c){ if ('A'<=c&&c<='Z') return c-'A'+'a'; return c; }

static bool is_vowel(char c){ c=lowerc(c); return (c=='a'||c=='i'||c=='u'||c=='e'||c=='o'); }
static bool is_consonant_start(const char* s,int len){
  if (len<1) return false;
  char c=lowerc(s[0]);
  return is_ascii_letter(c) && !is_vowel(c);
}

// 促音の確定（例: kka -> 先頭の k で「っ」確定、前者だけ消費）
static bool try_sokuon(char *buf){
  if (g_pre.len>=2){
    char c1=lowerc(g_pre.s[0]), c2=lowerc(g_pre.s[1]);
    if (is_ascii_letter(c1) && is_ascii_letter(c2) && c1==c2 && c1!='n'){
      // 「っ」を確定
      const char *xtsu="っ";
      insert_buf_utf8(xtsu, (int)strlen(xtsu), buf);
      // 1文字分だけ左シフト
      memmove(&g_pre.s[0], &g_pre.s[1], (size_t)(g_pre.len-1));
      g_pre.len -= 1;
      g_pre.s[g_pre.len]=0;
      return true;
    }
  }
  return false;
}

// 撥音の確定ルール：
// 1) 先頭が 'n' で 次が子音 or 終端 -> 「ん」確定（ただし次が 'y' の場合は保留）
// 2) "nn" -> 「ん」確定
static bool try_hatsuon(char *buf){
  if (g_pre.len==0) return false;
  if (lowerc(g_pre.s[0])!='n') return false;

  // "nn" -> ん
  if (g_pre.len>=2 && lowerc(g_pre.s[1])=='n'){
    const char *n="ん";
    insert_buf_utf8(n,(int)strlen(n), buf);
    // 2文字消費
    memmove(&g_pre.s[0], &g_pre.s[2], (size_t)(g_pre.len-2));
    g_pre.len-=2; g_pre.s[g_pre.len]=0;
    return true;
  }

  // n + (子音/末尾/記号) かつ次が 'y' ではない → ん
  if (g_pre.len==1){
    // 末尾なら　とりあえず保留（次入力を待つ）にしても良いが、
    // 区切りキー（空白/記号）で確定する運用も可能。ここでは保留。
    return false;
  }else{
    char c2=lowerc(g_pre.s[1]);
    bool next_is_y = (c2=='y');
    bool next_is_vowel = is_vowel(c2);
    if (!next_is_y && !next_is_vowel){
      const char *n="ん";
      insert_buf_utf8(n,(int)strlen(n), buf);
      // 1文字（n）だけ消費
      memmove(&g_pre.s[0], &g_pre.s[1], (size_t)(g_pre.len-1));
      g_pre.len-=1; g_pre.s[g_pre.len]=0;
      return true;
    }
  }
  return false;
}

// テーブル最長一致
static int longest_match(const char* s, int len, const Map** hit){
  int best = 0;
  const Map* bestm = NULL;
  for (size_t i=0;i<sizeof(MAP)/sizeof(MAP[0]);++i){
    int rlen = (int)strlen(MAP[i].roma);
    if (rlen<=len && strncasecmp(s, MAP[i].roma, (size_t)rlen)==0){
      if (rlen>best){
        best = rlen; bestm = &MAP[i];
      }
    }
  }
  if (hit) *hit = bestm;
  return best;
}

// 可能な限り確定して本文へ流し込む
void flush_preedit(char *buf){
  // 促音→最長一致→撥音 の順でループ（順序が肝）
  bool progressed;
  do{
    progressed = false;

    // 促音（例：kka）
    if (try_sokuon(buf)) { progressed = true; continue; }

    // 最長一致
    const Map* m=NULL;
    int mlen = longest_match(g_pre.s, g_pre.len, &m);
    if (mlen>0 && m){
      insert_buf_utf8(m->kana, (int)strlen(m->kana), buf);
      // mlen だけ消費
      memmove(&g_pre.s[0], &g_pre.s[mlen], (size_t)(g_pre.len-mlen));
      g_pre.len -= mlen;
      g_pre.s[g_pre.len]=0;
      progressed = true;
      continue;
    }

    // 撥音（n）
    if (try_hatsuon(buf)) { progressed = true; continue; }

  } while(progressed);
}

// 1 文字キー入力（ASCII想定）→ プレエディットに追加→確定試行
void romaji_input_char(char ch, char *buf){
  // 区切り（空白・句読点など）は、先に flush してから確定で入れる運用がおすすめ
  bool is_delim = (ch==' ' || ch=='\t' || ch=='\n' || ch=='.' || ch==',' || ch=='-' );
  if (is_delim){
    // 未確定の 'n' を確定させるなどの後処理（必要なら）
    // 末尾が単独 'n' なら「ん」を確定してから区切りを挿入、等。
    if (g_pre.len==1 && lowerc(g_pre.s[0])=='n'){
      const char *n="ん";
      insert_buf_utf8(n,(int)strlen(n), buf);
      g_pre.len=0; g_pre.s[0]=0;
    } else {
      flush_preedit(buf);
    }
    // 区切りをそのまま本文へ（カタカナ用途なら '-'→長音などの処理も可）
    char tmp[2]={ch,0};
    insert_buf_utf8(tmp,1,buf);
    return;
  }

  // 普通の英字
  if (g_pre.len < (int)sizeof(g_pre.s)-1){
    g_pre.s[g_pre.len++] = ch;
    g_pre.s[g_pre.len] = 0;
    flush_preedit(buf);
  }
}

// Backspace ハンドラ（プレエディット優先）
void romaji_backspace(char *buf){
  if (g_pre.len>0){
    g_pre.len--;
    g_pre.s[g_pre.len]=0;
    return;
  }
  backspace_buf(buf); // 本文側
}

// カーソル移動前に未確定をどうするかはポリシー次第
// ここでは「できるだけ確定」してから動かす
void romaji_move_left(char *buf){
  flush_preedit(buf);
  left_buf(buf);
}
void romaji_move_right(char *buf){
  flush_preedit(buf);
  right_buf(buf);
}
// ===== 表示組み立て：本文 + preedit を一体化して返す =====
// buf:  [0]=capacity(byte), [1]=cursor(char count), [2..]=UTF-8 string
// pre:  未確定ローマ字（ASCII）を格納（g_pre を渡す想定）
// out:  出力（UTF-8 連結文字列）
// out_sz: out のバイト容量
// out_preedit_start/out_preedit_len: out 内の preedit 部分の「バイト範囲」
// out_caret_byte: out 内でキャレット（|）を置きたい位置の「バイトオフセット」
//    ※ 通常は preedit の末尾にキャレットを置く（=確定前の入力点が視覚的に分かる）
#include <string.h>
#include <stdint.h>
#include <stdbool.h>


// 先に用意済みのヘルパを期待
static int charpos_to_offset(const uint8_t *s, int charpos);
static int used_bytes(const uint8_t *buf);

bool compose_line_with_preedit(
    const char *cbuf,
    const Preedit *pre,
    char *out, size_t out_sz,
    int *out_preedit_start, int *out_preedit_len,
    int *out_caret_byte  // out 内のバイト位置
){
  if (!cbuf || !out || out_sz == 0) return false;

  const uint8_t *buf = (const uint8_t*)cbuf;
  const uint8_t *s   = &buf[2];
  int cursor_chars   = buf[1];
  int ubytes         = used_bytes(buf);

  // 1) 本文のカーソル位置(文字) → バイトオフセット
  int cur_off = charpos_to_offset(s, cursor_chars);
  if (cur_off < 0) cur_off = 0;
  if (cur_off > ubytes) cur_off = ubytes;

  // 2) 左・右のバイト長
  int left_len  = cur_off;
  int right_len = ubytes - cur_off;

  // 3) 出力バッファへ [LEFT][PREEDIT][RIGHT] を連結
  size_t need = (size_t)left_len + (size_t)pre->len + (size_t)right_len + 1; // +1: 終端
  if (need > out_sz) {
    // 収まらない場合は、最低限「右側」を削る（UI 側でスクロールするなら十分）
    // ここは方針次第。簡易に truncate。
    // 右側をどこまで入れられるか計算
    size_t room = out_sz - 1; // 終端分
    if (room == 0) { out[0] = '\0'; return false; }

    // left -> pre -> right の順で詰められるだけ詰める
    size_t w = 0;
    size_t lcopy = (room < (size_t)left_len) ? room : (size_t)left_len;
    memcpy(out + w, s, lcopy); w += lcopy;

    size_t pcopy = 0;
    if (w < room) {
      size_t rem = room - w;
      pcopy = (rem < (size_t)pre->len) ? rem : (size_t)pre->len;
      memcpy(out + w, pre->s, pcopy); w += pcopy;
    }

    size_t rcopy = 0;
    if (w < room) {
      size_t rem = room - w;
      rcopy = (rem < (size_t)right_len) ? rem : (size_t)right_len;
      memcpy(out + w, s + left_len, rcopy); w += rcopy;
    }
    out[w] = '\0';

    // preedit の範囲と caret の位置（可能な範囲で設定）
    if (out_preedit_start) *out_preedit_start = (int)lcopy;          // left の直後
    if (out_preedit_len)   *out_preedit_len   = (int)pcopy;          // 実際に入った preedit 長
    if (out_caret_byte)    *out_caret_byte    = (int)(lcopy + pcopy);// preedit の末尾にキャレット
    return true;
  }

  // 余裕あり：フルで連結
  size_t w = 0;
  memcpy(out + w, s, (size_t)left_len); w += (size_t)left_len;
  if (out_preedit_start) *out_preedit_start = (int)w;

  memcpy(out + w, pre->s, (size_t)pre->len); w += (size_t)pre->len;
  if (out_preedit_len) *out_preedit_len = pre->len;

  memcpy(out + w, s + left_len, (size_t)right_len); w += (size_t)right_len;
  out[w] = '\0';

  if (out_caret_byte) *out_caret_byte = (int)( (size_t)left_len + (size_t)pre->len );
  return true;
}

// 例: "確定左[preedit]|確定右"
bool compose_marked_line(const char *cbuf, const Preedit *pre,
                         char *out, size_t out_sz)
{
  char tmp[256];
  int ps, pl, caret;
  if (!compose_line_with_preedit(cbuf, pre, tmp, sizeof(tmp), &ps, &pl, &caret)) return false;

  // tmp を 3分割して [ ] と | を差し込む
  // out_sz を越えないように安全に連結
  size_t w = 0;
  // left
  if (caret > 0) {
    size_t lcopy = (size_t)caret;
    if (w + lcopy >= out_sz) lcopy = (out_sz-1) - w;
    memcpy(out + w, tmp, lcopy); w += lcopy;
  }
  // [preedit]
  if (w < out_sz-1) out[w++] = '[';
  size_t pcopy = (size_t)pl;
  if (w + pcopy >= out_sz) pcopy = (out_sz-1) - w;
  memcpy(out + w, tmp + ps, pcopy); w += pcopy;
  if (w < out_sz-1) out[w++] = ']';
  // |
  if (w < out_sz-1) out[w++] = '|';
  // right
  const char *right = tmp + caret;
  size_t rlen = strlen(right);
  if (w + rlen >= out_sz) rlen = (out_sz-1) - w;
  memcpy(out + w, right, rlen); w += rlen;

  out[w] = '\0';
  return true;
}



#include <string.h>
#include <stdint.h>
#include <stddef.h>

size_t utf8_substr_range(const char *s, int i, int j,
                         char *out, size_t out_size) {
    if (!s || !out || out_size == 0 || i < 0 || j < i) {
        if (out && out_size > 0) out[0] = '\0';
        return 0;
    }

    const uint8_t *p = (const uint8_t*)s;
    int count = 0;
    int start_off = -1, end_off = -1;
    int off = 0;

    // i, j のバイトオフセットを探す
    while (p[off]) {
        if (count == i) start_off = off;
        if (count == j) { end_off = off; break; }

        int len = utf8_charlen(p[off]);
        off += len;
        count++;
    }
    if (start_off < 0) start_off = off; // i が末尾超なら末尾
    if (end_off < 0) end_off = off;     // j が末尾超なら末尾

    int copy_len = end_off - start_off;
    if (copy_len < 0) copy_len = 0;

    if ((size_t)copy_len >= out_size) copy_len = (int)out_size - 1;
    memcpy(out, s + start_off, (size_t)copy_len);
    out[copy_len] = '\0';

    return (size_t)copy_len;
}

#include <stdint.h>
#include <string.h>

static int utf8_decode_advance(const char *s, int max, uint32_t *out_cp) {
    if (max <= 0 || !s || !*s) return 0;
    const unsigned char *p = (const unsigned char *)s;

    // 1-byte (ASCII)
    if ((p[0] & 0x80) == 0x00) { *out_cp = p[0]; return 1; }

    // 2-byte
    if ((p[0] & 0xE0) == 0xC0) {
        if (max < 2 || (p[1] & 0xC0) != 0x80) { *out_cp = p[0]; return 1; }
        uint32_t cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp < 0x80) { *out_cp = p[0]; return 1; } // overlong
        *out_cp = cp; return 2;
    }

    // 3-byte
    if ((p[0] & 0xF0) == 0xE0) {
        if (max < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { *out_cp = p[0]; return 1; }
        uint32_t cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp < 0x800) { *out_cp = p[0]; return 1; } // overlong
        // surrogate 排除
        if (cp >= 0xD800 && cp <= 0xDFFF) { *out_cp = p[0]; return 1; }
        *out_cp = cp; return 3;
    }

    // 4-byte
    if ((p[0] & 0xF8) == 0xF0) {
        if (max < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) { *out_cp = p[0]; return 1; }
        uint32_t cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) { *out_cp = p[0]; return 1; } // overlong/out of range
        *out_cp = cp; return 4;
    }

    // 不正先頭は 1 バイト消費
    *out_cp = p[0];
    return 1;
}

static int is_combining_min(uint32_t u) {
    /* 最低限の結合文字帯（主にダイアクリティカル）だけ 0 幅にする */
    if ((u >= 0x0300 && u <= 0x036F) ||   // Combining Diacritical Marks
        (u >= 0x200B && u <= 0x200F) ||   // ZWSP/ZWNJ/ZWJ etc.
        (u >= 0xFE00 && u <= 0xFE0F) ||   // Variation Selectors
        (u == 0x3099 || u == 0x309A))     // ゙ ゚
        return 1;
    return 0;
}

static int mk_wcwidth_cjk(uint32_t u) {
    /* C0/C1 control → 幅 0 */
    if (u == 0) return 0;
    if ((u < 32) || (u >= 0x7F && u < 0xA0)) return 0;

    /* 結合（簡易）→ 幅 0 */
    if (is_combining_min(u)) return 0;

    /* 全角/広義の CJK、曖昧幅、Emoji を 2 幅扱い */
    if (u >= 0x1100 &&
        (u <= 0x115F ||                 // Hangul Jamo init. consonants
         u == 0x2329 || u == 0x232A ||
         (u >= 0x2E80 && u <= 0xA4CF && u != 0x303F) || // CJK…（曖昧含む）
         (u >= 0xAC00 && u <= 0xD7A3) ||                // Hangul Syllables
         (u >= 0xF900 && u <= 0xFAFF) ||                // CJK Compatibility Ideographs
         (u >= 0xFE10 && u <= 0xFE19) ||
         (u >= 0xFE30 && u <= 0xFE6F) ||
         (u >= 0xFF00 && u <= 0xFF60) ||                // Fullwidth forms
         (u >= 0xFFE0 && u <= 0xFFE6) ||
         (u >= 0x1F000 && u <= 0x1FAFF) ||              // Symbols/Emoji（広めに 2）
         (u >= 0x20000 && u <= 0x3FFFD)))
        return 2;

    /* それ以外は 1 幅 */
    return 1;
}

int utf8_display_width_upto_cjk(const char *s, int i_chars) {
    if (!s || i_chars <= 0) return 0;
    int cols = 0, done = 0;
    const char *p = s;
    while (*p && done < i_chars) {
        uint32_t cp; int adv = utf8_decode_advance(p, (int)strlen(p), &cp);
        if (adv <= 0) break; // 念のため
        cols += mk_wcwidth_cjk(cp);
        p += adv;
        ++done;
    }
    return cols;
}

int utf8_display_width_range_cjk(const char *s, int i_chars, int j_chars) {
    if (!s || j_chars <= i_chars) return 0;
    int wi = utf8_display_width_upto_cjk(s, i_chars);
    int wj = utf8_display_width_upto_cjk(s, j_chars);
    return (wj - wi);
}

typedef struct {
    int byte_offset;   // s 先頭からのバイト位置
    int display_cols;  // 先頭からの表示幅（mk_wcwidth_cjk の合計）
    int chars_done;    // 実際にたどり着いた文字数（末尾で打ち切られる場合あり）
} Utf8PosCJK;

Utf8PosCJK utf8_pos_at_char_cjk(const char *s, int i_chars) {
    Utf8PosCJK r = {0, 0, 0};
    if (!s || i_chars <= 0) return r;
    const char *p = s;
    while (*p && r.chars_done < i_chars) {
        uint32_t cp; int adv = utf8_decode_advance(p, (int)strlen(p), &cp);
        if (adv <= 0) break;
        r.display_cols += mk_wcwidth_cjk(cp);
        p += adv;
        r.chars_done++;
    }
    r.byte_offset = (int)(p - s);
    return r;
}
#include <stdint.h>
#include <string.h>


static void width_and_offset_upto_n(const char *s, int n,
                                    int *out_cols, int *out_byte_off) {
    const char *p = s;
    int done = 0, cols = 0;
    while (*p && done < n) {
        uint32_t cp; int adv = utf8_decode_advance(p, (int)strlen(p), &cp);
        if (adv <= 0) break;
        cols += mk_wcwidth_cjk(cp);
        p += adv;
        ++done;
    }
    if (out_cols)     *out_cols = cols;
    if (out_byte_off) *out_byte_off = (int)(p - s);
}

static int utf8_count_chars(const char *s) {
    const char *p = s; int cnt = 0;
    while (*p) {
        uint32_t cp; int adv = utf8_decode_advance(p, (int)strlen(p), &cp);
        if (adv <= 0) break;
        p += adv; ++cnt;
    }
    return cnt;
}

Utf8PosAtColCJK utf8_pos_at_column_cjk(const char *s, int target_cols) {
    Utf8PosAtColCJK r = {0,0,0,0,0,0,0};
    if (!s || target_cols <= 0) {
        /* 0列なら先頭（0文字） */
        r.total_chars = utf8_count_chars(s ? s : "");
        width_and_offset_upto_n(s ? s : "", r.total_chars, &r.total_cols, NULL);
        return r;
    }

    int total_chars = utf8_count_chars(s);
    int total_cols  = 0; width_and_offset_upto_n(s, total_chars, &total_cols, NULL);

    /* 早期判定：全体が target 以下なら末尾 */
    if (total_cols <= target_cols) {
        int off=0; width_and_offset_upto_n(s, total_chars, &r.display_cols, &off);
        r.char_index  = total_chars;
        r.byte_offset = off;
        r.total_chars = total_chars;
        r.total_cols  = total_cols;
        r.next_char_cols = 0;
        r.at_exact = (r.display_cols == target_cols);
        return r;
    }

    /* 二分探索：幅 <= target となる最大の mid を探す */
    int lo = 0, hi = total_chars; /* 解は[0, total_chars) にある */
    int best_idx = 0, best_cols = 0, best_off = 0;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cols = 0, off = 0;
        width_and_offset_upto_n(s, mid, &cols, &off);

        if (cols <= target_cols) {
            /* 条件を満たす → もっと右を探す */
            best_idx  = mid;
            best_cols = cols;
            best_off  = off;
            lo = mid + 1;
        } else {
            /* 超えた → 左へ */
            hi = mid - 1;
        }
    }

    /* 次の1文字の幅（あれば） */
    int next_w = 0;
    if (best_idx < total_chars) {
        /* best_idx の次の1文字の幅を調べる */
        int off = best_off;
        uint32_t cp; int adv = utf8_decode_advance(s + off, (int)strlen(s + off), &cp);
        if (adv > 0) next_w = mk_wcwidth_cjk(cp);
    }

    r.char_index     = best_idx;
    r.byte_offset    = best_off;
    r.display_cols   = best_cols;
    r.total_chars    = total_chars;
    r.total_cols     = total_cols;
    r.next_char_cols = next_w;
    r.at_exact       = (best_cols == target_cols);
    return r;
}

size_t utf8_slice_by_columns_cjk(const char *s, int colL, int colR,
                                 char *out, size_t out_sz) {
    if (!s || !out || out_sz == 0 || colR <= colL) {
        if (out && out_sz) out[0]='\0';
        return 0;
    }
    Utf8PosAtColCJK L = utf8_pos_at_column_cjk(s, colL);
    Utf8PosAtColCJK R = utf8_pos_at_column_cjk(s, colR);

    /* R は「<= colR」の最大位置なので、右端は R.byte_offset でOK */
    int start = L.byte_offset;
    int end   = R.byte_offset;
    if (end < start) end = start;

    int copy_len = end - start;
    if (copy_len < 0) copy_len = 0;
    if ((size_t)copy_len >= out_sz) copy_len = (int)out_sz - 1;

    memcpy(out, s + start, (size_t)copy_len);
    out[copy_len] = '\0';
    return (size_t)copy_len;
}


bool compose_line_with_preedit_cjk(
    const char *cbuf,
    const Preedit *pre,
    char *out, size_t out_sz,
    /* byte positions */
    int *out_preedit_start_byte, int *out_preedit_len_bytes, int *out_caret_byte,
    /* display columns (CJK=2) */
    int *out_preedit_start_cols, int *out_preedit_len_cols, int *out_caret_cols,
    /* totals */
    int *out_total_cols
){
    if (!cbuf || !pre || !out || out_sz==0) return false;

    const uint8_t *buf = (const uint8_t*)cbuf;
    const uint8_t *s   = &buf[2];
    const int cursor_chars = buf[1];

    const int ubytes = used_bytes(buf);
    int cur_off = charpos_to_offset(s, cursor_chars);
    if (cur_off < 0) cur_off = 0;
    if (cur_off > ubytes) cur_off = ubytes;

    // 左右に分割（バイト）
    const int left_len  = cur_off;
    const int right_len = ubytes - cur_off;

    // 出力組み立て（右端切り詰めあり）
    size_t w = 0;
    size_t lcopy = (out_sz-1 < (size_t)left_len) ? (out_sz-1) : (size_t)left_len;
    memcpy(out + w, s, lcopy); w += lcopy;

    const int pre_start_byte = (int)w;
    // preedit を out に書く（UTF-8 として扱う：ASCII でもOK）
    size_t pcopy = 0;
    if (w < out_sz-1) {
        size_t rem = (out_sz-1) - w;
        size_t pre_avail = (size_t)pre->len;
        // pre->s は UTF-8 可。必要ならここで UTF-8 正当性チェックも可。
        pcopy = (rem < pre_avail) ? rem : pre_avail;
        memcpy(out + w, pre->s, pcopy); w += pcopy;
    }
    const int caret_byte = (int)w; // preedit 末尾

    size_t rcopy = 0;
    if (w < out_sz-1) {
        size_t rem = (out_sz-1) - w;
        rcopy = (rem < (size_t)right_len) ? rem : (size_t)right_len;
        memcpy(out + w, s + left_len, rcopy); w += rcopy;
    }
    out[w] = '\0';

    // --- 列幅の計算（論理値。out の切り詰めと無関係） ---
    const int body_total_chars = utf8_count_chars((const char*)s);
    const int body_total_cols  = utf8_display_width_upto_cjk((const char*)s, body_total_chars);
    const int left_cols        = utf8_display_width_upto_cjk((const char*)s, cursor_chars);

    int pre_cols = 0; // preedit の列幅（UTF-8 として走査）
    {
        const char *pp = pre->s; int rem = pre->len;
        while (rem > 0 && *pp) {
            uint32_t cp; int adv = utf8_decode_advance(pp, rem, &cp);
            if (adv <= 0) { pre_cols += 1; pp++; rem--; }   // 壊れたら 1 幅扱い
            else { pre_cols += mk_wcwidth_cjk(cp); pp += adv; rem -= adv; }
        }
    }
    const int right_cols = body_total_cols - left_cols;
    const int total_cols = left_cols + pre_cols + right_cols; // 行全体の列幅

    // byte 出力
    if (out_preedit_start_byte) *out_preedit_start_byte = pre_start_byte;
    if (out_preedit_len_bytes)  *out_preedit_len_bytes  = (int)pcopy;
    if (out_caret_byte)         *out_caret_byte         = caret_byte;

    // 列幅出力
    if (out_preedit_start_cols) *out_preedit_start_cols = left_cols;
    if (out_preedit_len_cols)   *out_preedit_len_cols   = pre_cols;
    if (out_caret_cols)         *out_caret_cols         = left_cols + pre_cols;

    if (out_total_cols)         *out_total_cols         = total_cols;

    return true;
}





size_t window_line_by_columns_caret_cjk(
    const char *s, int total_cols, int caret_col,
    int max_cols, int desired_local_caret_col,
    char *out, size_t out_sz,
    int *out_colL, int *out_caret_local_col
){
    if (!s || !out || out_sz == 0 || max_cols <= 0) {
        if (out && out_sz) out[0] = '\0';
        if (out_colL) *out_colL = 0;
        if (out_caret_local_col) *out_caret_local_col = 0;
        return 0;
    }

    // クリップ（caret は末尾 total_cols も許容）
    if (total_cols < 0) total_cols = 0;
    if (caret_col < 0) caret_col = 0;
    if (caret_col > total_cols) caret_col = total_cols;

    // 全体が入るならそのまま
    if (total_cols <= max_cols) {
        int window_width = total_cols;                 // 実際の窓幅
        size_t n = utf8_slice_by_columns_cjk(s, 0, window_width, out, out_sz);
        if (out_colL) *out_colL = 0;
        int caret_local = caret_col;                   // 0..window_width
        if (caret_local < 0) caret_local = 0;
        if (caret_local > window_width) caret_local = window_width;
        if (out_caret_local_col) *out_caret_local_col = caret_local;
        return n;
    }

    // 希望のローカル位置（末尾も許容）: 0..max_cols
    if (desired_local_caret_col < 0) desired_local_caret_col = 0;
    if (desired_local_caret_col > max_cols) desired_local_caret_col = max_cols;

    // 左端を決める（右端はみ出し防止）。total_cols > max_cols が前提。
    int colL = caret_col - desired_local_caret_col;
    if (colL < 0) colL = 0;
    int max_colL = total_cols - max_cols;             // 少なくとも1以上
    if (colL > max_colL) colL = max_colL;

    // 実際の窓幅（末尾付近は max_cols 未満になる）
    int window_width = total_cols - colL;
    if (window_width > max_cols) window_width = max_cols;

    int colR = colL + window_width;

    // 切り出し（[colL, colR)）
    size_t n = utf8_slice_by_columns_cjk(s, colL, colR, out, out_sz);
    if (out_colL) *out_colL = colL;

    // 窓内キャレット位置（0..window_width）
    int caret_local = caret_col - colL;
    if (caret_local < 0) caret_local = 0;
    if (caret_local > window_width) caret_local = window_width;
    if (out_caret_local_col) *out_caret_local_col = caret_local;

    return n;
}



// 1) 十分大きい一時バッファに「本文+preedit」を組む
//char composed[512];
//int ps_b, pl_b, caret_b;
//int ps_c, pl_c, caret_c;
//int total_cols;
//
//compose_line_with_preedit_cjk((const char*)buf, &g_pre,
//    composed, sizeof(composed),
//    &ps_b, &pl_b, &caret_b,
//    &ps_c, &pl_c, &caret_c,
//    &total_cols);

//// 2) 画面の最大列を 32 とし、キャレットを常に 8 列目に置く方針
//char view[256];
//int colL, caret_local;
//window_line_by_columns_caret_cjk(
//    composed, total_cols, caret_c,
//    /*max_cols=*///32, /*desired_local_caret_col=*/8,
//    view, sizeof(view),
//    &colL, &caret_local);
// 3) view を描画し、caret_local 列にキャレット表示
//drawTextUTF8(view);
//drawCaretAtColumn(caret_local);
//*/

size_t window_from_caret_simple_cjk(
    const char *s, int total_cols, int caret_col,
    int max_cols, int desired_local_caret_col,
    char *out, size_t out_sz,
    int *out_colL, int *out_caret_local_col
){
    if (!s || !out || out_sz == 0 || max_cols <= 0) {
        if (out && out_sz) out[0]='\0';
        if (out_colL) *out_colL = 0;
        if (out_caret_local_col) *out_caret_local_col = 0;
        return 0;
    }


    int colL,colR;
    if (caret_col<= desired_local_caret_col) {
      colL=0;
      if (total_cols>= colL+max_cols) {
	colR=colL+max_cols+1;
      } else {
	colR=total_cols+1;
      }
    } else {
      // determine start_end window based on caret_col to be desired_local_caret_col location      
      colL=caret_col - desired_local_caret_col-1;
      colR=colL+total_cols+1;
    }

    // 切り出し & ローカル caret
    size_t n = utf8_slice_by_columns_cjk(s, colL, colR, out, out_sz);
    *out_colL=colL;
    *out_caret_local_col = caret_col - colL;
    
    return n;
}
