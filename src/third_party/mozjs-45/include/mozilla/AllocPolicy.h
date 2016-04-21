/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * An allocation policy concept, usable for structures and algorithms to
 * control how memory is allocated and how failures are handled.
 */

#ifndef mozilla_AllocPolicy_h
#define mozilla_AllocPolicy_h

#include "mozilla/TemplateLib.h"

#include <stddef.h>
#include <stdlib.h>

namespace mozilla {

/*
 * Allocation policies are used to implement the standard allocation behaviors
 * in a customizable way.  Additionally, custom behaviors may be added to these
 * behaviors, such as additionally reporting an error through an out-of-band
 * mechanism when OOM occurs.  The concept modeled here is as follows:
 *
 *  - public copy constructor, assignment, destructor
 *  - template <typename T> T* maybe_pod_malloc(size_t)
 *      Fallible, but doesn't report an error on OOM.
 *  - template <typename T> T* maybe_pod_calloc(size_t)
 *      Fallible, but doesn't report an error on OOM.
 *  - template <typename T> T* maybe_pod_realloc(T*, size_t, size_t)
 *      Fallible, but doesn't report an error on OOM.  The old allocation
 *      size is passed in, in addition to the new allocation size requested.
 *  - template <typename T> T* pod_malloc(size_t)
 *      Responsible for OOM reporting when null is returned.
 *  - template <typename T> T* pod_calloc(size_t)
 *      Responsible for OOM reporting when null is returned.
 *  - template <typename T> T* pod_realloc(T*, size_t, size_t)
 *      Responsible for OOM reporting when null is returned.  The old allocation
 *      size is passed in, in addition to the new allocation size requested.
 *  - void free_(void*)
 *  - void reportAllocOverflow() const
 *      Called on allocation overflow (that is, an allocation implicitly tried
 *      to allocate more than the available memory space -- think allocating an
 *      array of large-size objects, where N * size overflows) before null is
 *      returned.
 *  - bool checkSimulatedOOM() const
 *      Some clients generally allocate memory yet in some circumstances won't
 *      need to do so. For example, appending to a vector with a small amount of
 *      inline storage generally allocates memory, but no allocation occurs
 *      unless appending exceeds inline storage. But for testing purposes, it
 *      can be useful to treat *every* operation as allocating.
 *      Clients (such as this hypothetical append method implementation) should
 *      call this method in situations that don't allocate, but could generally,
 *      to support this. The default behavior should return true; more
 *      complicated behavior might be to return false only after a certain
 *      number of allocations-or-check-simulated-OOMs (coordinating with the
 *      other AllocPolicy methods) have occurred.
 *
 * mfbt provides (and typically uses by default) only MallocAllocPolicy, which
 * does nothing more than delegate to the malloc/alloc/free functions.
 */

/*
 * A policy that straightforwardly uses malloc/calloc/realloc/free and adds no
 * extra behaviors.
 */
class MallocAllocPolicy
{
public:
  template <typename T>
  T* maybe_pod_malloc(size_t aNumElems)
  {
    if (aNumElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value) {
      return nullptr;
    }
    return static_cast<T*>(malloc(aNumElems * sizeof(T)));
  }

  template <typename T>
  T* maybe_pod_calloc(size_t aNumElems)
  {
    return static_cast<T*>(calloc(aNumElems, sizeof(T)));
  }

  template <typename T>
  T* maybe_pod_realloc(T* aPtr, size_t aOldSize, size_t aNewSize)
  {
    if (aNewSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value) {
      return nullptr;
    }
    return static_cast<T*>(realloc(aPtr, aNewSize * sizeof(T)));
  }

  template <typename T>
  T* pod_malloc(size_t aNumElems)
  {
    return maybe_pod_malloc<T>(aNumElems);
  }

  template <typename T>
  T* pod_calloc(size_t aNumElems)
  {
    return maybe_pod_calloc<T>(aNumElems);
  }

  template <typename T>
  T* pod_realloc(T* aPtr, size_t aOldSize, size_t aNewSize)
  {
    return maybe_pod_realloc<T>(aPtr, aOldSize, aNewSize);
  }

  void free_(void* aPtr)
  {
    free(aPtr);
  }

  void reportAllocOverflow() const
  {
  }

  bool checkSimulatedOOM() const
  {
    return true;
  }
};

} // namespace mozilla

#endif /* mozilla_AllocPolicy_h */
