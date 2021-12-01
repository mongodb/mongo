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
#include "mozilla/RefPtr.h"

#ifdef __cplusplus

namespace mozilla {

template<typename T>
class LinkedListElement;

namespace detail {

/**
 * LinkedList supports refcounted elements using this adapter class. Clients
 * using LinkedList<RefPtr<T>> will get a data structure that holds a strong
 * reference to T as long as T is in the list.
 */
template<typename T>
struct LinkedListElementTraits
{
  typedef T* RawType;
  typedef const T* ConstRawType;
  typedef T* ClientType;
  typedef const T* ConstClientType;

  // These static methods are called when an element is added to or removed from
  // a linked list. It can be used to keep track ownership in lists that are
  // supposed to own their elements. If elements are transferred from one list
  // to another, no enter or exit calls happen since the elements still belong
  // to a list.
  static void enterList(LinkedListElement<T>* elt) {}
  static void exitList(LinkedListElement<T>* elt) {}
};

template<typename T>
struct LinkedListElementTraits<RefPtr<T>>
{
  typedef T* RawType;
  typedef const T* ConstRawType;
  typedef RefPtr<T> ClientType;
  typedef RefPtr<const T> ConstClientType;

  static void enterList(LinkedListElement<RefPtr<T>>* elt) { elt->asT()->AddRef(); }
  static void exitList(LinkedListElement<RefPtr<T>>* elt) { elt->asT()->Release(); }
};

} /* namespace detail */

template<typename T>
class LinkedList;

template<typename T>
class LinkedListElement
{
  typedef typename detail::LinkedListElementTraits<T> Traits;
  typedef typename Traits::RawType RawType;
  typedef typename Traits::ConstRawType ConstRawType;
  typedef typename Traits::ClientType ClientType;
  typedef typename Traits::ConstClientType ConstClientType;

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

  /*
   * Moves |aOther| into |*this|. If |aOther| is already in a list, then
   * |aOther| is removed from the list and replaced by |*this|.
   */
  LinkedListElement(LinkedListElement<T>&& aOther)
    : mIsSentinel(aOther.mIsSentinel)
  {
    adjustLinkForMove(Move(aOther));
  }

  LinkedListElement& operator=(LinkedListElement<T>&& aOther)
  {
    MOZ_ASSERT(mIsSentinel == aOther.mIsSentinel, "Mismatch NodeKind!");
    MOZ_ASSERT(!isInList(),
               "Assigning to an element in a list messes up that list!");
    adjustLinkForMove(Move(aOther));
    return *this;
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
  RawType getNext()            { return mNext->asT(); }
  ConstRawType getNext() const { return mNext->asT(); }

  /*
   * Get the previous element in the list, or nullptr if this is the first
   * element in the list.
   */
  RawType getPrevious()            { return mPrev->asT(); }
  ConstRawType getPrevious() const { return mPrev->asT(); }

  /*
   * Insert aElem after this element in the list.  |this| must be part of a
   * linked list when you call setNext(); otherwise, this method will assert.
   */
  void setNext(RawType aElem)
  {
    MOZ_ASSERT(isInList());
    setNextUnsafe(aElem);
  }

  /*
   * Insert aElem before this element in the list.  |this| must be part of a
   * linked list when you call setPrevious(); otherwise, this method will
   * assert.
   */
  void setPrevious(RawType aElem)
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

    Traits::exitList(this);
  }

  /*
   * Remove this element from the list containing it.  Returns a pointer to the
   * element that follows this element (before it was removed).  This method
   * asserts if the element does not belong to a list. Note: In a refcounted list,
   * |this| may be destroyed.
   */
  RawType removeAndGetNext()
  {
    RawType r = getNext();
    remove();
    return r;
  }

  /*
   * Remove this element from the list containing it.  Returns a pointer to the
   * previous element in the containing list (before the removal).  This method
   * asserts if the element does not belong to a list. Note: In a refcounted list,
   * |this| may be destroyed.
   */
  RawType removeAndGetPrevious()
  {
    RawType r = getPrevious();
    remove();
    return r;
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
  friend struct detail::LinkedListElementTraits<T>;

  enum class NodeKind {
    Normal,
    Sentinel
  };

  explicit LinkedListElement(NodeKind nodeKind)
    : mNext(this),
      mPrev(this),
      mIsSentinel(nodeKind == NodeKind::Sentinel)
  { }

  /*
   * Return |this| cast to T* if we're a normal node, or return nullptr if
   * we're a sentinel node.
   */
  RawType asT()
  {
    return mIsSentinel ? nullptr : static_cast<RawType>(this);
  }
  ConstRawType asT() const
  {
    return mIsSentinel ? nullptr : static_cast<ConstRawType>(this);
  }

  /*
   * Insert aElem after this element, but don't check that this element is in
   * the list.  This is called by LinkedList::insertFront().
   */
  void setNextUnsafe(RawType aElem)
  {
    LinkedListElement *listElem = static_cast<LinkedListElement*>(aElem);
    MOZ_ASSERT(!listElem->isInList());

    listElem->mNext = this->mNext;
    listElem->mPrev = this;
    this->mNext->mPrev = listElem;
    this->mNext = listElem;

    Traits::enterList(aElem);
  }

  /*
   * Insert aElem before this element, but don't check that this element is in
   * the list.  This is called by LinkedList::insertBack().
   */
  void setPreviousUnsafe(RawType aElem)
  {
    LinkedListElement<T>* listElem = static_cast<LinkedListElement<T>*>(aElem);
    MOZ_ASSERT(!listElem->isInList());

    listElem->mNext = this;
    listElem->mPrev = this->mPrev;
    this->mPrev->mNext = listElem;
    this->mPrev = listElem;

    Traits::enterList(aElem);
  }

  /*
   * Adjust mNext and mPrev for implementing move constructor and move
   * assignment.
   */
  void adjustLinkForMove(LinkedListElement<T>&& aOther)
  {
    if (!aOther.isInList()) {
      mNext = this;
      mPrev = this;
      return;
    }

    if (!mIsSentinel) {
      Traits::enterList(this);
    }

    MOZ_ASSERT(aOther.mNext->mPrev == &aOther);
    MOZ_ASSERT(aOther.mPrev->mNext == &aOther);

    /*
     * Initialize |this| with |aOther|'s mPrev/mNext pointers, and adjust those
     * element to point to this one.
     */
    mNext = aOther.mNext;
    mPrev = aOther.mPrev;

    mNext->mPrev = this;
    mPrev->mNext = this;

    /*
     * Adjust |aOther| so it doesn't think it's in a list.  This makes it
     * safely destructable.
     */
    aOther.mNext = &aOther;
    aOther.mPrev = &aOther;

    if (!mIsSentinel) {
      Traits::exitList(&aOther);
    }
  }

  LinkedListElement& operator=(const LinkedListElement<T>& aOther) = delete;
  LinkedListElement(const LinkedListElement<T>& aOther) = delete;
};

template<typename T>
class LinkedList
{
private:
  typedef typename detail::LinkedListElementTraits<T> Traits;
  typedef typename Traits::RawType RawType;
  typedef typename Traits::ConstRawType ConstRawType;
  typedef typename Traits::ClientType ClientType;
  typedef typename Traits::ConstClientType ConstClientType;

  LinkedListElement<T> sentinel;

public:
  class Iterator {
    RawType mCurrent;

  public:
    explicit Iterator(RawType aCurrent) : mCurrent(aCurrent) {}

    RawType operator *() const {
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

  LinkedList() : sentinel(LinkedListElement<T>::NodeKind::Sentinel) { }

  LinkedList(LinkedList<T>&& aOther)
    : sentinel(mozilla::Move(aOther.sentinel))
  { }

  LinkedList& operator=(LinkedList<T>&& aOther)
  {
    MOZ_ASSERT(isEmpty(), "Assigning to a non-empty list leaks elements in that list!");
    sentinel = mozilla::Move(aOther.sentinel);
    return *this;
  }

  ~LinkedList() {
    MOZ_ASSERT(isEmpty(),
               "failing this assertion means this LinkedList's creator is "
               "buggy: it should have removed all this list's elements before "
               "the list's destruction");
  }

  /*
   * Add aElem to the front of the list.
   */
  void insertFront(RawType aElem)
  {
    /* Bypass setNext()'s this->isInList() assertion. */
    sentinel.setNextUnsafe(aElem);
  }

  /*
   * Add aElem to the back of the list.
   */
  void insertBack(RawType aElem)
  {
    sentinel.setPreviousUnsafe(aElem);
  }

  /*
   * Get the first element of the list, or nullptr if the list is empty.
   */
  RawType getFirst()            { return sentinel.getNext(); }
  ConstRawType getFirst() const { return sentinel.getNext(); }

  /*
   * Get the last element of the list, or nullptr if the list is empty.
   */
  RawType getLast()            { return sentinel.getPrevious(); }
  ConstRawType getLast() const { return sentinel.getPrevious(); }

  /*
   * Get and remove the first element of the list.  If the list is empty,
   * return nullptr.
   */
  ClientType popFirst()
  {
    ClientType ret = sentinel.getNext();
    if (ret) {
      static_cast<LinkedListElement<T>*>(RawType(ret))->remove();
    }
    return ret;
  }

  /*
   * Get and remove the last element of the list.  If the list is empty,
   * return nullptr.
   */
  ClientType popLast()
  {
    ClientType ret = sentinel.getPrevious();
    if (ret) {
      static_cast<LinkedListElement<T>*>(RawType(ret))->remove();
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
    for (ConstRawType t = getFirst(); t; t = t->getNext()) {
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

  void assertContains(const RawType aValue) const
  {
#ifdef DEBUG
    for (ConstRawType elem = getFirst(); elem; elem = elem->getNext()) {
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
    clear();
  }

  AutoCleanLinkedList& operator=(AutoCleanLinkedList&& aOther)
  {
    LinkedList<T>::operator=(Forward<LinkedList<T>>(aOther));
    return *this;
  }

  void clear()
  {
    while (T* element = this->popFirst()) {
      delete element;
    }
  }
};

} /* namespace mozilla */

#endif /* __cplusplus */

#endif /* mozilla_LinkedList_h */
