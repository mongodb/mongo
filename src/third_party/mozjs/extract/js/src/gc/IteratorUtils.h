/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_IteratorUtils_h
#define gc_IteratorUtils_h

#include "mozilla/Array.h"
#include "mozilla/Maybe.h"

#include <initializer_list>

namespace js {

/*
 * Create an iterator that yields the values from IteratorB(a) for all a in
 * IteratorA(). Equivalent to nested for loops over IteratorA and IteratorB
 * where IteratorB is constructed with a value from IteratorA.
 */
template <typename IteratorA, typename IteratorB>
class NestedIterator {
  using T = decltype(std::declval<IteratorB>().get());

  IteratorA a;
  mozilla::Maybe<IteratorB> b;

 public:
  template <typename... Args>
  explicit NestedIterator(Args&&... args) : a(std::forward<Args>(args)...) {
    settle();
  }

  bool done() const { return b.isNothing(); }

  T get() const {
    MOZ_ASSERT(!done());
    return b.ref().get();
  }

  void next() {
    MOZ_ASSERT(!done());
    b->next();
    if (b->done()) {
      b.reset();
      a.next();
      settle();
    }
  }

  const IteratorB& ref() const { return *b; }

  operator T() const { return get(); }

  T operator->() const { return get(); }

 private:
  void settle() {
    MOZ_ASSERT(b.isNothing());
    while (!a.done()) {
      b.emplace(a.get());
      if (!b->done()) {
        break;
      }
      b.reset();
      a.next();
    }
  }
};

/*
 * An iterator the yields values from each of N of instances of Iterator in
 * sequence.
 */
template <typename Iterator, size_t N>
class ChainedIterator {
  using T = decltype(std::declval<Iterator>().get());

  mozilla::Array<Iterator, N> iterators;
  size_t index = 0;

 public:
  template <typename... Args>
  MOZ_IMPLICIT ChainedIterator(Args&&... args)
      : iterators(Iterator(std::forward<Args>(args))...) {
    static_assert(N > 1);
    settle();
  }

  bool done() const { return index == N; }

  void next() {
    MOZ_ASSERT(!done());
    iterators[index].next();
    settle();
  }

  T get() const {
    MOZ_ASSERT(!done());
    return iterators[index].get();
  }

  operator T() const { return get(); }
  T operator->() const { return get(); }

 private:
  void settle() {
    MOZ_ASSERT(!done());
    while (iterators[index].done()) {
      index++;
      if (done()) {
        break;
      }
    }
  }
};

} /* namespace js */

#endif  // gc_IteratorUtils_h
