/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A poison value that can be used to fill a memory space with
 * an address that leads to a safe crash when dereferenced.
 */

#ifndef mozilla_Poison_h
#define mozilla_Poison_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include <stdint.h>

MOZ_BEGIN_EXTERN_C

extern MFBT_DATA uintptr_t gMozillaPoisonValue;

/**
 * @return the poison value.
 */
inline uintptr_t mozPoisonValue()
{
  return gMozillaPoisonValue;
}

/**
 * Overwrite the memory block of aSize bytes at aPtr with the poison value.
 * aPtr MUST be aligned at a sizeof(uintptr_t) boundary.
 * Only an even number of sizeof(uintptr_t) bytes are overwritten, the last
 * few bytes (if any) is not overwritten.
 */
inline void mozWritePoison(void* aPtr, size_t aSize)
{
  const uintptr_t POISON = mozPoisonValue();
  char* p = (char*)aPtr;
  char* limit = p + aSize;
  MOZ_ASSERT((uintptr_t)aPtr % sizeof(uintptr_t) == 0, "bad alignment");
  MOZ_ASSERT(aSize >= sizeof(uintptr_t), "poisoning this object has no effect");
  for (; p < limit; p += sizeof(uintptr_t)) {
    *((uintptr_t*)p) = POISON;
  }
}

/**
 * Initialize the poison value.
 * This should only be called once.
 */
extern MFBT_API void mozPoisonValueInit();

/* Values annotated by CrashReporter */
extern MFBT_DATA uintptr_t gMozillaPoisonBase;
extern MFBT_DATA uintptr_t gMozillaPoisonSize;

MOZ_END_EXTERN_C

#endif /* mozilla_Poison_h */
