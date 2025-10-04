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
#include <string.h>

MOZ_BEGIN_EXTERN_C

extern MFBT_DATA uintptr_t gMozillaPoisonValue;

/**
 * @return the poison value.
 */
inline uintptr_t mozPoisonValue() { return gMozillaPoisonValue; }

/**
 * Overwrite the memory block of aSize bytes at aPtr with the poison value.
 * Only a multiple of sizeof(uintptr_t) bytes are overwritten, the last
 * few bytes (if any) are not overwritten.
 */
inline void mozWritePoison(void* aPtr, size_t aSize) {
  const uintptr_t POISON = mozPoisonValue();
  char* p = (char*)aPtr;
  char* limit = p + (aSize & ~(sizeof(uintptr_t) - 1));
  MOZ_ASSERT(aSize >= sizeof(uintptr_t), "poisoning this object has no effect");
  for (; p < limit; p += sizeof(uintptr_t)) {
    memcpy(p, &POISON, sizeof(POISON));
  }
}

/* Values annotated by CrashReporter */
extern MFBT_DATA uintptr_t gMozillaPoisonBase;
extern MFBT_DATA uintptr_t gMozillaPoisonSize;

MOZ_END_EXTERN_C

#if defined(__cplusplus)

namespace mozilla {

/**
 * A version of CorruptionCanary that is suitable as a member of objects that
 * are statically allocated.
 */
class CorruptionCanaryForStatics {
 public:
  constexpr CorruptionCanaryForStatics() : mValue(kCanarySet) {}

  // This is required to avoid static constructor bloat.
  ~CorruptionCanaryForStatics() = default;

  void Check() const {
    if (mValue != kCanarySet) {
      MOZ_CRASH("Canary check failed, check lifetime");
    }
  }

 protected:
  uintptr_t mValue;

 private:
  static const uintptr_t kCanarySet = 0x0f0b0f0b;
};

/**
 * This class is designed to cause crashes when various kinds of memory
 * corruption are observed. For instance, let's say we have a class C where we
 * suspect out-of-bounds writes to some members.  We can insert a member of type
 * Poison near the members we suspect are being corrupted by out-of-bounds
 * writes.  Or perhaps we have a class K we suspect is subject to use-after-free
 * violations, in which case it doesn't particularly matter where in the class
 * we add the member of type Poison.
 *
 * In either case, we then insert calls to Check() throughout the code.  Doing
 * so enables us to narrow down the location where the corruption is occurring.
 * A pleasant side-effect of these additional Check() calls is that crash
 * signatures may become more regular, as crashes will ideally occur
 * consolidated at the point of a Check(), rather than scattered about at
 * various uses of the corrupted memory.
 */
class CorruptionCanary : public CorruptionCanaryForStatics {
 public:
  constexpr CorruptionCanary() = default;

  ~CorruptionCanary() {
    Check();
    mValue = mozPoisonValue();
  }
};

}  // namespace mozilla

#endif

#endif /* mozilla_Poison_h */
