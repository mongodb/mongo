/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InlineList_h
#define jit_InlineList_h

#include "mozilla/Assertions.h"

namespace js {

template <typename T>
class InlineForwardList;
template <typename T>
class InlineForwardListIterator;

template <typename T>
class InlineForwardListNode {
 public:
  InlineForwardListNode() : next(nullptr) {}
  explicit InlineForwardListNode(InlineForwardListNode<T>* n) : next(n) {}

  InlineForwardListNode(const InlineForwardListNode<T>&) = delete;

 protected:
  friend class InlineForwardList<T>;
  friend class InlineForwardListIterator<T>;

  InlineForwardListNode<T>* next;
};

template <typename T>
class InlineForwardList : protected InlineForwardListNode<T> {
  friend class InlineForwardListIterator<T>;

  using Node = InlineForwardListNode<T>;

  Node* tail_;
#ifdef DEBUG
  int modifyCount_;
#endif

  InlineForwardList<T>* thisFromConstructor() { return this; }

 public:
  InlineForwardList() : tail_(thisFromConstructor()) {
#ifdef DEBUG
    modifyCount_ = 0;
#endif
  }

 public:
  using iterator = InlineForwardListIterator<T>;

 public:
  iterator begin() const { return iterator(this); }
  iterator begin(Node* item) const { return iterator(this, item); }
  iterator end() const { return iterator(nullptr); }
  void removeAt(iterator where) { removeAfter(where.prev, where.iter); }
  void pushFront(Node* t) { insertAfter(this, t); }
  void pushBack(Node* t) {
    MOZ_ASSERT(t->next == nullptr);
#ifdef DEBUG
    modifyCount_++;
#endif
    tail_->next = t;
    tail_ = t;
  }
  T* popFront() {
    MOZ_ASSERT(!empty());
    T* result = static_cast<T*>(this->next);
    removeAfter(this, result);
    return result;
  }
  T* back() const {
    MOZ_ASSERT(!empty());
    return static_cast<T*>(tail_);
  }
  // Move all items from |other| to the end of this list.
  void extendBack(InlineForwardList<T>&& other) {
    MOZ_ASSERT(this != &other);
    if (other.empty()) {
      return;
    }
#ifdef DEBUG
    modifyCount_++;
    other.modifyCount_++;
#endif
    MOZ_ASSERT(tail_->next == nullptr);
    tail_->next = other.next;
    tail_ = other.tail_;
    other.tail_ = &other;
    other.next = nullptr;
    MOZ_ASSERT(!empty());
    MOZ_ASSERT(other.empty());
  }
  void insertAfter(Node* at, Node* item) {
    MOZ_ASSERT(item->next == nullptr);
#ifdef DEBUG
    modifyCount_++;
#endif
    if (at == tail_) {
      tail_ = item;
    }
    item->next = at->next;
    at->next = item;
  }
  void removeAfter(Node* at, Node* item) {
#ifdef DEBUG
    modifyCount_++;
#endif
    if (item == tail_) {
      tail_ = at;
    }
    MOZ_ASSERT(at->next == item);
    at->next = item->next;
    item->next = nullptr;
  }
  void removeAndIncrement(iterator& where) {
    // Do not change modifyCount_ here. The iterator can still be used
    // after calling this method, unlike the other methods that modify
    // the list.
    Node* item = where.iter;
    where.iter = item->next;
    if (item == tail_) {
      tail_ = where.prev;
    }
    MOZ_ASSERT(where.prev->next == item);
    where.prev->next = where.iter;
    item->next = nullptr;
  }
  void splitAfter(Node* at, InlineForwardList<T>* to) {
    MOZ_ASSERT(to->empty());
    if (!at) {
      at = this;
    }
    if (at == tail_) {
      return;
    }
#ifdef DEBUG
    modifyCount_++;
#endif
    to->next = at->next;
    to->tail_ = tail_;
    tail_ = at;
    at->next = nullptr;
  }
  bool empty() const { return tail_ == this; }
  void clear() {
    this->next = nullptr;
    tail_ = this;
#ifdef DEBUG
    modifyCount_ = 0;
#endif
  }
};

template <typename T>
class InlineForwardListIterator {
 private:
  friend class InlineForwardList<T>;

  using Node = InlineForwardListNode<T>;

  explicit InlineForwardListIterator(const InlineForwardList<T>* owner)
      : prev(const_cast<Node*>(static_cast<const Node*>(owner))),
        iter(owner ? owner->next : nullptr)
#ifdef DEBUG
        ,
        owner_(owner),
        modifyCount_(owner ? owner->modifyCount_ : 0)
#endif
  {
  }

  InlineForwardListIterator(const InlineForwardList<T>* owner, Node* node)
      : prev(nullptr),
        iter(node)
#ifdef DEBUG
        ,
        owner_(owner),
        modifyCount_(owner ? owner->modifyCount_ : 0)
#endif
  {
  }

 public:
  InlineForwardListIterator<T>& operator++() {
    MOZ_ASSERT(modifyCount_ == owner_->modifyCount_);
    prev = iter;
    iter = iter->next;
    return *this;
  }
  InlineForwardListIterator<T> operator++(int) {
    InlineForwardListIterator<T> old(*this);
    operator++();
    return old;
  }
  T* operator*() const {
    MOZ_ASSERT(modifyCount_ == owner_->modifyCount_);
    return static_cast<T*>(iter);
  }
  T* operator->() const {
    MOZ_ASSERT(modifyCount_ == owner_->modifyCount_);
    return static_cast<T*>(iter);
  }
  bool operator!=(const InlineForwardListIterator<T>& where) const {
    return iter != where.iter;
  }
  bool operator==(const InlineForwardListIterator<T>& where) const {
    return iter == where.iter;
  }
  explicit operator bool() const { return iter != nullptr; }

 private:
  Node* prev;
  Node* iter;

#ifdef DEBUG
  const InlineForwardList<T>* owner_;
  int modifyCount_;
#endif
};

// AppendOnlyList implements a subset of InlineForwardList which allow appending
// elements to the list while it is being iterated over.
template <typename T>
class AppendOnlyList;
template <typename T>
class AppendOnlyListIterator;

template <typename T>
class AppendOnlyListNode {
 public:
  AppendOnlyListNode() : next(nullptr) {}
  explicit AppendOnlyListNode(AppendOnlyListNode<T>* n) : next(n) {}

  AppendOnlyListNode(const AppendOnlyListNode<T>&) = delete;

 protected:
  friend class AppendOnlyList<T>;
  friend class AppendOnlyListIterator<T>;

  AppendOnlyListNode<T>* next;
};

template <typename T>
class AppendOnlyList : protected AppendOnlyListNode<T> {
  friend class AppendOnlyListIterator<T>;

  using Node = AppendOnlyListNode<T>;

  Node* tail_;
  AppendOnlyList<T>* thisFromConstructor() { return this; }

 public:
  AppendOnlyList() : tail_(thisFromConstructor()) {}

 public:
  using iterator = AppendOnlyListIterator<T>;

 public:
  iterator begin() const { return iterator(this); }
  iterator end() const { return iterator(nullptr); }
  void pushBack(Node* t) {
    MOZ_ASSERT(t->next == nullptr);
    tail_->next = t;
    tail_ = t;
  }
};

template <typename T>
class AppendOnlyListIterator {
 private:
  friend class AppendOnlyList<T>;

  using Node = AppendOnlyListNode<T>;

  explicit AppendOnlyListIterator(const AppendOnlyList<T>* owner)
      : iter(owner ? owner->next : nullptr) {}

 public:
  AppendOnlyListIterator<T>& operator++() {
    iter = iter->next;
    return *this;
  }
  AppendOnlyListIterator<T> operator++(int) {
    AppendOnlyListIterator<T> old(*this);
    operator++();
    return old;
  }
  T* operator*() const { return static_cast<T*>(iter); }
  T* operator->() const { return static_cast<T*>(iter); }
  bool operator!=(const AppendOnlyListIterator<T>& where) const {
    return iter != where.iter;
  }
  bool operator==(const AppendOnlyListIterator<T>& where) const {
    return iter == where.iter;
  }
  explicit operator bool() const { return iter != nullptr; }

 private:
  Node* iter;
};

template <typename T>
class InlineList;
template <typename T>
class InlineListIterator;
template <typename T>
class InlineListReverseIterator;

template <typename T>
class InlineListNode : public InlineForwardListNode<T> {
 public:
  InlineListNode() : InlineForwardListNode<T>(nullptr), prev(nullptr) {}
  InlineListNode(InlineListNode<T>* n, InlineListNode<T>* p)
      : InlineForwardListNode<T>(n), prev(p) {}

  // Move constructor. Nodes may be moved without being removed from their
  // containing lists. For example, this allows list nodes to be safely
  // stored in a resizable Vector -- when the Vector resizes, the new storage
  // is initialized by this move constructor. |other| is a reference to the
  // old node which the |this| node here is replacing.
  InlineListNode(InlineListNode<T>&& other)
      : InlineForwardListNode<T>(other.next) {
    InlineListNode<T>* newNext = static_cast<InlineListNode<T>*>(other.next);
    InlineListNode<T>* newPrev = other.prev;
    prev = newPrev;

    // Update the pointers in the adjacent nodes to point to this node's new
    // location.
    newNext->prev = this;
    newPrev->next = this;
  }

  InlineListNode(const InlineListNode<T>&) = delete;
  void operator=(const InlineListNode<T>&) = delete;

  bool isInList() { return prev != nullptr && this->next != nullptr; }

 protected:
  friend class InlineList<T>;
  friend class InlineListIterator<T>;
  friend class InlineListReverseIterator<T>;

  InlineListNode<T>* prev;
};

template <typename T>
class InlineList : protected InlineListNode<T> {
  using Node = InlineListNode<T>;

 public:
  InlineList() : InlineListNode<T>(this, this) {}

 public:
  using iterator = InlineListIterator<T>;
  using reverse_iterator = InlineListReverseIterator<T>;

 public:
  iterator begin() const { return iterator(static_cast<Node*>(this->next)); }
  iterator begin(const Node* t) const { return iterator(t); }
  iterator end() const { return iterator(this); }
  reverse_iterator rbegin() const { return reverse_iterator(this->prev); }
  reverse_iterator rbegin(const Node* t) const { return reverse_iterator(t); }
  reverse_iterator rend() const { return reverse_iterator(this); }
  void pushFront(Node* t) { insertAfter(this, t); }
  void pushFrontUnchecked(Node* t) { insertAfterUnchecked(this, t); }
  void pushBack(Node* t) { insertBefore(this, t); }
  void pushBackUnchecked(Node* t) { insertBeforeUnchecked(this, t); }
  T* popFront() {
    MOZ_ASSERT(!empty());
    T* t = static_cast<T*>(this->next);
    remove(t);
    return t;
  }
  T* popBack() {
    MOZ_ASSERT(!empty());
    T* t = static_cast<T*>(this->prev);
    remove(t);
    return t;
  }
  T* peekBack() const {
    iterator iter = end();
    iter--;
    return *iter;
  }
  void insertBefore(Node* at, Node* item) {
    MOZ_ASSERT(item->prev == nullptr);
    MOZ_ASSERT(item->next == nullptr);
    insertBeforeUnchecked(at, item);
  }
  void insertBeforeUnchecked(Node* at, Node* item) {
    Node* atPrev = at->prev;
    item->next = at;
    item->prev = atPrev;
    atPrev->next = item;
    at->prev = item;
  }
  void insertAfter(Node* at, Node* item) {
    MOZ_ASSERT(item->prev == nullptr);
    MOZ_ASSERT(item->next == nullptr);
    insertAfterUnchecked(at, item);
  }
  void insertAfterUnchecked(Node* at, Node* item) {
    Node* atNext = static_cast<Node*>(at->next);
    item->next = atNext;
    item->prev = at;
    atNext->prev = item;
    at->next = item;
  }
  void remove(Node* t) {
    Node* tNext = static_cast<Node*>(t->next);
    Node* tPrev = t->prev;
    tPrev->next = tNext;
    tNext->prev = tPrev;
    t->next = nullptr;
    t->prev = nullptr;
  }
  // Remove |old| from the list and insert |now| in its place.
  void replace(Node* old, Node* now) {
    MOZ_ASSERT(now->next == nullptr && now->prev == nullptr);
    Node* listNext = static_cast<Node*>(old->next);
    Node* listPrev = old->prev;
    listPrev->next = now;
    listNext->prev = now;
    now->next = listNext;
    now->prev = listPrev;
    old->next = nullptr;
    old->prev = nullptr;
  }
  void clear() { this->next = this->prev = this; }
  bool empty() const { return begin() == end(); }
  void takeElements(InlineList& l) {
    MOZ_ASSERT(&l != this, "cannot takeElements from this");
    Node* lprev = l.prev;
    static_cast<Node*>(l.next)->prev = this;
    lprev->next = this->next;
    static_cast<Node*>(this->next)->prev = l.prev;
    this->next = l.next;
    l.clear();
  }
};

template <typename T>
class InlineListIterator {
 private:
  friend class InlineList<T>;

  using Node = InlineListNode<T>;

  explicit InlineListIterator(const Node* iter)
      : iter(const_cast<Node*>(iter)) {}

 public:
  InlineListIterator<T>& operator++() {
    iter = static_cast<Node*>(iter->next);
    return *this;
  }
  InlineListIterator<T> operator++(int) {
    InlineListIterator<T> old(*this);
    operator++();
    return old;
  }
  InlineListIterator<T>& operator--() {
    iter = iter->prev;
    return *this;
  }
  InlineListIterator<T> operator--(int) {
    InlineListIterator<T> old(*this);
    operator--();
    return old;
  }
  T* operator*() const { return static_cast<T*>(iter); }
  T* operator->() const { return static_cast<T*>(iter); }
  bool operator!=(const InlineListIterator<T>& where) const {
    return iter != where.iter;
  }
  bool operator==(const InlineListIterator<T>& where) const {
    return iter == where.iter;
  }

 private:
  Node* iter;
};

template <typename T>
class InlineListReverseIterator {
 private:
  friend class InlineList<T>;

  using Node = InlineListNode<T>;

  explicit InlineListReverseIterator(const Node* iter)
      : iter(const_cast<Node*>(iter)) {}

 public:
  InlineListReverseIterator<T>& operator++() {
    iter = iter->prev;
    return *this;
  }
  InlineListReverseIterator<T> operator++(int) {
    InlineListReverseIterator<T> old(*this);
    operator++();
    return old;
  }
  InlineListReverseIterator<T>& operator--() {
    iter = static_cast<Node*>(iter->next);
    return *this;
  }
  InlineListReverseIterator<T> operator--(int) {
    InlineListReverseIterator<T> old(*this);
    operator--();
    return old;
  }
  T* operator*() { return static_cast<T*>(iter); }
  T* operator->() { return static_cast<T*>(iter); }
  bool operator!=(const InlineListReverseIterator<T>& where) const {
    return iter != where.iter;
  }
  bool operator==(const InlineListReverseIterator<T>& where) const {
    return iter == where.iter;
  }

 private:
  Node* iter;
};

template <typename T>
class InlineSpaghettiStack;
template <typename T>
class InlineSpaghettiStackNode;
template <typename T>
class InlineSpaghettiStackIterator;

template <typename T>
class InlineSpaghettiStackNode : public InlineForwardListNode<T> {
  using Parent = InlineForwardListNode<T>;

 public:
  InlineSpaghettiStackNode() : Parent() {}

  explicit InlineSpaghettiStackNode(InlineSpaghettiStackNode<T>* n)
      : Parent(n) {}

  InlineSpaghettiStackNode(const InlineSpaghettiStackNode<T>&) = delete;

 protected:
  friend class InlineSpaghettiStack<T>;
  friend class InlineSpaghettiStackIterator<T>;
};

template <typename T>
class InlineSpaghettiStack : protected InlineSpaghettiStackNode<T> {
  friend class InlineSpaghettiStackIterator<T>;

  using Node = InlineSpaghettiStackNode<T>;

 public:
  InlineSpaghettiStack() = default;

 public:
  using iterator = InlineSpaghettiStackIterator<T>;

 public:
  iterator begin() const { return iterator(this); }
  iterator end() const { return iterator(nullptr); }

  void push(Node* t) {
    MOZ_ASSERT(t->next == nullptr);
    t->next = this->next;
    this->next = t;
  }

  void copy(const InlineSpaghettiStack<T>& stack) { this->next = stack.next; }

  bool empty() const { return this->next == nullptr; }
};

template <typename T>
class InlineSpaghettiStackIterator {
 private:
  friend class InlineSpaghettiStack<T>;

  using Node = InlineSpaghettiStackNode<T>;

  explicit InlineSpaghettiStackIterator(const InlineSpaghettiStack<T>* owner)
      : iter(owner ? static_cast<Node*>(owner->next) : nullptr) {}

 public:
  InlineSpaghettiStackIterator<T>& operator++() {
    iter = static_cast<Node*>(iter->next);
    return *this;
  }
  InlineSpaghettiStackIterator<T> operator++(int) {
    InlineSpaghettiStackIterator<T> old(*this);
    operator++();
    return old;
  }
  T* operator*() const { return static_cast<T*>(iter); }
  T* operator->() const { return static_cast<T*>(iter); }
  bool operator!=(const InlineSpaghettiStackIterator<T>& where) const {
    return iter != where.iter;
  }
  bool operator==(const InlineSpaghettiStackIterator<T>& where) const {
    return iter == where.iter;
  }

 private:
  Node* iter;
};

}  // namespace js

#endif /* jit_InlineList_h */
