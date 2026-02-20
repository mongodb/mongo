/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The classes SlimLinkedList<T> and SlimLinkedListElement<T> provide a
 * type-safe doubly-linked list class which uses one word for the list and two
 * words for each element (for comparison mozilla::LinkedList uses three words
 * for the list and for each element due to padding).
 *
 * This aims to be a replacement for mozilla::LinkedList although the interface
 * is not identical. In particular most actions are implemented as methods on
 * the list itself as opposed to the element.
 *
 * Private element inheritance is not supported; clients must publicly derive
 * from LinkedListElement.
 */

#ifndef ds_SlimLinkedList_h
#define ds_SlimLinkedList_h

#include "mozilla/Assertions.h"

#include <algorithm>
#include <utility>

namespace js {

template <typename T>
class SlimLinkedListElement;

template <typename T>
class SlimLinkedList;

template <typename T>
class SlimLinkedListElement {
  using ElementPtr = T*;
  using ConstElementPtr = const T*;

  // Tag bit used to indicate the start/end of the list. The tag is set on the
  // prev_ pointer of the first node and the next_ pointer of the last node in
  // the list.
  static constexpr uintptr_t EndTag = 1;

  uintptr_t next_ = 0;
  uintptr_t prev_ = 0;

  friend class js::SlimLinkedList<T>;

  static uintptr_t UntaggedPtr(ElementPtr ptr) {
    MOZ_ASSERT((uintptr_t(ptr) & EndTag) == 0);
    return uintptr_t(ptr);
  }
  static uintptr_t GetTag(uintptr_t taggedPtr) { return taggedPtr & EndTag; }
  static ElementPtr GetPtr(uintptr_t taggedPtr) {
    return reinterpret_cast<ElementPtr>(uintptr_t(taggedPtr) & ~EndTag);
  }
  static ConstElementPtr GetConstPtr(uintptr_t taggedPtr) {
    return reinterpret_cast<ConstElementPtr>(uintptr_t(taggedPtr) & ~EndTag);
  }

  static void LinkElements(ElementPtr a, ElementPtr b, uintptr_t maybeTag = 0) {
    MOZ_ASSERT((maybeTag & ~EndTag) == 0);
    a->next_ = UntaggedPtr(b) | maybeTag;
    b->prev_ = UntaggedPtr(a) | maybeTag;
  }

 public:
  SlimLinkedListElement() = default;

  ~SlimLinkedListElement() { MOZ_ASSERT(!isInList()); }

  SlimLinkedListElement(const SlimLinkedListElement<T>& other) = delete;
  SlimLinkedListElement& operator=(const SlimLinkedListElement<T>& other) =
      delete;

  // Don't allow moving elements that are part of a list.
  SlimLinkedListElement(SlimLinkedListElement<T>&& other) {
    MOZ_ASSERT(this != &other);
    MOZ_ASSERT(!isInList());
    MOZ_ASSERT(!other.isInList());
  }
  SlimLinkedListElement& operator=(SlimLinkedListElement<T>&& other) {
    MOZ_ASSERT(this != &other);
    MOZ_ASSERT(!isInList());
    MOZ_ASSERT(!other.isInList());
    return *this;
  }

  bool isInList() const {
    MOZ_ASSERT(bool(next_) == bool(prev_));
    return next_;
  }

  bool isLast() const {
    MOZ_ASSERT(isInList());
    return GetTag(next_);
  }

  bool isFirst() const {
    MOZ_ASSERT(isInList());
    return GetTag(prev_);
  }

  ElementPtr getNext() { return isLast() ? nullptr : getNextUnchecked(); }
  ConstElementPtr getNext() const {
    return isLast() ? nullptr : getNextUnchecked();
  }

  ElementPtr getPrev() { return isFirst() ? nullptr : getPrevUnchecked(); }
  ConstElementPtr getPrev() const {
    return isFirst() ? nullptr : getPrevUnchecked();
  }

 private:
  ElementPtr getNextUnchecked() { return GetPtr(next_); }
  ConstElementPtr getNextUnchecked() const { return GetConstPtr(next_); };
  ElementPtr getPrevUnchecked() { return GetPtr(prev_); }
  ConstElementPtr getPrevUnchecked() const { return GetConstPtr(prev_); };

  ElementPtr thisElement() { return static_cast<ElementPtr>(this); }

  void makeSingleton() {
    MOZ_ASSERT(!isInList());
    LinkElements(thisElement(), thisElement(), EndTag);
  }

  void insertAfter(ElementPtr newElement) {
    insertListAfter(newElement, newElement);
  }

  /*
   * Insert the list of elements from |listFirst| to |listLast| between |this|
   * and the next element |next|. Any tag goes between |listLast| and |next|.
   */
  void insertListAfter(ElementPtr listFirst, ElementPtr listLast) {
    MOZ_ASSERT(isInList());
    MOZ_ASSERT_IF(listFirst != listLast,
                  listFirst->getPrevUnchecked() == listLast);
    MOZ_ASSERT_IF(listFirst != listLast,
                  listLast->getNextUnchecked() == listFirst);

    ElementPtr next = GetPtr(next_);
    uintptr_t tag = GetTag(next_);

    LinkElements(thisElement(), listFirst);
    LinkElements(listLast, next, tag);
  }

  void insertBefore(ElementPtr newElement) {
    insertListBefore(newElement, newElement);
  }

  /*
   * Insert the list of elements from |listFirst| to |listLast| between the
   * previous element |prev| and |this|. Any tag goes between |prev| and
   * |listFirst|.
   */
  void insertListBefore(ElementPtr listFirst, ElementPtr listLast) {
    MOZ_ASSERT(isInList());
    MOZ_ASSERT_IF(listFirst != listLast,
                  listFirst->getPrevUnchecked() == listLast);
    MOZ_ASSERT_IF(listFirst != listLast,
                  listLast->getNextUnchecked() == listFirst);

    ElementPtr prev = GetPtr(prev_);
    uintptr_t tag = GetTag(prev_);

    LinkElements(prev, listFirst, tag);
    LinkElements(listLast, thisElement());
  }

  /*
   * Remove element |this| from its containing list.
   */
  void remove() {
    MOZ_ASSERT(isInList());

    ElementPtr prev = GetPtr(prev_);
    ElementPtr next = GetPtr(next_);
    uintptr_t tag = GetTag(prev_) | GetTag(next_);

    LinkElements(prev, next, tag);

    next_ = 0;
    prev_ = 0;
  }
};

template <typename T>
class SlimLinkedList {
  using ElementPtr = T*;
  using ConstElementPtr = const T*;

  ElementPtr first_ = nullptr;

 public:
  template <typename Type>
  class Iterator {
    Type current_;

   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    explicit Iterator(Type current) : current_(current) {}

    Type operator*() const { return current_; }

    const Iterator& operator++() {
      current_ = current_->getNext();
      return *this;
    }

    bool operator==(const Iterator& other) const {
      return current_ == other.current_;
    }
    bool operator!=(const Iterator& other) const { return !(*this == other); }
  };

  SlimLinkedList() = default;

  SlimLinkedList(const SlimLinkedList<T>& other) = delete;
  SlimLinkedList& operator=(const SlimLinkedList<T>& other) = delete;

  SlimLinkedList(SlimLinkedList<T>&& other) {
    MOZ_ASSERT(this != &other);
    MOZ_ASSERT(isEmpty());
    std::swap(first_, other.first_);
  }
  SlimLinkedList& operator=(SlimLinkedList<T>&& other) {
    MOZ_ASSERT(this != &other);
    MOZ_ASSERT(isEmpty());
    std::swap(first_, other.first_);
    return *this;
  }

  ~SlimLinkedList() { MOZ_ASSERT(isEmpty()); }

  /*
   * Add |newElement| to the front of the list.
   */
  void pushFront(ElementPtr newElement) {
    if (isEmpty()) {
      newElement->makeSingleton();
    } else {
      first_->insertBefore(newElement);
    }
    first_ = newElement;
  }

  /*
   * Add |newElement| to the back of the list.
   */
  void pushBack(ElementPtr newElement) {
    if (isEmpty()) {
      newElement->makeSingleton();
      first_ = newElement;
      return;
    }

    getLast()->insertAfter(newElement);
  }

  /*
   * Move all elements from list |other| to the end of this list. |other| is
   * left empty.
   */
  void append(SlimLinkedList<T>&& other) {
    MOZ_ASSERT(this != &other);
    if (other.isEmpty()) {
      return;
    }

    if (isEmpty()) {
      *this = std::move(other);
      return;
    }

    getLast()->insertListAfter(other.getFirst(), other.getLast());
    other.first_ = nullptr;
  }

  /*
   * Move all elements from list |other| to the start of this list. |other| is
   * left empty.
   */
  void prepend(SlimLinkedList<T>&& other) {
    MOZ_ASSERT(this != &other);
    if (other.isEmpty()) {
      return;
    }

    if (isEmpty()) {
      *this = std::move(other);
      return;
    }

    getFirst()->insertListBefore(other.getFirst(), other.getLast());
    first_ = other.first_;
    other.first_ = nullptr;
  }

  /*
   * Get the first element of the list, or nullptr if the list is empty.
   */
  ElementPtr getFirst() { return first_; }
  ConstElementPtr getFirst() const { return first_; }

  /*
   * Get the last element of the list, or nullptr if the list is empty.
   */
  ElementPtr getLast() {
    return isEmpty() ? nullptr : first_->getPrevUnchecked();
  }
  ConstElementPtr getLast() const {
    return isEmpty() ? nullptr : first_->getPrevUnchecked();
  }

  /*
   * Get and remove the first element of the list. If the list is empty, return
   * nullptr.
   */
  ElementPtr popFirst() {
    if (isEmpty()) {
      return nullptr;
    }

    ElementPtr result = first_;
    first_ = result->getNext();
    result->remove();
    return result;
  }

  /*
   * Get and remove the last element of the list. If the list is empty, return
   * nullptr.
   */
  ElementPtr popLast() {
    if (isEmpty()) {
      return nullptr;
    }

    ElementPtr result = getLast();
    if (result == first_) {
      first_ = nullptr;
    }
    result->remove();
    return result;
  }

  /*
   * Return true if the list is empty, or false otherwise.
   */
  bool isEmpty() const { return !first_; }

  /*
   * Returns whether the given element is in the list.
   */
  bool contains(ConstElementPtr aElm) const {
    return std::find(begin(), end(), aElm) != end();
  }

  /*
   * Remove |element| from this list.
   */
  void remove(ElementPtr element) {
    checkContains(element);
    if (element == first_) {
      first_ = element->getNext();
    }
    element->remove();
  }

  void checkContains(ElementPtr element) {
#ifdef DEBUG
    size_t i = 0;
    for (const auto& e : *this) {
      if (e == element) {
        return;  // Found.
      }
      if (i == 100) {
        return;  // Limit time spent checking.
      }
    }
    MOZ_CRASH("Element not found");
#endif
  }

  /*
   * Remove all the elements from the list.
   *
   * This runs in time linear to the list's length, because we have to mark
   * each element as not in the list.
   */
  void clear() {
    while (popFirst()) {
    }
  }

  /*
   * Remove all the elements from the list, calling |func| on each one first. On
   * return the list is empty.
   */
  template <typename F>
  void drain(F&& func) {
    while (ElementPtr element = popFirst()) {
      func(element);
    }
  }

  /**
   * Return the length of elements in the list.
   */
  size_t length() const { return std::distance(begin(), end()); }

  /*
   * Allow range-based iteration:
   *
   *     for (MyElementPtr* elt : myList) { ... }
   */
  Iterator<ElementPtr> begin() { return Iterator<ElementPtr>(getFirst()); }
  Iterator<ConstElementPtr> begin() const {
    return Iterator<ConstElementPtr>(getFirst());
  }
  Iterator<ElementPtr> end() { return Iterator<ElementPtr>(nullptr); }
  Iterator<ConstElementPtr> end() const {
    return Iterator<ConstElementPtr>(nullptr);
  }
};

} /* namespace js */

#endif /* ds_SlimLinkedList_h */
