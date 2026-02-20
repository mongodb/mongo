/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_PriorityQueue_h
#define ds_PriorityQueue_h

#include "js/Vector.h"

namespace js {

/*
 * Class which represents a heap based priority queue using a vector.
 * Inserting elements and removing the highest priority one are both O(log n).
 *
 * Template parameters are the same as for Vector, with the addition that P
 * must have a static higherPriority(const T& a, const T& b) method which
 * returns true if |a| has a higher priority than |b|.
 */
template <class T, class P, size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy>
class PriorityQueue {
  Vector<T, MinInlineCapacity, AllocPolicy> heap;

  PriorityQueue(const PriorityQueue&) = delete;
  PriorityQueue& operator=(const PriorityQueue&) = delete;

 public:
  explicit PriorityQueue(AllocPolicy ap = AllocPolicy())
      : heap(std::move(ap)) {}

  [[nodiscard]] bool reserve(size_t capacity) { return heap.reserve(capacity); }

  size_t length() const { return heap.length(); }

  bool empty() const { return heap.empty(); }

  // highest and popHighest are used to enforce necessary move semantics for
  // working with UniquePtrs in a queue, and should be used together Example:
  //   UniquePtr<...> x = std::move(queue.highest());
  //   queue.popHighest();
  T& highest() {
    MOZ_ASSERT(!empty());
    return heap[0];
  }

  void popHighest() {
    if (heap.length() == 1) {
      heap.popBack();
      return;
    }
    std::swap(heap[0], heap.back());
    heap.popBack();
    siftDown(0);
  }

  // removeHighest cannot be used with UniquePtrs, and should only be used for
  // other datatypes.
  T removeHighest() {
    T highest = heap[0];
    T last = heap.popCopy();
    if (!heap.empty()) {
      heap[0] = last;
      siftDown(0);
    }
    return highest;
  }

  [[nodiscard]] bool insert(T&& v) {
    if (!heap.append(std::move(v))) {
      return false;
    }
    siftUp(heap.length() - 1);
    return true;
  }

  void infallibleInsert(T&& v) {
    heap.infallibleAppend(std::move(v));
    siftUp(heap.length() - 1);
  }

 private:
  /*
   * Elements of the vector encode a binary tree:
   *
   *      0
   *    1   2
   *   3 4 5 6
   *
   * The children of element N are (2N + 1) and (2N + 2).
   * The parent of element N is (N - 1) / 2.
   *
   * Each element has higher priority than its children.
   */

  void siftDown(size_t n) {
    while (true) {
      size_t left = n * 2 + 1;
      size_t right = n * 2 + 2;

      if (left < heap.length()) {
        if (right < heap.length()) {
          if (P::higherPriority(heap[right], heap[n]) &&
              P::higherPriority(heap[right], heap[left])) {
            swap(n, right);
            n = right;
            continue;
          }
        }

        if (P::higherPriority(heap[left], heap[n])) {
          swap(n, left);
          n = left;
          continue;
        }
      }

      break;
    }
  }

  void siftUp(size_t n) {
    while (n > 0) {
      size_t parent = (n - 1) / 2;

      if (P::higherPriority(heap[parent], heap[n])) {
        break;
      }

      swap(n, parent);
      n = parent;
    }
  }

  void swap(size_t a, size_t b) { std::swap(heap[a], heap[b]); }
};

} /* namespace js */

#endif /* ds_PriorityQueue_h */
