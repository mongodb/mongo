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
 * This class, and the corresponding macro MOZ_ALIGNOF, figures out how many
 * bytes of alignment a given type needs.
 */
template<typename T>
class AlignmentFinder
{
  struct Aligner
  {
    char mChar;
    T mT;
  };

public:
  static const size_t alignment = sizeof(Aligner) - sizeof(T);
};

#define MOZ_ALIGNOF(T) mozilla::AlignmentFinder<T>::alignment

namespace detail {
template<typename T>
struct AlignasHelper
{
  T mT;
};
} // namespace detail

/*
 * Use this instead of alignof to align struct field as if it is inside
 * a struct. On some platforms, there exist types which have different
 * alignment between when it is used on its own and when it is used on
 * a struct field.
 *
 * Known examples are 64bit types (uint64_t, double) on 32bit Linux,
 * where they have 8byte alignment on their own, and 4byte alignment
 * when in struct.
 */
#define MOZ_ALIGNAS_IN_STRUCT(T) alignas(mozilla::detail::AlignasHelper<T>)

/*
 * Declare the MOZ_ALIGNED_DECL macro for declaring aligned types.
 *
 * For instance,
 *
 *   MOZ_ALIGNED_DECL(char arr[2], 8);
 *
 * will declare a two-character array |arr| aligned to 8 bytes.
 */

#if defined(__GNUC__)
#  define MOZ_ALIGNED_DECL(_type, _align) \
     _type __attribute__((aligned(_align)))
#elif defined(_MSC_VER)
#  define MOZ_ALIGNED_DECL(_type, _align) \
     __declspec(align(_align)) _type
#else
#  warning "We don't know how to align variables on this compiler."
#  define MOZ_ALIGNED_DECL(_type, _align) _type
#endif

/*
 * AlignedElem<N> is a structure whose alignment is guaranteed to be at least N
 * bytes.
 *
 * We support 1, 2, 4, 8, and 16-byte alignment.
 */
template<size_t Align>
struct AlignedElem;

/*
 * We have to specialize this template because GCC doesn't like
 * __attribute__((aligned(foo))) where foo is a template parameter.
 */

template<>
struct AlignedElem<1>
{
  MOZ_ALIGNED_DECL(uint8_t elem, 1);
};

template<>
struct AlignedElem<2>
{
  MOZ_ALIGNED_DECL(uint8_t elem, 2);
};

template<>
struct AlignedElem<4>
{
  MOZ_ALIGNED_DECL(uint8_t elem, 4);
};

template<>
struct AlignedElem<8>
{
  MOZ_ALIGNED_DECL(uint8_t elem, 8);
};

template<>
struct AlignedElem<16>
{
  MOZ_ALIGNED_DECL(uint8_t elem, 16);
};

template<typename T>
struct MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS AlignedStorage2
{
  union U
  {
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
