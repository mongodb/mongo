/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCVector_h
#define js_GCVector_h

#include "mozilla/Vector.h"

#include "js/GCPolicyAPI.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/Vector.h"

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
template <typename T,
          size_t MinInlineCapacity = 0,
          typename AllocPolicy = js::TempAllocPolicy>
class GCVector
{
    mozilla::Vector<T, MinInlineCapacity, AllocPolicy> vector;

  public:
    explicit GCVector(AllocPolicy alloc = AllocPolicy())
      : vector(alloc)
    {}

    GCVector(GCVector&& vec)
      : vector(mozilla::Move(vec.vector))
    {}

    GCVector& operator=(GCVector&& vec) {
        vector = mozilla::Move(vec.vector);
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

    bool initCapacity(size_t cap) { return vector.initCapacity(cap); }
    MOZ_MUST_USE bool reserve(size_t req) { return vector.reserve(req); }
    void shrinkBy(size_t amount) { return vector.shrinkBy(amount); }
    MOZ_MUST_USE bool growBy(size_t amount) { return vector.growBy(amount); }
    MOZ_MUST_USE bool resize(size_t newLen) { return vector.resize(newLen); }

    void clear() { return vector.clear(); }
    void clearAndFree() { return vector.clearAndFree(); }

    template<typename U> bool append(U&& item) { return vector.append(mozilla::Forward<U>(item)); }

    template<typename... Args>
    MOZ_MUST_USE bool
    emplaceBack(Args&&... args) {
        return vector.emplaceBack(mozilla::Forward<Args>(args)...);
    }

    template<typename U>
    void infallibleAppend(U&& aU) {
        return vector.infallibleAppend(mozilla::Forward<U>(aU));
    }
    void infallibleAppendN(const T& aT, size_t aN) {
        return vector.infallibleAppendN(aT, aN);
    }
    template<typename U> void
    infallibleAppend(const U* aBegin, const U* aEnd) {
        return vector.infallibleAppend(aBegin, aEnd);
    }
    template<typename U> void infallibleAppend(const U* aBegin, size_t aLength) {
        return vector.infallibleAppend(aBegin, aLength);
    }

    template<typename U>
    MOZ_MUST_USE bool appendAll(const U& aU) {
        return vector.append(aU.begin(), aU.end());
    }

    MOZ_MUST_USE bool appendN(const T& val, size_t count) {
        return vector.appendN(val, count);
    }

    template<typename U>
    MOZ_MUST_USE bool append(const U* aBegin, const U* aEnd) {
        return vector.append(aBegin, aEnd);
    }
    template<typename U>
    MOZ_MUST_USE bool append(const U* aBegin, size_t aLength) {
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

    static void trace(GCVector* vec, JSTracer* trc) { vec->trace(trc); }

    void trace(JSTracer* trc) {
        for (auto& elem : vector)
            GCPolicy<T>::trace(trc, &elem, "vector element");
    }

    bool needsSweep() const {
        return !this->empty();
    }

    void sweep() {
        uint32_t src, dst = 0;
        for (src = 0; src < length(); src++) {
            if (!GCPolicy<T>::needsSweep(&vector[src])) {
                if (dst != src)
                    vector[dst] = vector[src].unbarrieredGet();
                dst++;
            }
        }

        if (dst != length())
            vector.shrinkTo(dst);
    }
};

} // namespace JS

namespace js {

template <typename Wrapper, typename T, size_t Capacity, typename AllocPolicy>
class WrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>, Wrapper>
{
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
class MutableWrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>, Wrapper>
  : public WrappedPtrOperations<JS::GCVector<T, Capacity, AllocPolicy>, Wrapper>
{
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

    MOZ_MUST_USE bool initCapacity(size_t aRequest) { return vec().initCapacity(aRequest); }
    MOZ_MUST_USE bool reserve(size_t aRequest) { return vec().reserve(aRequest); }
    void shrinkBy(size_t aIncr) { vec().shrinkBy(aIncr); }
    MOZ_MUST_USE bool growBy(size_t aIncr) { return vec().growBy(aIncr); }
    MOZ_MUST_USE bool resize(size_t aNewLength) { return vec().resize(aNewLength); }
    MOZ_MUST_USE bool growByUninitialized(size_t aIncr) { return vec().growByUninitialized(aIncr); }
    void infallibleGrowByUninitialized(size_t aIncr) { vec().infallibleGrowByUninitialized(aIncr); }
    MOZ_MUST_USE bool resizeUninitialized(size_t aNewLength) { return vec().resizeUninitialized(aNewLength); }
    void clear() { vec().clear(); }
    void clearAndFree() { vec().clearAndFree(); }
    template<typename U>
    MOZ_MUST_USE bool append(U&& aU) { return vec().append(mozilla::Forward<U>(aU)); }
    template<typename... Args>
    MOZ_MUST_USE bool emplaceBack(Args&&... aArgs) {
        return vec().emplaceBack(mozilla::Forward<Args...>(aArgs...));
    }
    template<typename U>
    MOZ_MUST_USE bool appendAll(const U& aU) { return vec().appendAll(aU); }
    MOZ_MUST_USE bool appendN(const T& aT, size_t aN) { return vec().appendN(aT, aN); }
    template<typename U>
    MOZ_MUST_USE bool append(const U* aBegin, const U* aEnd) {
        return vec().append(aBegin, aEnd);
    }
    template<typename U>
    MOZ_MUST_USE bool append(const U* aBegin, size_t aLength) {
        return vec().append(aBegin, aLength);
    }
    template<typename U> void infallibleAppend(U&& aU) {
        vec().infallibleAppend(mozilla::Forward<U>(aU));
    }
    void infallibleAppendN(const T& aT, size_t aN) { vec().infallibleAppendN(aT, aN); }
    template<typename U> void infallibleAppend(const U* aBegin, const U* aEnd) {
        vec().infallibleAppend(aBegin, aEnd);
    }
    template<typename U> void infallibleAppend(const U* aBegin, size_t aLength) {
        vec().infallibleAppend(aBegin, aLength);
    }
    void popBack() { vec().popBack(); }
    T popCopy() { return vec().popCopy(); }
    template<typename U> T* insert(T* aP, U&& aVal) {
        return vec().insert(aP, mozilla::Forward<U>(aVal));
    }
    void erase(T* aT) { vec().erase(aT); }
    void erase(T* aBegin, T* aEnd) { vec().erase(aBegin, aEnd); }
};

} // namespace js

namespace JS {

// An automatically rooted vector for stack use.
template <typename T>
class AutoVector : public Rooted<GCVector<T, 8>> {
    using Vec = GCVector<T, 8>;
    using Base = Rooted<Vec>;
  public:
    explicit AutoVector(JSContext* cx) : Base(cx, Vec(cx)) {}
};

} // namespace JS

#endif // js_GCVector_h
