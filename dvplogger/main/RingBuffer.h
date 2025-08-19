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

#pragma once
#include <cstddef>

template<typename T>
class RingBuffer {
public:
  // 🔥 デフォルトコンストラクタ: デフォルトサイズ 1024 要素
 RingBuffer(size_t capacity = 1024)
   : capacity_(capacity), head_(0), tail_(0) {
    buffer_ = new T[capacity_];
  }
  
  // RingBuffer(size_t capacity)
  //        : capacity_(capacity), head_(0), tail_(0) {
  //        buffer_ = new T[capacity_];
  //    }

    ~RingBuffer() {
        delete[] buffer_;
    }

    bool push(const T &item) {
        size_t next = (head_ + 1) % capacity_;
        if (next == tail_) return false; // Full
        buffer_[head_] = item;
        head_ = next;
        return true;
    }

    bool pop(T &item) {
        if (empty()) return false;
        item = buffer_[tail_];
        tail_ = (tail_ + 1) % capacity_;
        return true;
    }

    bool peek(T &item, size_t offset=0) {
        if (offset >= count()) return false;
        size_t pos = (tail_ + offset) % capacity_;
        item = buffer_[pos];
        return true;
    }

    size_t count() const {
        return (head_ + capacity_ - tail_) % capacity_;
    }

    bool empty() const {
        return head_ == tail_;
    }

    bool full() const {
        return ((head_ + 1) % capacity_) == tail_;
    }

    void clear() {
        head_ = tail_ = 0;
    }

private:
    T *buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
};
