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

#ifndef FILE_EDIT_BUF_H
#define FILE_EDIT_BUF_H
void backspace_buf(char *buf) ;
void delete_buf(char *buf) ;
void left_buf(char *buf) ;
void right_buf(char *buf);
// cp は 1コードポイント（1〜4B）、cp_len はそのバイト数
bool insert_buf_utf8(const char *cp, int cp_len, char *cbuf);
void insert_buf(char c, char *buf) ;
bool overwrite_buf_utf8(const char *cp, int cp_len, char *cbuf);
void overwrite_buf(char c, char *buf) ;
void clear_buf(char *p) ;
void init_buf(char *p, int siz) ;
void adjust_cursor_buf(char *buf);

typedef struct {
  char s[16];
  int  len;
} Preedit;

extern Preedit g_pre ;
extern void flush_preedit(char *buf);
void romaji_input_char(char ch, char *buf);
void romaji_backspace(char *buf);
void romaji_move_left(char *buf);
void romaji_move_right(char *buf);
bool compose_line_with_preedit(
    const char *cbuf,
    const Preedit *pre,
    char *out, size_t out_sz,
    int *out_preedit_start, int *out_preedit_len,
    int *out_caret_byte  // out 内のバイト位置
			       );
bool compose_marked_line(const char *cbuf, const Preedit *pre,
                         char *out, size_t out_sz);
size_t utf8_substr_range(const char *s, int i, int j,
                         char *out, size_t out_size) ;

int utf8_display_width_upto_cjk(const char *s, int i_chars) ;

/* 結果パック：target_cols に収まる最大の文字位置を返す */
typedef struct {
    int char_index;    /* 先頭からの文字数（コードポイント数）。[0..total_chars] */
    int byte_offset;   /* s 先頭からのバイト位置（char_index の先頭） */
    int display_cols;  /* 先頭から char_index 文字ぶんの累積列幅 (<= target_cols) */
    int total_chars;   /* 文字列の総文字数 */
    int total_cols;    /* 文字列全体の列幅（参考） */
    int next_char_cols;/* 次の1文字の幅（末尾なら0） */
    int at_exact;      /* ちょうど target_cols に一致したら1、超過せずに最大なら0 */
} Utf8PosAtColCJK;

Utf8PosAtColCJK utf8_pos_at_column_cjk(const char *s, int target_cols) ;


/**
 * 列幅レンジ [colL, colR) を「文字境界」で切り出す安全な [i,j) を求める。
 * 返り値：書き出したバイト数（out に UTF-8 で格納）。常に NUL 終端。
 */

size_t utf8_slice_by_columns_cjk(const char *s, int colL, int colR,
                                 char *out, size_t out_sz) ;

/**
 * 本文 buf と preedit を連結した「表示用ライン」を out に構築。
 * さらに、preedit/ caret の「バイト位置」と「列幅位置」、そして **行全体の列幅** を返す。
 *
 * buf: [0]=capacity(bytes), [1]=cursor(文字数; code point), [2..]=UTF-8 本文
 * pre: ローマ字プレエディット（ASCII/UTF-8可）
 * out: 連結した UTF-8 文字列（[LEFT][PREEDIT][RIGHT]）
 * out_sz: out の容量（NUL込み）※ out が小さい場合は右側を切り詰めます
 *
 * 出力:
 *  - out_preedit_start_byte / out_preedit_len_bytes / out_caret_byte … out 内のバイト位置
 *  - out_preedit_start_cols / out_preedit_len_cols / out_caret_cols … CJK=2 の列幅
 *  - out_total_cols … 行全体の列幅（**out の切り詰めに関係なく、論理的な総幅**）
 *
 * 戻り値: true=成功（truncation の可能性あり）/ false=引数不正
 */
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
				   );



size_t window_line_by_columns_caret_cjk(
    const char *s, int total_cols, int caret_col,
    int max_cols, int desired_local_caret_col,
    char *out, size_t out_sz,
    int *out_colL, int *out_caret_local_col
					);

size_t window_from_caret_simple_cjk(
    const char *s, int total_cols, int caret_col,
    int max_cols, int desired_local_caret_col,
    char *out, size_t out_sz,
    int *out_colL, int *out_caret_local_col
				    );

#endif
