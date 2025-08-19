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

#ifndef FILE_SETTINGS_H
#define FILE_SETTINGS_H

#include <vector>
#define N_SETTINGS_DICT 41
// --- 型定義 ---
#define DICT_VALUE_TYPE_CHARPTR   0
#define DICT_VALUE_TYPE_INT       1
#define DICT_VALUE_TYPE_FLOAT     2
#define DICT_VALUE_TYPE_CHARARRAY 3

// --- dict_item 構造体 ---
struct dict_item {
    const char *name;
    void *value;
    int value_type;
};

// --- settings_dict ---
//extern std::vector<dict_item> settings_dict;

// --- 型自動判定 ---
template<typename T>
struct ValueTypeOf;

template<> struct ValueTypeOf<int> {
    static constexpr int value = DICT_VALUE_TYPE_INT;
};
template<> struct ValueTypeOf<float> {
    static constexpr int value = DICT_VALUE_TYPE_FLOAT;
};
template<> struct ValueTypeOf<char*> {
    static constexpr int value = DICT_VALUE_TYPE_CHARPTR;
};
template<size_t N> struct ValueTypeOf<char[N]> {
    static constexpr int value = DICT_VALUE_TYPE_CHARARRAY;
};

#define value_type_of(T) (ValueTypeOf<T>::value)

// --- オフセット指定マクロ (グローバル変数OK) ---
#define REGISTER_SETTING_OFFSET(dict, obj, member, offset, type)      \
    do {                                                              \
        dict_item entry;                                              \
        entry.name = #obj #member;                                    \
        entry.value = (void *)((char *)(obj ? &(obj)->member : &member) + (offset)); \
        entry.value_type = type;                                      \
        dict.push_back(entry);                                        \
    } while (0)

// --- オフセット無し (明示型) ---
#define REGISTER_SETTING(dict, obj, member, type)                     \
    REGISTER_SETTING_OFFSET(dict, obj, member, 0, type)

// --- オフセット無し + 型自動判定 ---
#define REGISTER_SETTING_AUTO(dict, obj, member)                      \
    REGISTER_SETTING_OFFSET(dict, obj, member, 0,                     \
        value_type_of<decltype((obj ? (obj)->member : member))>)



extern int n_settings_dict;
extern struct dict_item settings_dict[N_SETTINGS_DICT];

int readline(File *f, char *buf, int term, int size) ;
void init_settings_dict();
int assign_settings(char *line, struct dict_item *dict) ;
int dump_settings(Stream *f, struct dict_item *dict) ;
int load_settings(char *fn) ;
int save_settings(char *fn) ;
void set_grid_locator_information();
#endif
