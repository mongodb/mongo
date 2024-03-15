/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** A doubly-linked list with flexible next/prev naming. */

#ifndef mozilla_DoublyLinkedList_h
#define mozilla_DoublyLinkedList_h

#include <algorithm>
#include <iosfwd>
#include <iterator>
#include <type_traits>

#include "mozilla/Assertions.h"

/**
 * Where mozilla::LinkedList strives for ease of use above all other
 * considerations, mozilla::DoublyLinkedList strives for flexibility. The
 * following are things that can be done with mozilla::DoublyLinkedList that
 * cannot be done with mozilla::LinkedList:
 *
 *   * Arbitrary next/prev placement and naming. With the tools provided here,
 *     the next and previous pointers can be at the end of the structure, in a
 *     sub-structure, stored with a tag, in a union, wherever, as long as you
 *     can look them up and set them on demand.
 *   * Can be used without deriving from a new base and, thus, does not require
 *     use of constructors.
 *
 * Example:
 *
 *   class Observer : public DoublyLinkedListElement<Observer>
 *   {
 *   public:
 *     void observe(char* aTopic) { ... }
 *   };
 *
 *   class ObserverContainer
 *   {
 *   private:
 *     DoublyLinkedList<Observer> mList;
 *
 *   public:
 *     void addObserver(Observer* aObserver)
 *     {
 *       // Will assert if |aObserver| is part of another list.
 *       mList.pushBack(aObserver);
 *     }
 *
 *     void removeObserver(Observer* aObserver)
 *     {
 *       // Will assert if |aObserver| is not part of |list|.
 *       mList.remove(aObserver);
 *     }
 *
 *     void notifyObservers(char* aTopic)
 *     {
 *       for (Observer* o : mList) {
 *         o->observe(aTopic);
 *       }
 *     }
 *   };
 */

namespace mozilla {

/**
 *  Deriving from this will allow T to be inserted into and removed from a
 *  DoublyLinkedList.
 */
template <typename T>
class DoublyLinkedListElement {
  template <typename U, typename E>
  friend class DoublyLinkedList;
  friend T;
  T* mNext;
  T* mPrev;

 public:
  DoublyLinkedListElement() : mNext(nullptr), mPrev(nullptr) {}
};

/**
 * Provides access to a DoublyLinkedListElement within T.
 *
 * The default implementation of this template works for types that derive
 * from DoublyLinkedListElement, but one can specialize for their class so
 * that some appropriate DoublyLinkedListElement reference is returned.
 *
 * For more complex cases (multiple DoublyLinkedListElements, for example),
 * one can define their own trait class and use that as ElementAccess for
 * DoublyLinkedList. See TestDoublyLinkedList.cpp for an example.
 */
template <typename T>
struct GetDoublyLinkedListElement {
  static_assert(std::is_base_of<DoublyLinkedListElement<T>, T>::value,
                "You need your own specialization of GetDoublyLinkedListElement"
                " or use a separate Trait.");
  static DoublyLinkedListElement<T>& Get(T* aThis) { return *aThis; }
};

/**
 * A doubly linked list. |T| is the type of element stored in this list. |T|
 * must contain or have access to unique next and previous element pointers.
 * The template argument |ElementAccess| provides code to tell this list how to
 * get a reference to a DoublyLinkedListElement that may reside anywhere.
 */
template <typename T, typename ElementAccess = GetDoublyLinkedListElement<T>>
class DoublyLinkedList final {
  T* mHead;
  T* mTail;

  /**
   * Checks that either the list is empty and both mHead and mTail are nullptr
   * or the list has entries and both mHead and mTail are non-null.
   */
  bool isStateValid() const { return (mHead != nullptr) == (mTail != nullptr); }

  bool ElementNotInList(T* aElm) {
    if (!ElementAccess::Get(aElm).mNext && !ElementAccess::Get(aElm).mPrev) {
      // Both mNext and mPrev being NULL can mean two things:
      // - the element is not in the list.
      // - the element is the first and only element in the list.
      // So check for the latter.
      return mHead != aElm;
    }
    return false;
  }

 public:
  DoublyLinkedList() : mHead(nullptr), mTail(nullptr) {}

  class Iterator final {
    T* mCurrent;

   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    Iterator() : mCurrent(nullptr) {}
    explicit Iterator(T* aCurrent) : mCurrent(aCurrent) {}

    T& operator*() const { return *mCurrent; }
    T* operator->() const { return mCurrent; }

    Iterator& operator++() {
      mCurrent = mCurrent ? ElementAccess::Get(mCurrent).mNext : nullptr;
      return *this;
    }

    Iterator operator++(int) {
      Iterator result = *this;
      ++(*this);
      return result;
    }

    Iterator& operator--() {
      mCurrent = ElementAccess::Get(mCurrent).mPrev;
      return *this;
    }

    Iterator operator--(int) {
      Iterator result = *this;
      --(*this);
      return result;
    }

    bool operator!=(const Iterator& aOther) const {
      return mCurrent != aOther.mCurrent;
    }

    bool operator==(const Iterator& aOther) const {
      return mCurrent == aOther.mCurrent;
    }

    explicit operator bool() const { return mCurrent; }
  };

  Iterator begin() { return Iterator(mHead); }
  const Iterator begin() const { return Iterator(mHead); }
  const Iterator cbegin() const { return Iterator(mHead); }

  Iterator end() { return Iterator(); }
  const Iterator end() const { return Iterator(); }
  const Iterator cend() const { return Iterator(); }

  /**
   * Returns true if the list contains no elements.
   */
  bool isEmpty() const {
    MOZ_ASSERT(isStateValid());
    return mHead == nullptr;
  }

  /**
   * Inserts aElm into the list at the head position. |aElm| must not already
   * be in a list.
   */
  void pushFront(T* aElm) {
    MOZ_ASSERT(aElm);
    MOZ_ASSERT(ElementNotInList(aElm));
    MOZ_ASSERT(isStateValid());

    ElementAccess::Get(aElm).mNext = mHead;
    if (mHead) {
      MOZ_ASSERT(!ElementAccess::Get(mHead).mPrev);
      ElementAccess::Get(mHead).mPrev = aElm;
    }

    mHead = aElm;
    if (!mTail) {
      mTail = aElm;
    }
  }

  /**
   * Remove the head of the list and return it. Calling this on an empty list
   * will assert.
   */
  T* popFront() {
    MOZ_ASSERT(!isEmpty());
    MOZ_ASSERT(isStateValid());

    T* result = mHead;
    mHead = result ? ElementAccess::Get(result).mNext : nullptr;
    if (mHead) {
      ElementAccess::Get(mHead).mPrev = nullptr;
    }

    if (mTail == result) {
      mTail = nullptr;
    }

    if (result) {
      ElementAccess::Get(result).mNext = nullptr;
      ElementAccess::Get(result).mPrev = nullptr;
    }

    return result;
  }

  /**
   * Inserts aElm into the list at the tail position. |aElm| must not already
   * be in a list.
   */
  void pushBack(T* aElm) {
    MOZ_ASSERT(aElm);
    MOZ_ASSERT(ElementNotInList(aElm));
    MOZ_ASSERT(isStateValid());

    ElementAccess::Get(aElm).mNext = nullptr;
    ElementAccess::Get(aElm).mPrev = mTail;
    if (mTail) {
      MOZ_ASSERT(!ElementAccess::Get(mTail).mNext);
      ElementAccess::Get(mTail).mNext = aElm;
    }

    mTail = aElm;
    if (!mHead) {
      mHead = aElm;
    }
  }

  /**
   * Remove the tail of the list and return it. Calling this on an empty list
   * will assert.
   */
  T* popBack() {
    MOZ_ASSERT(!isEmpty());
    MOZ_ASSERT(isStateValid());

    T* result = mTail;
    mTail = result ? ElementAccess::Get(result).mPrev : nullptr;
    if (mTail) {
      ElementAccess::Get(mTail).mNext = nullptr;
    }

    if (mHead == result) {
      mHead = nullptr;
    }

    if (result) {
      ElementAccess::Get(result).mNext = nullptr;
      ElementAccess::Get(result).mPrev = nullptr;
    }

    return result;
  }

  /**
   * Insert the given |aElm| *before* |aIter|.
   */
  void insertBefore(const Iterator& aIter, T* aElm) {
    MOZ_ASSERT(aElm);
    MOZ_ASSERT(ElementNotInList(aElm));
    MOZ_ASSERT(isStateValid());

    if (!aIter) {
      return pushBack(aElm);
    } else if (aIter == begin()) {
      return pushFront(aElm);
    }

    T* after = &(*aIter);
    T* before = ElementAccess::Get(after).mPrev;
    MOZ_ASSERT(before);

    ElementAccess::Get(before).mNext = aElm;
    ElementAccess::Get(aElm).mPrev = before;
    ElementAccess::Get(aElm).mNext = after;
    ElementAccess::Get(after).mPrev = aElm;
  }

  /**
   * Removes the given element from the list. The element must be in this list.
   */
  void remove(T* aElm) {
    MOZ_ASSERT(aElm);
    MOZ_ASSERT(ElementAccess::Get(aElm).mNext ||
                   ElementAccess::Get(aElm).mPrev ||
                   (aElm == mHead && aElm == mTail),
               "Attempted to remove element not in this list");

    if (T* prev = ElementAccess::Get(aElm).mPrev) {
      ElementAccess::Get(prev).mNext = ElementAccess::Get(aElm).mNext;
    } else {
      MOZ_ASSERT(mHead == aElm);
      mHead = ElementAccess::Get(aElm).mNext;
    }

    if (T* next = ElementAccess::Get(aElm).mNext) {
      ElementAccess::Get(next).mPrev = ElementAccess::Get(aElm).mPrev;
    } else {
      MOZ_ASSERT(mTail == aElm);
      mTail = ElementAccess::Get(aElm).mPrev;
    }

    ElementAccess::Get(aElm).mNext = nullptr;
    ElementAccess::Get(aElm).mPrev = nullptr;
  }

  /**
   * Returns an iterator referencing the first found element whose value matches
   * the given element according to operator==.
   */
  Iterator find(const T& aElm) { return std::find(begin(), end(), aElm); }

  /**
   * Returns whether the given element is in the list. Note that this uses
   * T::operator==, not pointer comparison.
   */
  bool contains(const T& aElm) { return find(aElm) != Iterator(); }

  /**
   * Returns whether the given element might be in the list. Note that this
   * assumes the element is either in the list or not in the list, and ignores
   * the case where the element might be in another list in order to make the
   * check fast.
   */
  bool ElementProbablyInList(T* aElm) {
    if (isEmpty()) {
      return false;
    }
    return !ElementNotInList(aElm);
  }
};

/**
 * @brief Double linked list that allows insertion/removal during iteration.
 *
 * This class uses the mozilla::DoublyLinkedList internally and keeps
 * track of created iterator instances by putting them on a simple list on stack
 * (compare nsTAutoObserverArray).
 * This allows insertion or removal operations to adjust iterators and therefore
 * keeping them valid during iteration.
 */
template <typename T, typename ElementAccess = GetDoublyLinkedListElement<T>>
class SafeDoublyLinkedList {
 public:
  /**
   * @brief Iterator class for SafeDoublyLinkedList.
   *
   * The iterator contains two iterators of the underlying list:
   *  - mCurrent points to the current list element of the iterator.
   *  - mNext points to the next element of the list.
   *
   * When removing an element from the list, mCurrent and mNext may
   * be adjusted:
   *  - If mCurrent is the element to be deleted, it is set to empty. mNext can
   *    still be used to advance to the next element.
   *  - If mNext is the element to be deleted, it is set to its next element
   *    (or to empty if mNext is the last element of the list).
   */
  class SafeIterator {
    using BaseIterator = typename DoublyLinkedList<T, ElementAccess>::Iterator;
    friend class SafeDoublyLinkedList<T, ElementAccess>;

   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;

    SafeIterator() = default;
    SafeIterator(SafeIterator const& aOther)
        : SafeIterator(aOther.mCurrent, aOther.mList) {}

    SafeIterator(BaseIterator aBaseIter,
                 SafeDoublyLinkedList<T, ElementAccess>* aList)
        : mCurrent(aBaseIter),
          mNext(aBaseIter ? ++aBaseIter : BaseIterator()),
          mList(aList) {
      if (mList) {
        mNextIterator = mList->mIter;
        mList->mIter = this;
      }
    }
    ~SafeIterator() {
      if (mList) {
        MOZ_ASSERT(mList->mIter == this,
                   "Iterators must currently be destroyed in opposite order "
                   "from the construction order. It is suggested that you "
                   "simply put them on the stack");
        mList->mIter = mNextIterator;
      }
    }

    SafeIterator& operator++() {
      mCurrent = mNext;
      if (mNext) {
        ++mNext;
      }
      return *this;
    }

    pointer operator->() { return &*mCurrent; }
    const_pointer operator->() const { return &*mCurrent; }
    reference operator*() { return *mCurrent; }
    const_reference operator*() const { return *mCurrent; }

    pointer current() { return mCurrent ? &*mCurrent : nullptr; }
    const_pointer current() const { return mCurrent ? &*mCurrent : nullptr; }

    explicit operator bool() const { return bool(mCurrent); }
    bool operator==(SafeIterator const& other) const {
      return mCurrent == other.mCurrent;
    }
    bool operator!=(SafeIterator const& other) const {
      return mCurrent != other.mCurrent;
    }

    BaseIterator& next() { return mNext; }  // mainly needed for unittests.
   private:
    /**
     * Base list iterator pointing to the current list element of the iteration.
     * If element mCurrent points to gets removed, the iterator will be set to
     * empty. mNext keeps the iterator valid.
     */
    BaseIterator mCurrent{nullptr};
    /**
     * Base list iterator pointing to the next list element of the iteration.
     * If element mCurrent points to gets removed, mNext is still valid.
     * If element mNext points to gets removed, mNext advances, keeping this
     * iterator valid.
     */
    BaseIterator mNext{nullptr};

    /**
     * Next element in the stack-allocated list of iterators stored in the
     * SafeLinkedList object.
     */
    SafeIterator* mNextIterator{nullptr};
    SafeDoublyLinkedList<T, ElementAccess>* mList{nullptr};

    void setNext(T* aElm) { mNext = BaseIterator(aElm); }
    void setCurrent(T* aElm) { mCurrent = BaseIterator(aElm); }
  };

 private:
  using BaseListType = DoublyLinkedList<T, ElementAccess>;
  friend class SafeIterator;

 public:
  SafeDoublyLinkedList() = default;

  bool isEmpty() const { return mList.isEmpty(); }
  bool contains(T* aElm) {
    for (auto iter = mList.begin(); iter != mList.end(); ++iter) {
      if (&*iter == aElm) {
        return true;
      }
    }
    return false;
  }

  SafeIterator begin() { return SafeIterator(mList.begin(), this); }
  SafeIterator begin() const { return SafeIterator(mList.begin(), this); }
  SafeIterator cbegin() const { return begin(); }

  SafeIterator end() { return SafeIterator(); }
  SafeIterator end() const { return SafeIterator(); }
  SafeIterator cend() const { return SafeIterator(); }

  void pushFront(T* aElm) { mList.pushFront(aElm); }

  void pushBack(T* aElm) {
    mList.pushBack(aElm);
    auto* iter = mIter;
    while (iter) {
      if (!iter->mNext) {
        iter->setNext(aElm);
      }
      iter = iter->mNextIterator;
    }
  }

  T* popFront() {
    T* firstElm = mList.popFront();
    auto* iter = mIter;
    while (iter) {
      if (iter->current() == firstElm) {
        iter->setCurrent(nullptr);
      }
      iter = iter->mNextIterator;
    }

    return firstElm;
  }

  T* popBack() {
    T* lastElm = mList.popBack();
    auto* iter = mIter;
    while (iter) {
      if (iter->current() == lastElm) {
        iter->setCurrent(nullptr);
      } else if (iter->mNext && &*(iter->mNext) == lastElm) {
        iter->setNext(nullptr);
      }
      iter = iter->mNextIterator;
    }

    return lastElm;
  }

  void remove(T* aElm) {
    if (!mList.ElementProbablyInList(aElm)) {
      return;
    }
    auto* iter = mIter;
    while (iter) {
      if (iter->mNext && &*(iter->mNext) == aElm) {
        ++(iter->mNext);
      }
      if (iter->current() == aElm) {
        iter->setCurrent(nullptr);
      }
      iter = iter->mNextIterator;
    }

    mList.remove(aElm);
  }

 private:
  BaseListType mList;
  SafeIterator* mIter{nullptr};
};

}  // namespace mozilla

#endif  // mozilla_DoublyLinkedList_h
