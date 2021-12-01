/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_RefCounted_h
#define js_RefCounted_h

#include "mozilla/Atomics.h"
#include "mozilla/RefCountType.h"

#include "js/Utility.h"

// These types implement the same interface as mozilla::(Atomic)RefCounted and
// must be used instead of mozilla::(Atomic)RefCounted for everything in
// SpiderMonkey. There are two reasons:
//  - Release() needs to call js_delete, not delete
//  - SpiderMonkey does not have MOZILLA_INTERNAL_API defined which can lead
//    to ODR violations that show up as spurious leak reports when ref-counted
//    types are allocated by SpiderMonkey and released by Gecko (or vice versa).

namespace js {

template <typename T>
class RefCounted
{
    static const MozRefCountType DEAD = 0xffffdead;

  protected:
    RefCounted() : mRefCnt(0) {}
    ~RefCounted() { MOZ_ASSERT(mRefCnt == DEAD); }

  public:
    void AddRef() const
    {
        MOZ_ASSERT(int32_t(mRefCnt) >= 0);
        ++mRefCnt;
    }

    void Release() const
    {
      MOZ_ASSERT(int32_t(mRefCnt) > 0);
      MozRefCountType cnt = --mRefCnt;
      if (0 == cnt) {
#ifdef DEBUG
          mRefCnt = DEAD;
#endif
          js_delete(const_cast<T*>(static_cast<const T*>(this)));
      }
    }

  private:
    mutable MozRefCountType mRefCnt;
};

template <typename T>
class AtomicRefCounted
{
    static const MozRefCountType DEAD = 0xffffdead;

  protected:
    AtomicRefCounted() : mRefCnt(0) {}
    ~AtomicRefCounted() { MOZ_ASSERT(mRefCnt == DEAD); }

  public:
    void AddRef() const
    {
        MOZ_ASSERT(int32_t(mRefCnt) >= 0);
        ++mRefCnt;
    }

    void Release() const
    {
        MOZ_ASSERT(int32_t(mRefCnt) > 0);
        MozRefCountType cnt = --mRefCnt;
        if (0 == cnt) {
#ifdef DEBUG
            mRefCnt = DEAD;
#endif
            js_delete(const_cast<T*>(static_cast<const T*>(this)));
        }
    }

  private:
    mutable mozilla::Atomic<MozRefCountType> mRefCnt;
};

} // namespace js

#endif /* js_RefCounted_h */
