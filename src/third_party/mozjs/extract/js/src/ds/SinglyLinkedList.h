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
 */
template <typename T>
class SinglyLinkedList {
  T* last_ = nullptr;

 public:
  SinglyLinkedList() {
    static_assert(std::is_same_v<decltype(T::next), T*>,
                  "SinglyLinkedList requires T has a next field of type T*");
    MOZ_ASSERT(isEmpty());
  }

  SinglyLinkedList(T* first, T* last) : last_(last) {
    MOZ_ASSERT(!last_->next);
    last_->next = first;
  }

  // It's not possible for elements to be present in more than one list, so copy
  // operations are not provided.
  SinglyLinkedList(const SinglyLinkedList& other) = delete;
  SinglyLinkedList& operator=(const SinglyLinkedList& other) = delete;

  SinglyLinkedList(SinglyLinkedList&& other) {
    std::swap(last_, other.last_);
    MOZ_ASSERT(other.isEmpty());
  }
  SinglyLinkedList& operator=(SinglyLinkedList&& other) {
    MOZ_ASSERT(isEmpty());
    return *new (this) SinglyLinkedList(std::move(other));
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
  }

  void append(SinglyLinkedList&& other) {
    if (other.isEmpty()) {
      return;
    }

    if (isEmpty()) {
      new (this) SinglyLinkedList(std::move(other));
      return;
    }

    T* firstElement = first();
    last()->next = other.first();
    other.last()->next = firstElement;
    last_ = other.last();
    other.last_ = nullptr;
  }

  // template <typename T>
  class Iterator {
    T* i = nullptr;
    T* last = nullptr;

   public:
    explicit Iterator(const SinglyLinkedList& list)
        : i(list.first()), last(list.last()) {}
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
