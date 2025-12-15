/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_SinglyLinkedList_h
#define ds_SinglyLinkedList_h

#include "mozilla/Assertions.h"

#include <utility>

namespace js {

/*
 * Circular singly linked list that requires only one word per element and for
 * the list itself.
 *
 * Requires T has field |T::next| for the link pointer.
 *
 * The list only stores a pointer to the last element. Since the list is
 * circular, that provides access to the first element and allows insertion at
 * the start and end of the list.
 */
template <typename T>
class SinglyLinkedList {
  T* last_ = nullptr;

 public:
  // Create an empty list.
  SinglyLinkedList() {
    static_assert(std::is_same_v<decltype(T::next), T*>,
                  "SinglyLinkedList requires T has a next field of type T*");
    MOZ_ASSERT(isEmpty());
  }

  // Create a list from an existing non-circular linked list from |first| to
  // |last|.
  SinglyLinkedList(T* first, T* last) {
    MOZ_ASSERT(first);
    MOZ_ASSERT(last);
    MOZ_ASSERT(!last->next);
    last->next = first;
    last_ = last;
    checkContains(first);
    checkContains(last);
  }

  // It's not possible for elements to be present in more than one list, so copy
  // operations are not provided.
  SinglyLinkedList(const SinglyLinkedList& other) = delete;
  SinglyLinkedList& operator=(const SinglyLinkedList& other) = delete;

  SinglyLinkedList(SinglyLinkedList&& other) {
    MOZ_ASSERT(&other != this);
    std::swap(last_, other.last_);
    MOZ_ASSERT(other.isEmpty());
  }
  SinglyLinkedList& operator=(SinglyLinkedList&& other) {
    MOZ_ASSERT(&other != this);
    MOZ_ASSERT(isEmpty());
    std::swap(last_, other.last_);
    return *this;
  }

  ~SinglyLinkedList() { MOZ_ASSERT(isEmpty()); }

  bool isEmpty() const { return !last_; }

  // These return nullptr if the list is empty.
  T* first() const {
    if (isEmpty()) {
      return nullptr;
    }
    T* element = last_->next;
    MOZ_ASSERT(element);
    return element;
  }
  T* last() const { return last_; }

  T* popFront() {
    MOZ_ASSERT(!isEmpty());

    T* element = last_->next;

    if (element == last_) {
      last_ = nullptr;
    } else {
      last_->next = element->next;
    }

    element->next = nullptr;
    return element;
  }
  // popBack cannot be implemented in constant time for a singly linked list.

  void pushFront(T* element) {
    MOZ_ASSERT(!element->next);
    if (isEmpty()) {
      element->next = element;
      last_ = element;
      return;
    }

    element->next = last_->next;
    last_->next = element;
  }
  void pushBack(T* element) {
    pushFront(element);
    moveFrontToBack();
  }
  void moveFrontToBack() {
    MOZ_ASSERT(!isEmpty());
    last_ = last_->next;
    MOZ_ASSERT(!isEmpty());
  }

  void append(SinglyLinkedList&& other) {
    MOZ_ASSERT(&other != this);

    if (other.isEmpty()) {
      return;
    }

    if (isEmpty()) {
      *this = std::move(other);
      return;
    }

    T* firstElement = first();
    last()->next = other.first();
    other.last()->next = firstElement;
    last_ = other.last();
    other.last_ = nullptr;
  }

  void prepend(SinglyLinkedList&& other) {
    MOZ_ASSERT(&other != this);

    if (other.isEmpty()) {
      return;
    }

    if (isEmpty()) {
      *this = std::move(other);
      return;
    }

    T* firstElement = first();
    last()->next = other.first();
    other.last()->next = firstElement;
    other.last_ = nullptr;
  }

  // Remove all elements between |fromExclusive| and |toInclusive|. Return the
  // removed list segment as a non-circular linked list.
  //
  // The fact that the first parameter is exclusive is a requirement for
  // implementing this in constant time for a singly linked list.
  T* removeRange(T* fromExclusive, T* toInclusive) {
    MOZ_ASSERT(fromExclusive);
    MOZ_ASSERT(toInclusive);
    MOZ_ASSERT(fromExclusive != toInclusive);
    MOZ_ASSERT(!isEmpty());

#ifdef DEBUG
    size_t index = 0;
    size_t fromIndex = SIZE_MAX;
    size_t toIndex = SIZE_MAX;
    for (T* element = first(); element; element = element->next) {
      if (element == fromExclusive) {
        fromIndex = index;
      }
      if (element == toInclusive) {
        toIndex = index;
      }
      index++;
      if (index == 100) {
        break;
      }
    }
    if (index < 100) {
      MOZ_ASSERT(fromIndex != SIZE_MAX);
      MOZ_ASSERT(toIndex != SIZE_MAX);
      MOZ_ASSERT(fromIndex < toIndex);
    }
#endif

    T* result = fromExclusive->next;
    fromExclusive->next = toInclusive->next;
    toInclusive->next = nullptr;

    if (last_ == toInclusive) {
      last_ = fromExclusive;
    }

    return result;
  }

  // template <typename T>
  class Iterator {
    T* i = nullptr;
    T* last = nullptr;

   public:
    Iterator() = default;
    explicit Iterator(const SinglyLinkedList& list)
        : i(list.first()), last(list.last()) {}
    Iterator(const SinglyLinkedList& list, T* first)
        : i(first), last(list.last()) {}
    bool done() const { return !i; }
    void next() {
      MOZ_ASSERT(!done());
      i = i == last ? nullptr : i->next;
    }
    T* get() const {
      MOZ_ASSERT(!done());
      return i;
    }

    T& operator*() const { return *get(); }
    T* operator->() const { return get(); }
  };

  Iterator iter() const { return Iterator(*this); }

  Iterator iterFrom(T* fromInclusive) {
    checkContains(fromInclusive);
    return Iterator(*this, fromInclusive);
  }

  void checkContains(T* element) {
#ifdef DEBUG
    size_t i = 0;
    for (Iterator iter(*this); !iter.done(); iter.next()) {
      if (iter.get() == element) {
        return;  // Found.
      }
      i++;
      if (i == 100) {
        return;  // Limit time spent checking.
      }
    }
    MOZ_CRASH("Element not found");
#endif
  }

  // Extracts a non-circular linked list and clears this object.
  T* release() {
    if (isEmpty()) {
      return nullptr;
    }

    T* list = first();
    MOZ_ASSERT(last_->next);
    last_->next = nullptr;
    last_ = nullptr;
    return list;
  }
};

} /* namespace js */

#endif /* ds_SinglyLinkedList_h */
