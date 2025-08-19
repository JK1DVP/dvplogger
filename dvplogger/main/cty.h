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

#ifndef FILE_CTY_H
#define FILE_CTY_H
// cty.h
struct entity_info {
 const char *entity; 
 const char *entity_desc;
 const char *cqzone;
 const char *ituzone;
 const char *continent;
 const char *lat;
 const char *lon;
 const char *tz;
};
struct prefix_list {
    const char *prefix; 
    const int entity_idx; 
    const int ovrride_info_idx; 
};
extern const struct entity_info entity_infos[347];
extern const char *cty_ovrride_info[86];
extern int prefix_fwd_match_list_idx[36][2];
extern const struct prefix_list prefix_fwd_match_list[7542] ;
extern int prefix_exact_match_list_idx[36][2];
extern const struct prefix_list prefix_exact_match_list[21118];
#endif
