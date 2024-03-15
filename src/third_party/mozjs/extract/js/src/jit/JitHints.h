/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitHints_h
#define jit_JitHints_h

#include "mozilla/BloomFilter.h"
#include "vm/JSScript.h"

namespace js::jit {

/*
 * The JitHintsMap implements a BitBloomFilter to track whether or not a script,
 * identified by filename+sourceStart, has been baseline compiled before in the
 * same process.  This can occur frequently during navigations.
 *
 * The bloom filter allows us to have very efficient storage and lookup costs,
 * at the expense of occasional false positives.  The number of entries added
 * to the bloom filter is monitored in order to try and keep the false
 * positivity rate below 1%.  If the entry count exceeds MaxEntries_, which
 * indicates the false positivity rate may exceed 1.5%, then the filter is
 * completely cleared to reset the cache.
 */

class JitHintsMap {
  // ScriptKey is a hash on the filename+sourceStart.
  using ScriptKey = HashNumber;

  static constexpr uint32_t CacheSize_ = 16;
  mozilla::BitBloomFilter<CacheSize_, ScriptKey> map_;

  /*
   * MaxEntries_ is the approximate entry count for which the
   * false positivity rate will exceed p=0.015 using k=2 and m=2**CacheSize.
   * Formula is as follows:
   * MaxEntries_ = floor(m / (-k / ln(1-exp(ln(p) / k))))
   */
  static constexpr uint32_t MaxEntries_ = 4281;
  static_assert(CacheSize_ == 16 && MaxEntries_ == 4281,
                "MaxEntries should be recalculated for given CacheSize.");

  uint32_t entryCount_ = 0;

  ScriptKey getScriptKey(JSScript* script) const;
  void incrementEntryCount();

 public:
  void setEagerBaselineHint(JSScript* script);
  bool mightHaveEagerBaselineHint(JSScript* script) const;
};

}  // namespace js::jit
#endif /* jit_JitHints_h */
