/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitHints_h
#define jit_JitHints_h

#include "mozilla/BloomFilter.h"
#include "mozilla/HashTable.h"
#include "mozilla/LinkedList.h"
#include "jit/JitOptions.h"
#include "vm/BytecodeLocation.h"
#include "vm/JSScript.h"

namespace js::jit {

/*
 *
 * [SMDOC] JitHintsMap
 *
 * The Jit hints map is an in process cache used to collect Baseline and Ion
 * JIT hints to try and skip as much of the warmup as possible and jump
 * straight into those tiers.  Whenever a script enters one of these tiers
 * a hint is recorded in this cache using the script's filename+sourceStart
 * value, and if we ever encounter this script again later, e.g. during a
 * navigation, then we try to eagerly compile it into baseline and ion
 * based on its previous execution history.
 */

class JitHintsMap {
  // ScriptKey is a hash on the filename+sourceStart.
  using ScriptKey = HashNumber;
  ScriptKey getScriptKey(JSScript* script) const;

  /* Ion Hints
   * -------------------------------------------------------------------------
   * This implementation uses a combination of a HashMap and PriorityQueue
   * to store a threshold value for each script that has been Ion compiled.
   * The PriorityQueue is used to track the least recently used entries so
   * that the cache does not exceed |IonHintMaxEntries| entries.
   *
   * After a script has entered Ion the first time, an eager threshold hint
   * value is set using the warmup counter of when the last IC stub was
   * attached, if available. This minimizes the risk that the script will
   * bailout. If that script is bailout invalidated, the threshold value
   * is incremented by |InvalidationThresholdIncrement| up to a maximum value of
   * |JitOptions.normalIonWarmUpThreshold|.
   *
   * Each IonHint object also contains a list of bytecode offsets for locations
   * of monomorphic inline calls that is used as a hint for future compilations.
   *
   */
  class IonHint : public mozilla::LinkedListElement<IonHint> {
    ScriptKey key_ = 0;

    // We use a value of 0 to indicate that the script has not entered Ion
    // yet, but has been monomorphically inlined and Ion compiled into
    // another script and contains bytecode offsets of a nested call.
    uint32_t threshold_ = 0;

    // List of bytecode offsets that have been successfully inlined with
    // a state of monomorphic inline.
    Vector<uint32_t, 0, SystemAllocPolicy> monomorphicInlineOffsets;

   public:
    explicit IonHint(ScriptKey key) { key_ = key; }

    void initThreshold(uint32_t threshold) { threshold_ = threshold; }

    uint32_t threshold() { return threshold_; }

    void incThreshold(uint32_t inc) {
      uint32_t newThreshold = threshold() + inc;
      threshold_ = (newThreshold > JitOptions.normalIonWarmUpThreshold)
                       ? JitOptions.normalIonWarmUpThreshold
                       : newThreshold;
    }

    bool hasSpaceForMonomorphicInlineEntry() {
      return monomorphicInlineOffsets.length() < MonomorphicInlineMaxEntries;
    }

    bool hasMonomorphicInlineOffset(uint32_t offset) {
      for (uint32_t iterOffset : monomorphicInlineOffsets) {
        if (iterOffset == offset) {
          return true;
        }
      }
      return false;
    }

    bool addMonomorphicInlineOffset(uint32_t newOffset) {
      MOZ_ASSERT(hasSpaceForMonomorphicInlineEntry());

      if (hasMonomorphicInlineOffset(newOffset)) {
        return true;
      }
      return monomorphicInlineOffsets.append(newOffset);
    }

    ScriptKey key() {
      MOZ_ASSERT(key_ != 0, "Should have valid key.");
      return key_;
    }
  };

  using ScriptToHintMap =
      HashMap<ScriptKey, IonHint*, js::DefaultHasher<ScriptKey>,
              js::SystemAllocPolicy>;
  using IonHintPriorityQueue = mozilla::LinkedList<IonHint>;

  static constexpr uint32_t InvalidationThresholdIncrement = 500;
  static constexpr uint32_t IonHintMaxEntries = 5000;
  static constexpr uint32_t MonomorphicInlineMaxEntries = 16;

  static uint32_t IonHintEagerThresholdValue(uint32_t lastStubCounter,
                                             bool hasPretenuredAllocSites);

  ScriptToHintMap ionHintMap_;
  IonHintPriorityQueue ionHintQueue_;

  /* Baseline Hints
   * --------------------------------------------------------------------------
   * This implementation uses a BitBloomFilter to track whether or not a script
   * has been baseline compiled before in the same process.  This can occur
   * frequently during navigations.
   *
   * The bloom filter allows us to have very efficient storage and lookup costs,
   * at the expense of occasional false positives.  Using a bloom filter also
   * allows us to have many more entries at minimal memory and allocation cost.
   * The number of entries added to the bloom filter is monitored in order to
   * try and keep the false positivity rate below 1%.  If the entry count
   * exceeds MaxEntries_, which indicates the false positivity rate may exceed
   * 1.5%, then the filter is completely cleared to reset the cache.
   */
  static constexpr uint32_t EagerBaselineCacheSize_ = 16;
  mozilla::BitBloomFilter<EagerBaselineCacheSize_, ScriptKey> baselineHintMap_;

  /*
   * MaxEntries_ is the approximate entry count for which the
   * false positivity rate will exceed p=0.015 using k=2 and m=2**CacheSize.
   * Formula is as follows:
   * MaxEntries_ = floor(m / (-k / ln(1-exp(ln(p) / k))))
   */
  static constexpr uint32_t MaxEntries_ = 4281;
  static_assert(EagerBaselineCacheSize_ == 16 && MaxEntries_ == 4281,
                "MaxEntries should be recalculated for given CacheSize.");

  uint32_t baselineEntryCount_ = 0;
  void incrementBaselineEntryCount();

  void updateAsRecentlyUsed(IonHint* hint);
  IonHint* addIonHint(ScriptKey key, ScriptToHintMap::AddPtr& p);

 public:
  ~JitHintsMap();

  void setEagerBaselineHint(JSScript* script);
  bool mightHaveEagerBaselineHint(JSScript* script) const;

  bool recordIonCompilation(JSScript* script);
  bool getIonThresholdHint(JSScript* script, uint32_t& thresholdOut);

  bool addMonomorphicInlineLocation(JSScript* script, BytecodeLocation loc);
  bool hasMonomorphicInlineHintAtOffset(JSScript* script, uint32_t offset);

  void recordInvalidation(JSScript* script);
};

}  // namespace js::jit
#endif /* jit_JitHints_h */
