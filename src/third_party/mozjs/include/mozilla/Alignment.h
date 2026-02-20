/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Functionality related to memory alignment. */

#ifndef mozilla_Alignment_h
#define mozilla_Alignment_h

#include "mozilla/Attributes.h"
#include <stddef.h>
#include <stdint.h>

namespace mozilla {

/*
 * Declare the MOZ_ALIGNED_DECL macro for declaring aligned types.
 *
 * For instance,
 *
 *   MOZ_ALIGNED_DECL(8, char arr[2]);
 *
 * will declare a two-character array |arr| aligned to 8 bytes.
 */

#if defined(__GNUC__)
#  define MOZ_ALIGNED_DECL(_align, _type) _type __attribute__((aligned(_align)))
#elif defined(_MSC_VER)
#  define MOZ_ALIGNED_DECL(_align, _type) __declspec(align(_align)) _type
#else
#  warning "We don't know how to align variables on this compiler."
#  define MOZ_ALIGNED_DECL(_align, _type) _type
#endif

/*
 * AlignedElem<N> is a structure whose alignment is guaranteed to be at least N
 * bytes.
 *
 * We support 1, 2, 4, 8, and 16-byte alignment.
 */
template <size_t Align>
struct AlignedElem;

/*
 * We have to specialize this template because GCC doesn't like
 * __attribute__((aligned(foo))) where foo is a template parameter.
 */

template <>
struct AlignedElem<1> {
  MOZ_ALIGNED_DECL(1, uint8_t elem);
};

template <>
struct AlignedElem<2> {
  MOZ_ALIGNED_DECL(2, uint8_t elem);
};

template <>
struct AlignedElem<4> {
  MOZ_ALIGNED_DECL(4, uint8_t elem);
};

template <>
struct AlignedElem<8> {
  MOZ_ALIGNED_DECL(8, uint8_t elem);
};

template <>
struct AlignedElem<16> {
  MOZ_ALIGNED_DECL(16, uint8_t elem);
};

template <typename T>
struct MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS AlignedStorage2 {
  union U {
    char mBytes[sizeof(T)];
    uint64_t mDummy;
  } u;

  const T* addr() const { return reinterpret_cast<const T*>(u.mBytes); }
  T* addr() { return static_cast<T*>(static_cast<void*>(u.mBytes)); }

  AlignedStorage2() = default;

  // AlignedStorage2 is non-copyable: the default copy constructor violates
  // strict aliasing rules, per bug 1269319.
  AlignedStorage2(const AlignedStorage2&) = delete;
  void operator=(const AlignedStorage2&) = delete;
};

} /* namespace mozilla */

#endif /* mozilla_Alignment_h */
