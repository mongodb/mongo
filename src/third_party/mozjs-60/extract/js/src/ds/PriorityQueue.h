/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
 * must have a static priority(const T&) method which returns higher numbers
 * for higher priority elements.
 */
template <class T, class P,
          size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy>
class PriorityQueue
{
    Vector<T, MinInlineCapacity, AllocPolicy> heap;

    PriorityQueue(const PriorityQueue&) = delete;
    PriorityQueue& operator=(const PriorityQueue&) = delete;

  public:

    explicit PriorityQueue(AllocPolicy ap = AllocPolicy())
      : heap(ap)
    {}

    MOZ_MUST_USE bool reserve(size_t capacity) {
        return heap.reserve(capacity);
    }

    size_t length() const {
        return heap.length();
    }

    bool empty() const {
        return heap.empty();
    }

    T removeHighest() {
        T highest = heap[0];
        T last = heap.popCopy();
        if (!heap.empty()) {
            heap[0] = last;
            siftDown(0);
        }
        return highest;
    }

    MOZ_MUST_USE bool insert(const T& v) {
        if (!heap.append(v))
            return false;
        siftUp(heap.length() - 1);
        return true;
    }

    void infallibleInsert(const T& v) {
        heap.infallibleAppend(v);
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
                    if (P::priority(heap[n]) < P::priority(heap[right]) &&
                        P::priority(heap[left]) < P::priority(heap[right]))
                    {
                        swap(n, right);
                        n = right;
                        continue;
                    }
                }

                if (P::priority(heap[n]) < P::priority(heap[left])) {
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

            if (P::priority(heap[parent]) > P::priority(heap[n]))
                break;

            swap(n, parent);
            n = parent;
        }
    }

    void swap(size_t a, size_t b) {
        T tmp = heap[a];
        heap[a] = heap[b];
        heap[b] = tmp;
    }
};

}  /* namespace js */

#endif /* ds_PriorityQueue_h */
