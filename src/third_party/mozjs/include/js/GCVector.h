/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCVector_h
#define js_GCVector_h

#include "mozilla/Assertions.h"       // MOZ_ASSERT
#include "mozilla/Attributes.h"       // MOZ_STACK_CLASS
#include "mozilla/MemoryReporting.h"  // MallocSizeOf
#include "mozilla/Span.h"
#include "mozilla/Vector.h"

#include <stddef.h>  // size_t
#include <utility>   // forward, move

#include "js/AllocPolicy.h"
#include "js/GCPolicyAPI.h"
#include "js/RootingAPI.h"

class JSTracer;
struct JSContext;

namespace JS {

// A GCVector is a Vector with an additional trace method that knows how
// to visit all of the items stored in the Vector. For vectors that contain GC
// things, this is usually more convenient than manually iterating and marking
// the contents.
//
// Most types of GC pointers as keys and values can be traced with no extra
// infrastructure. For structs and non-gc-pointer members, ensure that there is
// a specialization of GCPolicy<T> with an appropriate trace method available
// to handle the custom type. Generic helpers can be found in
// js/public/TracingAPI.h.
//
// Note that although this Vector's trace will deal correctly with moved items,
// it does not itself know when to barrier or trace items. To function properly
// it must either be used with Rooted, or barriered and traced manually.
template <typename T, size_t MinInlineCapacity = 0,
          typename AllocPolicy = js::TempAllocPolicy>
class GCVector {
  mozilla::Vector<T, MinInlineCapacity, AllocPolicy> vector;

 public:
  using ElementType = T;

  explicit GCVector(AllocPolicy alloc) : vector(std::move(alloc)) {}
  GCVector() : GCVector(AllocPolicy()) {}

  GCVector(GCVector&& vec) : vector(std::move(vec.vector)) {}

  GCVector& operator=(GCVector&& vec) {
    vector = std::move(vec.vector);
    return *this;
  }

  size_t length() const { return vector.length(); }
  bool empty() const { return vector.empty(); }
  size_t capacity() const { return vector.capacity(); }

  T* begin() { return vector.begin(); }
  const T* begin() const { return vector.begin(); }

  T* end() { return vector.end(); }
  const T* end() const { return vector.end(); }

  T& operator[](size_t i) { return vector[i]; }
  const T& operator[](size_t i) const { return vector[i]; }

  T& back() { return vector.back(); }
  const T& back() const { return vector.back(); }

  operator mozilla::Span<T>() { return vector; }
  operator mozilla::Span<const T>() const { return vector; }

  bool initCapacity(size_t cap) { return vector.initCapacity(cap); }
  [[nodiscard]] bool reserve(size_t req) { return vector.reserve(req); }
  void shrinkBy(size_t amount) { return vector.shrinkBy(amount); }
  void shrinkTo(size_t newLen) { return vector.shrinkTo(newLen); }
  [[nodiscard]] bool growBy(size_t amount) { return vector.growBy(amount); }
  [[nodiscard]] bool resize(size_t newLen) { return vector.resize(newLen); }

  void clear() { return vector.clear(); }
  void clearAndFree() { return vector.clearAndFree(); }

  template <typename U>
  bool append(U&& item) {
    return vector.append(std::forward<U>(item));
  }

  void erase(T* it) { vector.erase(it); }
  void erase(T* begin, T* end) { vector.erase(begin, end); }
  template <typename Pred>
  void eraseIf(Pred pred) {
    vector.eraseIf(pred);
  }
  template <typename U>
  void eraseIfEqual(const U& u) {
    vector.eraseIfEqual(u);
  }

  template <typename... Args>
  [[nodiscard]] bool emplaceBack(Args&&... args) {
    return vector.emplaceBack(std::forward<Args>(args)...);
  }

  template <typename... Args>
  void infallibleEmplaceBack(Args&&... args) {
    vector.infallibleEmplaceBack(std::forward<Args>(args)...);
  }

  template <typename U>
  void infallibleAppend(U&& aU) {
    return vector.infallibleAppend(std::forward<U>(aU));
  }
  void infallibleAppendN(const T& aT, size_t aN) {
    return vector.infallibleAppendN(aT, aN);
  }
  template <typename U>
  void infallibleAppend(const U* aBegin, const U* aEnd) {
    return vector.infallibleAppend(aBegin, aEnd);
  }
  template <typename U>
  void infallibleAppend(const U* aBegin, size_t aLength) {
    return vector.infallibleAppend(aBegin, aLength);
  }

  template <typename U>
  [[nodiscard]] bool appendAll(const U& aU) {
    return vector.append(aU.begin(), aU.end());
  }
  template <typename T2, size_t MinInlineCapacity2, typename AllocPolicy2>
  [[nodiscard]] bool appendAll(
      GCVector<T2, MinInlineCapacity2, AllocPolicy2>&& aU) {
    return vector.appendAll(aU.begin(), aU.end());
  }

  [[nodiscard]] bool appendN(const T& val, size_t count) {
    return vector.appendN(val, count);
  }

  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, const U* aEnd) {
    return vector.append(aBegin, aEnd);
  }
  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, size_t aLength) {
    return vector.append(aBegin, aLength);
  }

  void popBack() { return vector.popBack(); }
  T popCopy() { return vector.popCopy(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return vector.sizeOfExcludingThis(mallocSizeOf);
  }

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return vector.sizeOfIncludingThis(mallocSizeOf);
  }

  void trace(JSTracer* trc) {
    for (auto& elem : vector) {
      GCPolicy<T>::trace(trc, &elem, "vector element");
    }
  }

  bool traceWeak(JSTracer* trc) {
    mutableEraseIf(
        [trc](T& elem) { return !GCPolicy<T>::traceWeak(trc, &elem); });
    return !empty();
  }

  // Like eraseIf, but may mutate the contents of the vector.
  template <typename Pred>
  void mutableEraseIf(Pred pred) {
    T* src = begin();
    T* dst = begin();
    while (src != end()) {
      if (!pred(*src)) {
        if (src != dst) {
          *dst = std::move(*src);
        }
        dst++;
      }
      src++;
    }

    MOZ_ASSERT(dst <= end());
    shrinkBy(end() - dst);
  }
};

// AllocPolicy is optional. It has a default value declared in TypeDecls.h
template <typename T, typename AllocPolicy>
class MOZ_STACK_CLASS StackGCVector : public GCVector<T, 8, AllocPolicy> {
 public:
  using Base = GCVector<T, 8, AllocPolicy>;

 private:
  // Inherit constructor from GCVector.
  using Base::Base;
};

}  // namespace JS

namespace js {

template <typename Wrapper, typename T, size_t Capacity, typename AllocPolicy>
class WrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>, Wrapper> {
  using Vec = JS::GCVector<T, Capacity, AllocPolicy>;
  const Vec& vec() const { return static_cast<const Wrapper*>(this)->get(); }

 public:
  const AllocPolicy& allocPolicy() const { return vec().allocPolicy(); }
  size_t length() const { return vec().length(); }
  bool empty() const { return vec().empty(); }
  size_t capacity() const { return vec().capacity(); }
  const T* begin() const { return vec().begin(); }
  const T* end() const { return vec().end(); }
  const T& back() const { return vec().back(); }

  JS::Handle<T> operator[](size_t aIndex) const {
    return JS::Handle<T>::fromMarkedLocation(&vec().operator[](aIndex));
  }
};

template <typename Wrapper, typename T, size_t Capacity, typename AllocPolicy>
class MutableWrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>,
                                  Wrapper>
    : public WrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>,
                                  Wrapper> {
  using Vec = JS::GCVector<T, Capacity, AllocPolicy>;
  const Vec& vec() const { return static_cast<const Wrapper*>(this)->get(); }
  Vec& vec() { return static_cast<Wrapper*>(this)->get(); }

 public:
  const AllocPolicy& allocPolicy() const { return vec().allocPolicy(); }
  AllocPolicy& allocPolicy() { return vec().allocPolicy(); }
  const T* begin() const { return vec().begin(); }
  T* begin() { return vec().begin(); }
  const T* end() const { return vec().end(); }
  T* end() { return vec().end(); }
  const T& back() const { return vec().back(); }
  T& back() { return vec().back(); }

  JS::Handle<T> operator[](size_t aIndex) const {
    return JS::Handle<T>::fromMarkedLocation(&vec().operator[](aIndex));
  }
  JS::MutableHandle<T> operator[](size_t aIndex) {
    return JS::MutableHandle<T>::fromMarkedLocation(&vec().operator[](aIndex));
  }

  [[nodiscard]] bool initCapacity(size_t aRequest) {
    return vec().initCapacity(aRequest);
  }
  [[nodiscard]] bool reserve(size_t aRequest) {
    return vec().reserve(aRequest);
  }
  void shrinkBy(size_t aIncr) { vec().shrinkBy(aIncr); }
  [[nodiscard]] bool growBy(size_t aIncr) { return vec().growBy(aIncr); }
  [[nodiscard]] bool resize(size_t aNewLength) {
    return vec().resize(aNewLength);
  }
  void clear() { vec().clear(); }
  void clearAndFree() { vec().clearAndFree(); }
  template <typename U>
  [[nodiscard]] bool append(U&& aU) {
    return vec().append(std::forward<U>(aU));
  }
  template <typename... Args>
  [[nodiscard]] bool emplaceBack(Args&&... aArgs) {
    return vec().emplaceBack(std::forward<Args>(aArgs)...);
  }
  template <typename... Args>
  void infallibleEmplaceBack(Args&&... args) {
    vec().infallibleEmplaceBack(std::forward<Args>(args)...);
  }
  template <typename U>
  [[nodiscard]] bool appendAll(U&& aU) {
    return vec().appendAll(aU);
  }
  [[nodiscard]] bool appendN(const T& aT, size_t aN) {
    return vec().appendN(aT, aN);
  }
  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, const U* aEnd) {
    return vec().append(aBegin, aEnd);
  }
  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, size_t aLength) {
    return vec().append(aBegin, aLength);
  }
  template <typename U>
  void infallibleAppend(U&& aU) {
    vec().infallibleAppend(std::forward<U>(aU));
  }
  void infallibleAppendN(const T& aT, size_t aN) {
    vec().infallibleAppendN(aT, aN);
  }
  template <typename U>
  void infallibleAppend(const U* aBegin, const U* aEnd) {
    vec().infallibleAppend(aBegin, aEnd);
  }
  template <typename U>
  void infallibleAppend(const U* aBegin, size_t aLength) {
    vec().infallibleAppend(aBegin, aLength);
  }
  void popBack() { vec().popBack(); }
  T popCopy() { return vec().popCopy(); }
  void erase(T* aT) { vec().erase(aT); }
  void erase(T* aBegin, T* aEnd) { vec().erase(aBegin, aEnd); }
  template <typename Pred>
  void eraseIf(Pred pred) {
    vec().eraseIf(pred);
  }
  template <typename U>
  void eraseIfEqual(const U& u) {
    vec().eraseIfEqual(u);
  }
};

template <typename Wrapper, typename T, typename AllocPolicy>
class WrappedPtrOperations<JS::StackGCVector<T, AllocPolicy>, Wrapper>
    : public WrappedPtrOperations<
          typename JS::StackGCVector<T, AllocPolicy>::Base, Wrapper> {};

template <typename Wrapper, typename T, typename AllocPolicy>
class MutableWrappedPtrOperations<JS::StackGCVector<T, AllocPolicy>, Wrapper>
    : public MutableWrappedPtrOperations<
          typename JS::StackGCVector<T, AllocPolicy>::Base, Wrapper> {};

}  // namespace js

namespace JS {

// An automatically rooted GCVector for stack use.
template <typename T>
class RootedVector : public Rooted<StackGCVector<T>> {
  using Vec = StackGCVector<T>;
  using Base = Rooted<Vec>;

 public:
  explicit RootedVector(JSContext* cx) : Base(cx, Vec(cx)) {}
};

// For use in rust code, an analog to RootedVector that doesn't require
// instances to be destroyed in LIFO order.
template <typename T>
class PersistentRootedVector : public PersistentRooted<StackGCVector<T>> {
  using Vec = StackGCVector<T>;
  using Base = PersistentRooted<Vec>;

 public:
  explicit PersistentRootedVector(JSContext* cx) : Base(cx, Vec(cx)) {}
};

}  // namespace JS

#endif  // js_GCVector_h
