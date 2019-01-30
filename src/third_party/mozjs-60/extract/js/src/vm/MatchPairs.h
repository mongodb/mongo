/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_MatchPairs_h
#define vm_MatchPairs_h

#include "ds/LifoAlloc.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"

/*
 * RegExp match results are succinctly represented by pairs of integer
 * indices delimiting (start, limit] segments of the input string.
 *
 * The pair count for a given RegExp match is the capturing parentheses
 * count plus one for the "0 capturing paren" whole text match.
 */

namespace js {

struct MatchPair
{
    int32_t start;
    int32_t limit;

    MatchPair()
      : start(-1), limit(-1)
    { }

    MatchPair(int32_t start, int32_t limit)
      : start(start), limit(limit)
    { }

    size_t length()      const { MOZ_ASSERT(!isUndefined()); return limit - start; }
    bool isEmpty()       const { return length() == 0; }
    bool isUndefined()   const { return start < 0; }

    void displace(size_t amount) {
        start += (start < 0) ? 0 : amount;
        limit += (limit < 0) ? 0 : amount;
    }

    inline bool check() const {
        MOZ_ASSERT(limit >= start);
        MOZ_ASSERT_IF(start < 0, start == -1);
        MOZ_ASSERT_IF(limit < 0, limit == -1);
        return true;
    }
};

// MachPairs is used as base class for VectorMatchPairs but can also be
// stack-allocated (without a Vector) in JIT code.
class MatchPairs
{
  protected:
    /* Length of pairs_. */
    uint32_t pairCount_;

    /* Raw pointer into an allocated MatchPair buffer. */
    MatchPair* pairs_;

  protected:
    /* Not used directly: use ScopedMatchPairs or VectorMatchPairs. */
    MatchPairs()
      : pairCount_(0), pairs_(nullptr)
    { }

  protected:
    /* Functions used by friend classes. */
    friend class RegExpShared;
    friend class RegExpStatics;

    void forgetArray() { pairs_ = nullptr; }

    void checkAgainst(size_t inputLength) {
#ifdef DEBUG
        for (size_t i = 0; i < pairCount_; i++) {
            const MatchPair& p = (*this)[i];
            MOZ_ASSERT(p.check());
            if (p.isUndefined())
                continue;
            MOZ_ASSERT(size_t(p.limit) <= inputLength);
        }
#endif
    }

  public:
    /* Querying functions in the style of RegExpStatics. */
    bool   empty() const           { return pairCount_ == 0; }
    size_t pairCount() const       { MOZ_ASSERT(pairCount_ > 0); return pairCount_; }
    size_t parenCount() const      { return pairCount_ - 1; }

    static size_t offsetOfPairs() { return offsetof(MatchPairs, pairs_); }
    static size_t offsetOfPairCount() { return offsetof(MatchPairs, pairCount_); }

    int32_t* pairsRaw() { return reinterpret_cast<int32_t*>(pairs_); }

  public:
    size_t length() const { return pairCount_; }

    const MatchPair& operator[](size_t i) const {
        MOZ_ASSERT(i < pairCount_);
        return pairs_[i];
    }
    MatchPair& operator[](size_t i) {
        MOZ_ASSERT(i < pairCount_);
        return pairs_[i];
    }
};

class VectorMatchPairs : public MatchPairs
{
    Vector<MatchPair, 10, SystemAllocPolicy> vec_;

  protected:
    friend class RegExpShared;
    friend class RegExpStatics;

    /* MatchPair buffer allocator: set pairs_ and pairCount_. */
    bool allocOrExpandArray(size_t pairCount);

    bool initArrayFrom(VectorMatchPairs& copyFrom);
};

} /* namespace js */

#endif /* vm_MatchPairs_h */
