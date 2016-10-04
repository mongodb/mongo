/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A type-safe doubly-linked list class. */

/*
 * The classes LinkedList<T> and LinkedListElement<T> together form a
 * convenient, type-safe doubly-linked list implementation.
 *
 * The class T which will be inserted into the linked list must inherit from
 * LinkedListElement<T>.  A given object may be in only one linked list at a
 * time.
 *
 * A LinkedListElement automatically removes itself from the list upon
 * destruction, and a LinkedList will fatally assert in debug builds if it's
 * non-empty when it's destructed.
 *
 * For example, you might use LinkedList in a simple observer list class as
 * follows.
 *
 *   class Observer : public LinkedListElement<Observer>
 *   {
 *   public:
 *     void observe(char* aTopic) { ... }
 *   };
 *
 *   class ObserverContainer
 *   {
 *   private:
 *     LinkedList<Observer> list;
 *
 *   public:
 *     void addObserver(Observer* aObserver)
 *     {
 *       // Will assert if |aObserver| is part of another list.
 *       list.insertBack(aObserver);
 *     }
 *
 *     void removeObserver(Observer* aObserver)
 *     {
 *       // Will assert if |aObserver| is not part of some list.
 *       aObserver.remove();
 *       // Or, will assert if |aObserver| is not part of |list| specifically.
 *       // aObserver.removeFrom(list);
 *     }
 *
 *     void notifyObservers(char* aTopic)
 *     {
 *       for (Observer* o = list.getFirst(); o != nullptr; o = o->getNext()) {
 *         o->observe(aTopic);
 *       }
 *     }
 *   };
 *
 * Additionally, the class AutoCleanLinkedList<T> is a LinkedList<T> that will
 * remove and delete each element still within itself upon destruction. Note
 * that because each element is deleted, elements must have been allocated
 * using |new|.
 */

#ifndef mozilla_LinkedList_h
#define mozilla_LinkedList_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"

#ifdef __cplusplus

namespace mozilla {

template<typename T>
class LinkedList;

template<typename T>
class LinkedListElement
{
  /*
   * It's convenient that we return nullptr when getNext() or getPrevious()
   * hits the end of the list, but doing so costs an extra word of storage in
   * each linked list node (to keep track of whether |this| is the sentinel
   * node) and a branch on this value in getNext/getPrevious.
   *
   * We could get rid of the extra word of storage by shoving the "is
   * sentinel" bit into one of the pointers, although this would, of course,
   * have performance implications of its own.
   *
   * But the goal here isn't to win an award for the fastest or slimmest
   * linked list; rather, we want a *convenient* linked list.  So we won't
   * waste time guessing which micro-optimization strategy is best.
   *
   *
   * Speaking of unnecessary work, it's worth addressing here why we wrote
   * mozilla::LinkedList in the first place, instead of using stl::list.
   *
   * The key difference between mozilla::LinkedList and stl::list is that
   * mozilla::LinkedList stores the mPrev/mNext pointers in the object itself,
   * while stl::list stores the mPrev/mNext pointers in a list element which
   * itself points to the object being stored.
   *
   * mozilla::LinkedList's approach makes it harder to store an object in more
   * than one list.  But the upside is that you can call next() / prev() /
   * remove() directly on the object.  With stl::list, you'd need to store a
   * pointer to its iterator in the object in order to accomplish this.  Not
   * only would this waste space, but you'd have to remember to update that
   * pointer every time you added or removed the object from a list.
   *
   * In-place, constant-time removal is a killer feature of doubly-linked
   * lists, and supporting this painlessly was a key design criterion.
   */

private:
  LinkedListElement* mNext;
  LinkedListElement* mPrev;
  const bool mIsSentinel;

public:
  LinkedListElement()
    : mNext(this),
      mPrev(this),
      mIsSentinel(false)
  { }

  LinkedListElement(LinkedListElement<T>&& other)
    : mIsSentinel(other.mIsSentinel)
  {
    if (!other.isInList()) {
      mNext = this;
      mPrev = this;
      return;
    }

    MOZ_ASSERT(other.mNext->mPrev == &other);
    MOZ_ASSERT(other.mPrev->mNext == &other);

    /*
     * Initialize |this| with |other|'s mPrev/mNext pointers, and adjust those
     * element to point to this one.
     */
    mNext = other.mNext;
    mPrev = other.mPrev;

    mNext->mPrev = this;
    mPrev->mNext = this;

    /*
     * Adjust |other| so it doesn't think it's in a list.  This makes it
     * safely destructable.
     */
    other.mNext = &other;
    other.mPrev = &other;
  }

  ~LinkedListElement()
  {
    if (!mIsSentinel && isInList()) {
      remove();
    }
  }

  /*
   * Get the next element in the list, or nullptr if this is the last element
   * in the list.
   */
  T* getNext()             { return mNext->asT(); }
  const T* getNext() const { return mNext->asT(); }

  /*
   * Get the previous element in the list, or nullptr if this is the first
   * element in the list.
   */
  T* getPrevious()             { return mPrev->asT(); }
  const T* getPrevious() const { return mPrev->asT(); }

  /*
   * Insert aElem after this element in the list.  |this| must be part of a
   * linked list when you call setNext(); otherwise, this method will assert.
   */
  void setNext(T* aElem)
  {
    MOZ_ASSERT(isInList());
    setNextUnsafe(aElem);
  }

  /*
   * Insert aElem before this element in the list.  |this| must be part of a
   * linked list when you call setPrevious(); otherwise, this method will
   * assert.
   */
  void setPrevious(T* aElem)
  {
    MOZ_ASSERT(isInList());
    setPreviousUnsafe(aElem);
  }

  /*
   * Remove this element from the list which contains it.  If this element is
   * not currently part of a linked list, this method asserts.
   */
  void remove()
  {
    MOZ_ASSERT(isInList());

    mPrev->mNext = mNext;
    mNext->mPrev = mPrev;
    mNext = this;
    mPrev = this;
  }

  /*
   * Identical to remove(), but also asserts in debug builds that this element
   * is in aList.
   */
  void removeFrom(const LinkedList<T>& aList)
  {
    aList.assertContains(asT());
    remove();
  }

  /*
   * Return true if |this| part is of a linked list, and false otherwise.
   */
  bool isInList() const
  {
    MOZ_ASSERT((mNext == this) == (mPrev == this));
    return mNext != this;
  }

private:
  friend class LinkedList<T>;

  enum NodeKind {
    NODE_KIND_NORMAL,
    NODE_KIND_SENTINEL
  };

  explicit LinkedListElement(NodeKind nodeKind)
    : mNext(this),
      mPrev(this),
      mIsSentinel(nodeKind == NODE_KIND_SENTINEL)
  { }

  /*
   * Return |this| cast to T* if we're a normal node, or return nullptr if
   * we're a sentinel node.
   */
  T* asT()
  {
    return mIsSentinel ? nullptr : static_cast<T*>(this);
  }
  const T* asT() const
  {
    return mIsSentinel ? nullptr : static_cast<const T*>(this);
  }

  /*
   * Insert aElem after this element, but don't check that this element is in
   * the list.  This is called by LinkedList::insertFront().
   */
  void setNextUnsafe(T* aElem)
  {
    LinkedListElement *listElem = static_cast<LinkedListElement*>(aElem);
    MOZ_ASSERT(!listElem->isInList());

    listElem->mNext = this->mNext;
    listElem->mPrev = this;
    this->mNext->mPrev = listElem;
    this->mNext = listElem;
  }

  /*
   * Insert aElem before this element, but don't check that this element is in
   * the list.  This is called by LinkedList::insertBack().
   */
  void setPreviousUnsafe(T* aElem)
  {
    LinkedListElement<T>* listElem = static_cast<LinkedListElement<T>*>(aElem);
    MOZ_ASSERT(!listElem->isInList());

    listElem->mNext = this;
    listElem->mPrev = this->mPrev;
    this->mPrev->mNext = listElem;
    this->mPrev = listElem;
  }

private:
  LinkedListElement& operator=(const LinkedListElement<T>& aOther) = delete;
  LinkedListElement(const LinkedListElement<T>& aOther) = delete;
};

template<typename T>
class LinkedList
{
private:
  LinkedListElement<T> sentinel;

public:
  class Iterator {
    T* mCurrent;

  public:
    explicit Iterator(T* aCurrent) : mCurrent(aCurrent) {}

    T* operator *() const {
      return mCurrent;
    }

    const Iterator& operator++() {
      mCurrent = mCurrent->getNext();
      return *this;
    }

    bool operator!=(Iterator& aOther) const {
      return mCurrent != aOther.mCurrent;
    }
  };

  LinkedList() : sentinel(LinkedListElement<T>::NODE_KIND_SENTINEL) { }

  LinkedList(LinkedList<T>&& aOther)
    : sentinel(mozilla::Move(aOther.sentinel))
  { }

  ~LinkedList() { MOZ_ASSERT(isEmpty()); }

  /*
   * Add aElem to the front of the list.
   */
  void insertFront(T* aElem)
  {
    /* Bypass setNext()'s this->isInList() assertion. */
    sentinel.setNextUnsafe(aElem);
  }

  /*
   * Add aElem to the back of the list.
   */
  void insertBack(T* aElem)
  {
    sentinel.setPreviousUnsafe(aElem);
  }

  /*
   * Get the first element of the list, or nullptr if the list is empty.
   */
  T* getFirst()             { return sentinel.getNext(); }
  const T* getFirst() const { return sentinel.getNext(); }

  /*
   * Get the last element of the list, or nullptr if the list is empty.
   */
  T* getLast()             { return sentinel.getPrevious(); }
  const T* getLast() const { return sentinel.getPrevious(); }

  /*
   * Get and remove the first element of the list.  If the list is empty,
   * return nullptr.
   */
  T* popFirst()
  {
    T* ret = sentinel.getNext();
    if (ret) {
      static_cast<LinkedListElement<T>*>(ret)->remove();
    }
    return ret;
  }

  /*
   * Get and remove the last element of the list.  If the list is empty,
   * return nullptr.
   */
  T* popLast()
  {
    T* ret = sentinel.getPrevious();
    if (ret) {
      static_cast<LinkedListElement<T>*>(ret)->remove();
    }
    return ret;
  }

  /*
   * Return true if the list is empty, or false otherwise.
   */
  bool isEmpty() const
  {
    return !sentinel.isInList();
  }

  /*
   * Remove all the elements from the list.
   *
   * This runs in time linear to the list's length, because we have to mark
   * each element as not in the list.
   */
  void clear()
  {
    while (popFirst()) {
      continue;
    }
  }

  /*
   * Allow range-based iteration:
   *
   *     for (MyElementType* elt : myList) { ... }
   */
  Iterator begin() {
    return Iterator(getFirst());
  }
  Iterator end() {
    return Iterator(nullptr);
  }

  /*
   * Measures the memory consumption of the list excluding |this|.  Note that
   * it only measures the list elements themselves.  If the list elements
   * contain pointers to other memory blocks, those blocks must be measured
   * separately during a subsequent iteration over the list.
   */
  size_t sizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const
  {
    size_t n = 0;
    for (const T* t = getFirst(); t; t = t->getNext()) {
      n += aMallocSizeOf(t);
    }
    return n;
  }

  /*
   * Like sizeOfExcludingThis(), but measures |this| as well.
   */
  size_t sizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
  {
    return aMallocSizeOf(this) + sizeOfExcludingThis(aMallocSizeOf);
  }

  /*
   * In a debug build, make sure that the list is sane (no cycles, consistent
   * mNext/mPrev pointers, only one sentinel).  Has no effect in release builds.
   */
  void debugAssertIsSane() const
  {
#ifdef DEBUG
    const LinkedListElement<T>* slow;
    const LinkedListElement<T>* fast1;
    const LinkedListElement<T>* fast2;

    /*
     * Check for cycles in the forward singly-linked list using the
     * tortoise/hare algorithm.
     */
    for (slow = sentinel.mNext,
         fast1 = sentinel.mNext->mNext,
         fast2 = sentinel.mNext->mNext->mNext;
         slow != &sentinel && fast1 != &sentinel && fast2 != &sentinel;
         slow = slow->mNext, fast1 = fast2->mNext, fast2 = fast1->mNext) {
      MOZ_ASSERT(slow != fast1);
      MOZ_ASSERT(slow != fast2);
    }

    /* Check for cycles in the backward singly-linked list. */
    for (slow = sentinel.mPrev,
         fast1 = sentinel.mPrev->mPrev,
         fast2 = sentinel.mPrev->mPrev->mPrev;
         slow != &sentinel && fast1 != &sentinel && fast2 != &sentinel;
         slow = slow->mPrev, fast1 = fast2->mPrev, fast2 = fast1->mPrev) {
      MOZ_ASSERT(slow != fast1);
      MOZ_ASSERT(slow != fast2);
    }

    /*
     * Check that |sentinel| is the only node in the list with
     * mIsSentinel == true.
     */
    for (const LinkedListElement<T>* elem = sentinel.mNext;
         elem != &sentinel;
         elem = elem->mNext) {
      MOZ_ASSERT(!elem->mIsSentinel);
    }

    /* Check that the mNext/mPrev pointers match up. */
    const LinkedListElement<T>* prev = &sentinel;
    const LinkedListElement<T>* cur = sentinel.mNext;
    do {
        MOZ_ASSERT(cur->mPrev == prev);
        MOZ_ASSERT(prev->mNext == cur);

        prev = cur;
        cur = cur->mNext;
    } while (cur != &sentinel);
#endif /* ifdef DEBUG */
  }

private:
  friend class LinkedListElement<T>;

  void assertContains(const T* aValue) const
  {
#ifdef DEBUG
    for (const T* elem = getFirst(); elem; elem = elem->getNext()) {
      if (elem == aValue) {
        return;
      }
    }
    MOZ_CRASH("element wasn't found in this list!");
#endif
  }

  LinkedList& operator=(const LinkedList<T>& aOther) = delete;
  LinkedList(const LinkedList<T>& aOther) = delete;
};

template <typename T>
class AutoCleanLinkedList : public LinkedList<T>
{
public:
  ~AutoCleanLinkedList()
  {
    while (T* element = this->popFirst()) {
      delete element;
    }
  }
};

} /* namespace mozilla */

#endif /* __cplusplus */

#endif /* mozilla_LinkedList_h */
