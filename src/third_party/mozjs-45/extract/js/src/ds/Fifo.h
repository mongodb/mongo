/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Fifo_h
#define js_Fifo_h

#include "mozilla/Move.h"

#include "js/Utility.h"
#include "js/Vector.h"

namespace js {

// A first-in-first-out queue container type. Fifo calls constructors and
// destructors of all elements added so non-PODs may be used safely. |Fifo|
// stores the first |MinInlineCapacity| elements in-place before resorting to
// dynamic allocation.
//
// T requirements:
//  - Either movable or copyable.
// MinInlineCapacity requirements:
//  - Must be even.
// AllocPolicy:
//  - see "Allocation policies" in AllocPolicy.h
template <typename T,
          size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy>
class Fifo
{
    static_assert(MinInlineCapacity % 2 == 0, "MinInlineCapacity must be even!");

  protected:
    // An element A is "younger" than an element B if B was inserted into the
    // |Fifo| before A was.
    //
    // Invariant 1: Every element within |front_| is younger than every element
    // within |rear_|.
    // Invariant 2: Entries within |front_| are sorted from younger to older.
    // Invariant 3: Entries within |rear_| are sorted from older to younger.
    // Invariant 4: If the |Fifo| is not empty, then |front_| is not empty.
    Vector<T, MinInlineCapacity / 2, AllocPolicy> front_;
    Vector<T, MinInlineCapacity / 2, AllocPolicy> rear_;

  private:
    // Maintain invariants after adding or removing entries.
    bool fixup() {
        if (!front_.empty())
            return true;

        if (!front_.reserve(rear_.length()))
            return false;

        while (!rear_.empty()) {
            front_.infallibleAppend(mozilla::Move(rear_.back()));
            rear_.popBack();
        }

        return true;
    }

  public:
    explicit Fifo(AllocPolicy alloc = AllocPolicy())
        : front_(alloc)
        , rear_(alloc)
    { }

    Fifo(Fifo&& rhs)
        : front_(mozilla::Move(rhs.front_))
        , rear_(mozilla::Move(rhs.rear_))
    { }

    Fifo& operator=(Fifo&& rhs) {
        MOZ_ASSERT(&rhs != this, "self-move disallowed");
        this->~Fifo();
        new (this) Fifo(mozilla::Move(rhs));
        return *this;
    }

    Fifo(const Fifo&) = delete;
    Fifo& operator=(const Fifo&) = delete;

    size_t length() const {
        MOZ_ASSERT_IF(rear_.length() > 0, front_.length() > 0); // Invariant 4.
        return front_.length() + rear_.length();
    }

    bool empty() const {
        MOZ_ASSERT_IF(rear_.length() > 0, front_.length() > 0); // Invariant 4.
        return front_.empty();
    }

    // Push an element to the back of the queue. This method can take either a
    // |const T&| or a |T&&|.
    template <typename U>
    bool pushBack(U&& u) {
        if (!rear_.append(mozilla::Forward<U>(u)))
            return false;
        if (!fixup()) {
            rear_.popBack();
            return false;
        }
        return true;
    }

    // Construct a T in-place at the back of the queue.
    template <typename... Args>
    bool emplaceBack(Args&&... args) {
        if (!rear_.emplaceBack(mozilla::Forward<Args>(args)...))
            return false;
        if (!fixup()) {
            rear_.popBack();
            return false;
        }
        return true;
    }

    // Access the element at the front of the queue.
    T& front() {
        MOZ_ASSERT(!empty());
        return front_.back();
    }
    const T& front() const {
        MOZ_ASSERT(!empty());
        return front_.back();
    }

    // Remove the front element from the queue.
    bool popFront() {
        MOZ_ASSERT(!empty());
        T t(mozilla::Move(front()));
        front_.popBack();
        if (!fixup()) {
            // Attempt to remain in a valid state by reinserting the element
            // back at the front. If we can't remain in a valid state in the
            // face of OOMs, crash.
            AutoEnterOOMUnsafeRegion oomUnsafe;
            if (!front_.append(mozilla::Move(t)))
                oomUnsafe.crash("js::Fifo::popFront");
            return false;
        }
        return true;
    }

    // Clear all elements from the queue.
    void clear() {
        front_.clear();
        rear_.clear();
    }
};

} // namespace js

#endif /* js_Fifo_h */
