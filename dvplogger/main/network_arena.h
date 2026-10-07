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
#ifndef DVPLOGGER_NETWORK_ARENA_H
#define DVPLOGGER_NETWORK_ARENA_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t NETWORK_SHARED_ARENA_SIZE = 6144;
static constexpr size_t NETWORK_ARENA_TELNET_BLOCK_SIZE = 512;
static constexpr size_t NETWORK_ARENA_TELNET_BLOCKS =
    NETWORK_SHARED_ARENA_SIZE / NETWORK_ARENA_TELNET_BLOCK_SIZE;

enum NetworkArenaMode : uint8_t {
  NETWORK_ARENA_IDLE = 0,
  NETWORK_ARENA_WEB = 1,
  NETWORK_ARENA_TELNET = 2,
};

struct NetworkArenaStatus {
  NetworkArenaMode mode;
  uint8_t telnet_blocks;
  uint32_t web_lease;
  uint32_t age_ms;
  const char *owner;
};

// Physical shared workspace.  Web owns the whole arena exclusively; Telnet
// divides it into fixed 512-byte blocks.  These two modes never overlap.
uint8_t *network_arena_data();

bool network_arena_web_acquire(const char *owner, uint32_t *lease_out,
                               bool *stale_reclaimed,
                               const char **old_owner,
                               uint32_t *old_age,
                               NetworkArenaStatus *busy_status);
bool network_arena_web_release(uint32_t lease, const char **owner_out,
                               uint32_t *age_out);

uint8_t *network_arena_telnet_acquire(size_t len);
void network_arena_telnet_release(uint8_t *ptr);

void network_arena_get_status(NetworkArenaStatus *status);

#endif
