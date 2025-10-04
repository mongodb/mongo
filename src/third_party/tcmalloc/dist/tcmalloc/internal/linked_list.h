// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Some very basic linked list functions for dealing with using void * as
// storage.

#ifndef TCMALLOC_INTERNAL_LINKED_LIST_H_
#define TCMALLOC_INTERNAL_LINKED_LIST_H_

#include <stddef.h>
#include <stdint.h>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* SLL_Next(void* t) {
  return *(reinterpret_cast<void**>(t));
}

inline void ABSL_ATTRIBUTE_ALWAYS_INLINE SLL_SetNext(void* t, void* n) {
  *(reinterpret_cast<void**>(t)) = n;
}

inline void ABSL_ATTRIBUTE_ALWAYS_INLINE SLL_Push(void** list, void* element) {
  SLL_SetNext(element, *list);
  *list = element;
}

inline void* SLL_Pop(void** list) {
  void* result = *list;
  void* next = SLL_Next(*list);
  *list = next;
  // Prefetching NULL leads to a DTLB miss, thus only prefetch when 'next'
  // is not NULL.
#if defined(__GNUC__)
  if (next) {
    __builtin_prefetch(next, 0, 3);
  }
#endif
  return result;
}

// LinkedList forms an in-place linked list with its void* elements.
class LinkedList {
 private:
  void* list_ = nullptr;  // Linked list.
  uint32_t length_ = 0;   // Current length.

 public:
  constexpr LinkedList() = default;

  // Not copy constructible or movable.
  LinkedList(const LinkedList&) = delete;
  LinkedList(LinkedList&&) = delete;
  LinkedList& operator=(const LinkedList&) = delete;
  LinkedList& operator=(LinkedList&&) = delete;

  // Return current length of list
  size_t length() const { return length_; }

  // Is list empty?
  bool empty() const { return list_ == nullptr; }

  void ABSL_ATTRIBUTE_ALWAYS_INLINE Push(void* ptr) {
    SLL_Push(&list_, ptr);
    length_++;
  }

  bool ABSL_ATTRIBUTE_ALWAYS_INLINE TryPop(void** ret) {
    void* obj = list_;
    if (ABSL_PREDICT_FALSE(obj == nullptr)) {
      return false;
    }

    void* next = SLL_Next(obj);
    list_ = next;
    length_--;

#if defined(__GNUC__)
    if (ABSL_PREDICT_TRUE(next)) {
      __builtin_prefetch(next, 0, 0);
    }
#endif

    *ret = obj;
    return true;
  }

  // PushBatch and PopBatch do not guarantee an ordering.
  void PushBatch(int N, void** batch) {
    TC_ASSERT_GT(N, 0);
    for (int i = 0; i < N - 1; ++i) {
      SLL_SetNext(batch[i], batch[i + 1]);
    }
    SLL_SetNext(batch[N - 1], list_);
    list_ = batch[0];
    length_ += N;
  }

  void PopBatch(int N, void** batch) {
    void* p = list_;
    for (int i = 0; i < N; ++i) {
      batch[i] = p;
      p = SLL_Next(p);
    }
    list_ = p;
    TC_ASSERT_GE(length_, N);
    length_ -= N;
  }
};

// A well-typed intrusive doubly linked list.
template <typename T>
class TList {
 private:
  class Iter;

 public:
  // The intrusive element supertype.  Use the CRTP to declare your class:
  // class MyListItems : public TList<MyListItems>::Elem { ...
  class Elem {
    friend class Iter;
    friend class TList<T>;
    Elem* next_;
    Elem* prev_;

   protected:
    constexpr Elem() : next_(nullptr), prev_(nullptr) {}

    // Returns true iff the list is empty after removing this
    bool remove() {
      // Copy out next/prev before doing stores, otherwise compiler assumes
      // potential aliasing and does unnecessary reloads after stores.
      Elem* next = next_;
      Elem* prev = prev_;
      TC_ASSERT_EQ(prev->next_, this);
      prev->next_ = next;
      TC_ASSERT_EQ(next->prev_, this);
      next->prev_ = prev;
#ifndef NDEBUG
      prev_ = nullptr;
      next_ = nullptr;
#endif
      return next == prev;
    }

    void prepend(Elem* item) {
      Elem* prev = prev_;
      item->prev_ = prev;
      item->next_ = this;
      prev->next_ = item;
      prev_ = item;
    }

    void append(Elem* item) {
      Elem* next = next_;
      item->next_ = next;
      item->prev_ = this;
      next->prev_ = item;
      next_ = item;
    }
  };

  // Initialize to empty list.
  constexpr TList() { head_.next_ = head_.prev_ = &head_; }

  // Not copy constructible/movable.
  TList(const TList&) = delete;
  TList(TList&&) = delete;
  TList& operator=(const TList&) = delete;
  TList& operator=(TList&&) = delete;

  bool empty() const { return head_.next_ == &head_; }

  // Return the length of the linked list. O(n).
  size_t length() const {
    size_t result = 0;
    for (Elem* e = head_.next_; e != &head_; e = e->next_) {
      result++;
    }
    return result;
  }

  // Returns first element in the list. The list must not be empty.
  ABSL_ATTRIBUTE_RETURNS_NONNULL T* first() const {
    TC_ASSERT(!empty());
    TC_ASSERT_NE(head_.next_, nullptr);
    return static_cast<T*>(head_.next_);
  }

  // Returns last element in the list. The list must not be empty.
  ABSL_ATTRIBUTE_RETURNS_NONNULL T* last() const {
    TC_ASSERT(!empty());
    TC_ASSERT_NE(head_.prev_, nullptr);
    return static_cast<T*>(head_.prev_);
  }

  // Add item to the front of list.
  void prepend(T* item) { head_.append(item); }

  void append(T* item) { head_.prepend(item); }

  bool remove(T* item) {
    // must be on the list; we don't check.
    return item->remove();
  }

  // Support for range-based iteration over a list.
  Iter begin() const { return Iter(head_.next_); }
  Iter end() const { return Iter(const_cast<Elem*>(&head_)); }

  // Iterator pointing to a given list item.
  // REQUIRES: item is a member of the list.
  Iter at(T* item) const { return Iter(item); }

 private:
  // Support for range-based iteration over a list.
  class Iter {
    friend class TList;
    Elem* elem_;
    explicit Iter(Elem* elem) : elem_(elem) {}

   public:
    Iter& operator++() {
      elem_ = elem_->next_;
      return *this;
    }
    Iter& operator--() {
      elem_ = elem_->prev_;
      return *this;
    }

    bool operator!=(Iter other) const { return elem_ != other.elem_; }
    bool operator==(Iter other) const { return elem_ == other.elem_; }
    T* operator*() const { return static_cast<T*>(elem_); }
    T* operator->() const { return static_cast<T*>(elem_); }
  };
  friend class Iter;

  Elem head_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_LINKED_LIST_H_
