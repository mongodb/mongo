/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedMem_h
#define vm_SharedMem_h

#include "mozilla/TypeTraits.h"

template<typename T>
class SharedMem
{
    static_assert(mozilla::IsPointer<T>::value,
                  "SharedMem encapsulates pointer types");

    enum Sharedness {
	IsUnshared,
	IsShared
    };

    T ptr_;
#ifdef DEBUG
    Sharedness sharedness_;
#endif

    SharedMem(T ptr, Sharedness sharedness)
      : ptr_(ptr)
#ifdef DEBUG
      , sharedness_(sharedness)
#endif
    {}

  public:
    // Create a SharedMem<T> that is an unshared nullptr.
    SharedMem()
      : ptr_(nullptr)
#ifdef DEBUG
      , sharedness_(IsUnshared)
#endif
    {}

    // Create a SharedMem<T> that's shared/unshared in the same way as
    // "forSharedness".
    SharedMem(T ptr, const SharedMem& forSharedness)
      : ptr_(ptr)
#ifdef DEBUG
      , sharedness_(forSharedness.sharedness_)
#endif
    {}

    // Create a SharedMem<T> that's marked as shared.
    static SharedMem shared(void* p) {
        return SharedMem(static_cast<T>(p), IsShared);
    }

    // Create a SharedMem<T> that's marked as unshared.
    static SharedMem unshared(void* p) {
        return SharedMem(static_cast<T>(p), IsUnshared);
    }

    SharedMem& operator =(const SharedMem& that) {
        ptr_ = that.ptr_;
#ifdef DEBUG
        sharedness_ = that.sharedness_;
#endif
        return *this;
    }

    // Reinterpret-cast the pointer to type U, preserving sharedness.
    template<typename U>
    inline SharedMem<U> cast() const {
#ifdef DEBUG
        MOZ_ASSERT(asValue() % sizeof(mozilla::Conditional<mozilla::IsVoid<typename mozilla::RemovePointer<U>::Type>::value,
                                                           char,
                                                           typename mozilla::RemovePointer<U>::Type>) == 0);
        if (sharedness_ == IsUnshared)
            return SharedMem<U>::unshared(unwrap());
#endif
        return SharedMem<U>::shared(unwrap());
    }

    explicit operator bool() { return ptr_ != nullptr; }

    SharedMem operator +(size_t offset) {
        return SharedMem(ptr_ + offset, *this);
    }

    SharedMem operator -(size_t offset) {
        return SharedMem(ptr_ - offset, *this);
    }

    SharedMem operator ++() {
        ptr_++;
        return *this;
    }

    SharedMem operator ++(int) {
        SharedMem<T> result(*this);
        ptr_++;
        return result;
    }

    SharedMem operator --() {
        ptr_--;
        return *this;
    }

    SharedMem operator --(int) {
        SharedMem<T> result(*this);
        ptr_--;
        return result;
    }

    uintptr_t asValue() const {
        return reinterpret_cast<uintptr_t>(ptr_);
    }

    // Cast to char*, add nbytes, and cast back to T.  Simplifies code in a few places.
    SharedMem addBytes(size_t nbytes) {
        MOZ_ASSERT(nbytes % sizeof(mozilla::Conditional<mozilla::IsVoid<typename mozilla::RemovePointer<T>::Type>::value,
                                                        char,
                                                        typename mozilla::RemovePointer<T>::Type>) == 0);
        return SharedMem(reinterpret_cast<T>(reinterpret_cast<char*>(ptr_) + nbytes), *this);
    }

    T unwrap() const {
        return ptr_;
    }

    T unwrapUnshared() const {
        MOZ_ASSERT(sharedness_ == IsUnshared);
        return ptr_;
    }

    uintptr_t unwrapValue() const {
        return reinterpret_cast<uintptr_t>(ptr_);
    }
};

template<typename T>
inline bool
operator >=(const SharedMem<T>& a, const SharedMem<T>& b)
{
    return a.unwrap() >= b.unwrap();
}

template<typename T>
inline bool
operator >=(const void* a, const SharedMem<T>& b)
{
    return a >= b.unwrap();
}

template<typename T>
inline bool
operator ==(const void* a, const SharedMem<T>& b)
{
    return a == b.unwrap();
}

template<typename T>
inline bool
operator ==(const SharedMem<T>& a, decltype(nullptr) b)
{
    return a.unwrap() == b;
}

template<typename T>
inline bool
operator ==(const SharedMem<T>& a, const SharedMem<T>& b)
{
    return a.unwrap() == b.unwrap();
}

template<typename T>
inline bool
operator !=(const SharedMem<T>& a, decltype(nullptr) b)
{
    return a.unwrap() != b;
}

template<typename T>
inline bool
operator !=(const SharedMem<T>& a, const SharedMem<T>& b)
{
    return a.unwrap() != b.unwrap();
}

template<typename T>
inline bool
operator >(const SharedMem<T>& a, const SharedMem<T>& b)
{
    return a.unwrap() > b.unwrap();
}

template<typename T>
inline bool
operator >(const void* a, const SharedMem<T>& b)
{
    return a > b.unwrap();
}

template<typename T>
inline bool
operator <=(const SharedMem<T>& a, const SharedMem<T>& b)
{
    return a.unwrap() <= b.unwrap();
}

template<typename T>
inline bool
operator <=(const void* a, const SharedMem<T>& b)
{
    return a <= b.unwrap();
}

template<typename T>
inline bool
operator <(const void* a, const SharedMem<T>& b)
{
    return a < b.unwrap();
}

#endif // vm_SharedMem_h
