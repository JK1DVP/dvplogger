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
#include "network_arena.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>

namespace {
alignas(4) static uint8_t s_arena[NETWORK_SHARED_ARENA_SIZE];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static NetworkArenaMode s_mode = NETWORK_ARENA_IDLE;
static uint16_t s_telnet_bitmap = 0;
static uint8_t s_telnet_blocks = 0;
static uint32_t s_web_lease_seq = 0;
static uint32_t s_web_active_lease = 0;
static uint32_t s_web_acquired_ms = 0;
static const char *s_web_owner = nullptr;
static constexpr uint32_t WEB_STALE_MS = 30000U;

static void fill_status_locked(NetworkArenaStatus *st, uint32_t now) {
  if (!st) return;
  st->mode = s_mode;
  st->telnet_blocks = s_telnet_blocks;
  st->web_lease = s_web_active_lease;
  st->owner = s_web_owner;
  st->age_ms = (s_mode == NETWORK_ARENA_WEB) ?
      (uint32_t)(now - s_web_acquired_ms) : 0;
}
}

uint8_t *network_arena_data() { return s_arena; }

bool network_arena_web_acquire(const char *owner, uint32_t *lease_out,
                               bool *stale_reclaimed,
                               const char **old_owner,
                               uint32_t *old_age,
                               NetworkArenaStatus *busy_status) {
  const uint32_t now = millis();
  bool acquired = false;
  bool stale = false;
  const char *stale_owner = nullptr;
  uint32_t stale_age = 0;
  uint32_t lease = 0;

  portENTER_CRITICAL(&s_mux);
  if (s_mode == NETWORK_ARENA_WEB) {
    stale_age = (uint32_t)(now - s_web_acquired_ms);
    if (stale_age >= WEB_STALE_MS) {
      stale_owner = s_web_owner;
      s_mode = NETWORK_ARENA_IDLE;
      s_web_active_lease = 0;
      s_web_owner = nullptr;
      stale = true;
    }
  }

  if (s_mode == NETWORK_ARENA_IDLE) {
    s_mode = NETWORK_ARENA_WEB;
    if (++s_web_lease_seq == 0) ++s_web_lease_seq;
    lease = s_web_lease_seq;
    s_web_active_lease = lease;
    s_web_acquired_ms = now;
    s_web_owner = owner;
    acquired = true;
  } else {
    fill_status_locked(busy_status, now);
  }
  portEXIT_CRITICAL(&s_mux);

  if (lease_out) *lease_out = acquired ? lease : 0;
  if (stale_reclaimed) *stale_reclaimed = stale;
  if (old_owner) *old_owner = stale_owner;
  if (old_age) *old_age = stale ? stale_age : 0;
  return acquired;
}

bool network_arena_web_release(uint32_t lease, const char **owner_out,
                               uint32_t *age_out) {
  if (!lease) return false;
  const uint32_t now = millis();
  bool released = false;
  const char *owner = nullptr;
  uint32_t age = 0;

  portENTER_CRITICAL(&s_mux);
  if (s_mode == NETWORK_ARENA_WEB && s_web_active_lease == lease) {
    owner = s_web_owner;
    age = (uint32_t)(now - s_web_acquired_ms);
    s_mode = NETWORK_ARENA_IDLE;
    s_web_active_lease = 0;
    s_web_owner = nullptr;
    released = true;
  }
  portEXIT_CRITICAL(&s_mux);

  if (owner_out) *owner_out = owner;
  if (age_out) *age_out = age;
  return released;
}

uint8_t *network_arena_telnet_acquire(size_t len) {
  if (len == 0 || len > NETWORK_ARENA_TELNET_BLOCK_SIZE) return nullptr;
  uint8_t *ret = nullptr;

  portENTER_CRITICAL(&s_mux);
  if (s_mode == NETWORK_ARENA_IDLE || s_mode == NETWORK_ARENA_TELNET) {
    for (size_t i = 0; i < NETWORK_ARENA_TELNET_BLOCKS; ++i) {
      const uint16_t bit = (uint16_t)(1U << i);
      if ((s_telnet_bitmap & bit) == 0) {
        s_telnet_bitmap |= bit;
        ++s_telnet_blocks;
        s_mode = NETWORK_ARENA_TELNET;
        ret = s_arena + i * NETWORK_ARENA_TELNET_BLOCK_SIZE;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&s_mux);
  return ret;
}

void network_arena_telnet_release(uint8_t *ptr) {
  if (!ptr) return;
  const uintptr_t base = (uintptr_t)s_arena;
  const uintptr_t p = (uintptr_t)ptr;
  if (p < base || p >= base + NETWORK_SHARED_ARENA_SIZE) return;
  const size_t off = (size_t)(p - base);
  if ((off % NETWORK_ARENA_TELNET_BLOCK_SIZE) != 0) return;
  const size_t idx = off / NETWORK_ARENA_TELNET_BLOCK_SIZE;
  if (idx >= NETWORK_ARENA_TELNET_BLOCKS) return;

  portENTER_CRITICAL(&s_mux);
  const uint16_t bit = (uint16_t)(1U << idx);
  if (s_telnet_bitmap & bit) {
    s_telnet_bitmap &= (uint16_t)~bit;
    if (s_telnet_blocks) --s_telnet_blocks;
    if (s_telnet_blocks == 0 && s_mode == NETWORK_ARENA_TELNET)
      s_mode = NETWORK_ARENA_IDLE;
  }
  portEXIT_CRITICAL(&s_mux);
}

void network_arena_get_status(NetworkArenaStatus *status) {
  if (!status) return;
  const uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  fill_status_locked(status, now);
  portEXIT_CRITICAL(&s_mux);
}
