/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitcodeMap.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"

#include "gc/Marking.h"
#include "gc/Statistics.h"
#include "jit/BaselineJIT.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "js/Vector.h"
#include "vm/BytecodeLocation.h"  // for BytecodeLocation
#include "vm/GeckoProfiler.h"

#include "vm/BytecodeLocation-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"

using mozilla::Maybe;

namespace js {
namespace jit {

static inline JitcodeRegionEntry RegionAtAddr(
    const JitcodeGlobalEntry::IonEntry& entry, void* ptr, uint32_t* ptrOffset) {
  MOZ_ASSERT(entry.containsPointer(ptr));
  *ptrOffset = reinterpret_cast<uint8_t*>(ptr) -
               reinterpret_cast<uint8_t*>(entry.nativeStartAddr());

  uint32_t regionIdx = entry.regionTable()->findRegionEntry(*ptrOffset);
  MOZ_ASSERT(regionIdx < entry.regionTable()->numRegions());

  return entry.regionTable()->regionEntry(regionIdx);
}

void* JitcodeGlobalEntry::IonEntry::canonicalNativeAddrFor(void* ptr) const {
  uint32_t ptrOffset;
  JitcodeRegionEntry region = RegionAtAddr(*this, ptr, &ptrOffset);
  return (void*)(((uint8_t*)nativeStartAddr()) + region.nativeOffset());
}

bool JitcodeGlobalEntry::IonEntry::callStackAtAddr(
    void* ptr, BytecodeLocationVector& results, uint32_t* depth) const {
  uint32_t ptrOffset;
  JitcodeRegionEntry region = RegionAtAddr(*this, ptr, &ptrOffset);
  *depth = region.scriptDepth();

  JitcodeRegionEntry::ScriptPcIterator locationIter = region.scriptPcIterator();
  MOZ_ASSERT(locationIter.hasMore());
  bool first = true;
  while (locationIter.hasMore()) {
    uint32_t scriptIdx, pcOffset;
    locationIter.readNext(&scriptIdx, &pcOffset);
    // For the first entry pushed (innermost frame), the pcOffset is obtained
    // from the delta-run encodings.
    if (first) {
      pcOffset = region.findPcOffset(ptrOffset, pcOffset);
      first = false;
    }
    JSScript* script = getScript(scriptIdx);
    jsbytecode* pc = script->offsetToPC(pcOffset);
    if (!results.append(BytecodeLocation(script, pc))) {
      return false;
    }
  }

  return true;
}

uint32_t JitcodeGlobalEntry::IonEntry::callStackAtAddr(
    void* ptr, const char** results, uint32_t maxResults) const {
  MOZ_ASSERT(maxResults >= 1);

  uint32_t ptrOffset;
  JitcodeRegionEntry region = RegionAtAddr(*this, ptr, &ptrOffset);

  JitcodeRegionEntry::ScriptPcIterator locationIter = region.scriptPcIterator();
  MOZ_ASSERT(locationIter.hasMore());
  uint32_t count = 0;
  while (locationIter.hasMore()) {
    uint32_t scriptIdx, pcOffset;

    locationIter.readNext(&scriptIdx, &pcOffset);
    MOZ_ASSERT(getStr(scriptIdx));

    results[count++] = getStr(scriptIdx);
    if (count >= maxResults) {
      break;
    }
  }

  return count;
}

uint64_t JitcodeGlobalEntry::IonEntry::lookupRealmID(void* ptr) const {
  uint32_t ptrOffset;
  JitcodeRegionEntry region = RegionAtAddr(*this, ptr, &ptrOffset);
  JitcodeRegionEntry::ScriptPcIterator locationIter = region.scriptPcIterator();
  MOZ_ASSERT(locationIter.hasMore());
  uint32_t scriptIdx, pcOffset;
  locationIter.readNext(&scriptIdx, &pcOffset);

  JSScript* script = getScript(scriptIdx);
  return script->realm()->creationOptions().profilerRealmID();
}

void JitcodeGlobalEntry::IonEntry::destroy() {
  // The region table is stored at the tail of the compacted data,
  // which means the start of the region table is a pointer to
  // the _middle_ of the memory space allocated for it.
  //
  // When freeing it, obtain the payload start pointer first.
  if (regionTable_) {
    js_free((void*)(regionTable_->payloadStart()));
  }
  regionTable_ = nullptr;

  // Free the scriptList strs.
  for (uint32_t i = 0; i < scriptList_->size; i++) {
    js_free(scriptList_->pairs[i].str);
    scriptList_->pairs[i].str = nullptr;
  }

  // Free the script list
  js_free(scriptList_);
  scriptList_ = nullptr;
}

void* JitcodeGlobalEntry::BaselineEntry::canonicalNativeAddrFor(
    void* ptr) const {
  // TODO: We can't yet normalize Baseline addresses until we unify
  // BaselineScript's PCMappingEntries with JitcodeGlobalTable.
  return ptr;
}

bool JitcodeGlobalEntry::BaselineEntry::callStackAtAddr(
    void* ptr, BytecodeLocationVector& results, uint32_t* depth) const {
  MOZ_ASSERT(containsPointer(ptr));
  MOZ_ASSERT(script_->hasBaselineScript());

  uint8_t* addr = reinterpret_cast<uint8_t*>(ptr);
  jsbytecode* pc =
      script_->baselineScript()->approximatePcForNativeAddress(script_, addr);
  if (!results.append(BytecodeLocation(script_, pc))) {
    return false;
  }

  *depth = 1;

  return true;
}

uint32_t JitcodeGlobalEntry::BaselineEntry::callStackAtAddr(
    void* ptr, const char** results, uint32_t maxResults) const {
  MOZ_ASSERT(containsPointer(ptr));
  MOZ_ASSERT(maxResults >= 1);

  results[0] = str();
  return 1;
}

uint64_t JitcodeGlobalEntry::BaselineEntry::lookupRealmID() const {
  return script_->realm()->creationOptions().profilerRealmID();
}

void JitcodeGlobalEntry::BaselineEntry::destroy() {
  if (!str_) {
    return;
  }
  js_free((void*)str_);
  str_ = nullptr;
}

void* JitcodeGlobalEntry::BaselineInterpreterEntry::canonicalNativeAddrFor(
    void* ptr) const {
  return ptr;
}

bool JitcodeGlobalEntry::BaselineInterpreterEntry::callStackAtAddr(
    void* ptr, BytecodeLocationVector& results, uint32_t* depth) const {
  MOZ_CRASH("shouldn't be called for BaselineInterpreter entries");
}

uint32_t JitcodeGlobalEntry::BaselineInterpreterEntry::callStackAtAddr(
    void* ptr, const char** results, uint32_t maxResults) const {
  MOZ_CRASH("shouldn't be called for BaselineInterpreter entries");
}

uint64_t JitcodeGlobalEntry::BaselineInterpreterEntry::lookupRealmID() const {
  MOZ_CRASH("shouldn't be called for BaselineInterpreter entries");
}

static int ComparePointers(const void* a, const void* b) {
  const uint8_t* a_ptr = reinterpret_cast<const uint8_t*>(a);
  const uint8_t* b_ptr = reinterpret_cast<const uint8_t*>(b);
  if (a_ptr < b_ptr) {
    return -1;
  }
  if (a_ptr > b_ptr) {
    return 1;
  }
  return 0;
}

/* static */
int JitcodeGlobalEntry::compare(const JitcodeGlobalEntry& ent1,
                                const JitcodeGlobalEntry& ent2) {
  // Both parts of compare cannot be a query.
  MOZ_ASSERT(!(ent1.isQuery() && ent2.isQuery()));

  // Ensure no overlaps for non-query lookups.
  MOZ_ASSERT_IF(!ent1.isQuery() && !ent2.isQuery(), !ent1.overlapsWith(ent2));

  // For two non-query entries, just comapare the start addresses.
  if (!ent1.isQuery() && !ent2.isQuery()) {
    return ComparePointers(ent1.nativeStartAddr(), ent2.nativeStartAddr());
  }

  void* ptr = ent1.isQuery() ? ent1.nativeStartAddr() : ent2.nativeStartAddr();
  const JitcodeGlobalEntry& ent = ent1.isQuery() ? ent2 : ent1;
  int flip = ent1.isQuery() ? 1 : -1;

  if (ent.startsBelowPointer(ptr)) {
    if (ent.endsAbovePointer(ptr)) {
      return 0;
    }

    // query ptr > entry
    return flip * 1;
  }

  // query ptr < entry
  return flip * -1;
}

JitcodeGlobalTable::Enum::Enum(JitcodeGlobalTable& table, JSRuntime* rt)
    : Range(table), rt_(rt), next_(cur_ ? cur_->tower_->next(0) : nullptr) {
  for (int level = JitcodeSkiplistTower::MAX_HEIGHT - 1; level >= 0; level--) {
    prevTower_[level] = nullptr;
  }
}

void JitcodeGlobalTable::Enum::popFront() {
  MOZ_ASSERT(!empty());

  // Did not remove current entry; advance prevTower_.
  if (cur_ != table_.freeEntries_) {
    for (int level = cur_->tower_->height() - 1; level >= 0; level--) {
      JitcodeGlobalEntry* prevTowerEntry = prevTower_[level];

      if (prevTowerEntry) {
        if (prevTowerEntry->tower_->next(level) == cur_) {
          prevTower_[level] = cur_;
        }
      } else {
        prevTower_[level] = table_.startTower_[level];
      }
    }
  }

  cur_ = next_;
  if (!empty()) {
    next_ = cur_->tower_->next(0);
  }
}

void JitcodeGlobalTable::Enum::removeFront() {
  MOZ_ASSERT(!empty());
  table_.releaseEntry(*cur_, prevTower_, rt_);
}

const JitcodeGlobalEntry* JitcodeGlobalTable::lookupForSampler(
    void* ptr, JSRuntime* rt, uint64_t samplePosInBuffer) {
  JitcodeGlobalEntry* entry = lookupInternal(ptr);
  if (!entry) {
    return nullptr;
  }

  entry->setSamplePositionInBuffer(samplePosInBuffer);

  // JitcodeGlobalEntries are marked at the end of the mark phase. A read
  // barrier is not needed. Any JS frames sampled during the sweep phase of
  // the GC must be on stack, and on-stack frames must already be marked at
  // the beginning of the sweep phase. It's not possible to assert this here
  // as we may be off main thread when called from the gecko profiler.

  return entry;
}

JitcodeGlobalEntry* JitcodeGlobalTable::lookupInternal(void* ptr) {
  JitcodeGlobalEntry query = JitcodeGlobalEntry::MakeQuery(ptr);
  JitcodeGlobalEntry* searchTower[JitcodeSkiplistTower::MAX_HEIGHT];
  searchInternal(query, searchTower);

  if (searchTower[0] == nullptr) {
    // Check startTower
    if (startTower_[0] == nullptr) {
      return nullptr;
    }

    MOZ_ASSERT(startTower_[0]->compareTo(query) >= 0);
    int cmp = startTower_[0]->compareTo(query);
    MOZ_ASSERT(cmp >= 0);
    return (cmp == 0) ? startTower_[0] : nullptr;
  }

  JitcodeGlobalEntry* bottom = searchTower[0];
  MOZ_ASSERT(bottom->compareTo(query) < 0);

  JitcodeGlobalEntry* bottomNext = bottom->tower_->next(0);
  if (bottomNext == nullptr) {
    return nullptr;
  }

  int cmp = bottomNext->compareTo(query);
  MOZ_ASSERT(cmp >= 0);
  return (cmp == 0) ? bottomNext : nullptr;
}

bool JitcodeGlobalTable::addEntry(const JitcodeGlobalEntry& entry) {
  MOZ_ASSERT(entry.isIon() || entry.isBaseline() ||
             entry.isBaselineInterpreter() || entry.isDummy());

  JitcodeGlobalEntry* searchTower[JitcodeSkiplistTower::MAX_HEIGHT];
  searchInternal(entry, searchTower);

  // Allocate a new entry and tower.
  JitcodeSkiplistTower* newTower = allocateTower(generateTowerHeight());
  if (!newTower) {
    return false;
  }

  JitcodeGlobalEntry* newEntry = allocateEntry();
  if (!newEntry) {
    return false;
  }

  *newEntry = entry;
  newEntry->tower_ = newTower;

  // Suppress profiler sampling while skiplist is being mutated.
  AutoSuppressProfilerSampling suppressSampling(TlsContext.get());

  // Link up entry with forward entries taken from tower.
  for (int level = newTower->height() - 1; level >= 0; level--) {
    JitcodeGlobalEntry* searchTowerEntry = searchTower[level];
    if (searchTowerEntry) {
      MOZ_ASSERT(searchTowerEntry->compareTo(*newEntry) < 0);
      JitcodeGlobalEntry* searchTowerNextEntry =
          searchTowerEntry->tower_->next(level);

      MOZ_ASSERT_IF(searchTowerNextEntry,
                    searchTowerNextEntry->compareTo(*newEntry) > 0);

      newTower->setNext(level, searchTowerNextEntry);
      searchTowerEntry->tower_->setNext(level, newEntry);
    } else {
      newTower->setNext(level, startTower_[level]);
      startTower_[level] = newEntry;
    }
  }
  skiplistSize_++;
  // verifySkiplist(); - disabled for release.

  return true;
}

void JitcodeGlobalTable::removeEntry(JitcodeGlobalEntry& entry,
                                     JitcodeGlobalEntry** prevTower) {
  MOZ_ASSERT(!TlsContext.get()->isProfilerSamplingEnabled());

  // Unlink query entry.
  for (int level = entry.tower_->height() - 1; level >= 0; level--) {
    JitcodeGlobalEntry* prevTowerEntry = prevTower[level];
    if (prevTowerEntry) {
      MOZ_ASSERT(prevTowerEntry->tower_->next(level) == &entry);
      prevTowerEntry->tower_->setNext(level, entry.tower_->next(level));
    } else {
      startTower_[level] = entry.tower_->next(level);
    }
  }
  skiplistSize_--;
  // verifySkiplist(); - disabled for release.

  // Entry has been unlinked.
  entry.destroy();
  entry.tower_->addToFreeList(&(freeTowers_[entry.tower_->height() - 1]));
  entry.tower_ = nullptr;
  entry = JitcodeGlobalEntry();
  entry.addToFreeList(&freeEntries_);
}

void JitcodeGlobalTable::releaseEntry(JitcodeGlobalEntry& entry,
                                      JitcodeGlobalEntry** prevTower,
                                      JSRuntime* rt) {
#ifdef DEBUG
  Maybe<uint64_t> rangeStart = rt->profilerSampleBufferRangeStart();
  MOZ_ASSERT_IF(rangeStart, !entry.isSampled(*rangeStart));
#endif
  removeEntry(entry, prevTower);
}

void JitcodeGlobalTable::searchInternal(const JitcodeGlobalEntry& query,
                                        JitcodeGlobalEntry** towerOut) {
  JitcodeGlobalEntry* cur = nullptr;
  for (int level = JitcodeSkiplistTower::MAX_HEIGHT - 1; level >= 0; level--) {
    JitcodeGlobalEntry* entry = searchAtHeight(level, cur, query);
    MOZ_ASSERT_IF(entry == nullptr, cur == nullptr);
    towerOut[level] = entry;
    cur = entry;
  }

  // Validate the resulting tower.
#ifdef DEBUG
  for (int level = JitcodeSkiplistTower::MAX_HEIGHT - 1; level >= 0; level--) {
    if (towerOut[level] == nullptr) {
      // If we got NULL for a given level, then we should have gotten NULL
      // for the level above as well.
      MOZ_ASSERT_IF(unsigned(level) < (JitcodeSkiplistTower::MAX_HEIGHT - 1),
                    towerOut[level + 1] == nullptr);
      continue;
    }

    JitcodeGlobalEntry* cur = towerOut[level];

    // Non-null result at a given level must sort < query.
    MOZ_ASSERT(cur->compareTo(query) < 0);

    // The entry must have a tower height that accomodates level.
    if (!cur->tower_->next(level)) {
      continue;
    }

    JitcodeGlobalEntry* next = cur->tower_->next(level);

    // Next entry must have tower height that accomodates level.
    MOZ_ASSERT(unsigned(level) < next->tower_->height());

    // Next entry must sort >= query.
    MOZ_ASSERT(next->compareTo(query) >= 0);
  }
#endif  // DEBUG
}

JitcodeGlobalEntry* JitcodeGlobalTable::searchAtHeight(
    unsigned level, JitcodeGlobalEntry* start,
    const JitcodeGlobalEntry& query) {
  JitcodeGlobalEntry* cur = start;

  // If starting with nullptr, use the start tower.
  if (start == nullptr) {
    cur = startTower_[level];
    if (cur == nullptr || cur->compareTo(query) >= 0) {
      return nullptr;
    }
  }

  // Keep skipping at |level| until we reach an entry < query whose
  // successor is an entry >= query.
  for (;;) {
    JitcodeGlobalEntry* next = cur->tower_->next(level);
    if (next == nullptr || next->compareTo(query) >= 0) {
      return cur;
    }

    cur = next;
  }
}

unsigned JitcodeGlobalTable::generateTowerHeight() {
  // Implementation taken from Hars L. and Pteruska G.,
  // "Pseudorandom Recursions: Small and fast Pseudorandom number generators for
  //  embedded applications."
  rand_ ^= mozilla::RotateLeft(rand_, 5) ^ mozilla::RotateLeft(rand_, 24);
  rand_ += 0x37798849;

  // Return 1 + number of lowbit zeros in new randval, capped at MAX_HEIGHT.
  unsigned result = 0;
  for (unsigned i = 0; i < JitcodeSkiplistTower::MAX_HEIGHT - 1; i++) {
    if ((rand_ >> i) & 0x1) {
      break;
    }
    result++;
  }
  return result + 1;
}

JitcodeSkiplistTower* JitcodeGlobalTable::allocateTower(unsigned height) {
  MOZ_ASSERT(height >= 1);
  JitcodeSkiplistTower* tower =
      JitcodeSkiplistTower::PopFromFreeList(&freeTowers_[height - 1]);
  if (tower) {
    return tower;
  }

  size_t size = JitcodeSkiplistTower::CalculateSize(height);
  tower = (JitcodeSkiplistTower*)alloc_.alloc(size);
  if (!tower) {
    return nullptr;
  }

  return new (tower) JitcodeSkiplistTower(height);
}

JitcodeGlobalEntry* JitcodeGlobalTable::allocateEntry() {
  JitcodeGlobalEntry* entry =
      JitcodeGlobalEntry::PopFromFreeList(&freeEntries_);
  if (entry) {
    return entry;
  }

  return alloc_.new_<JitcodeGlobalEntry>();
}

#ifdef DEBUG
void JitcodeGlobalTable::verifySkiplist() {
  JitcodeGlobalEntry* curTower[JitcodeSkiplistTower::MAX_HEIGHT];
  for (unsigned i = 0; i < JitcodeSkiplistTower::MAX_HEIGHT; i++) {
    curTower[i] = startTower_[i];
  }

  uint32_t count = 0;
  JitcodeGlobalEntry* curEntry = startTower_[0];
  while (curEntry) {
    count++;
    unsigned curHeight = curEntry->tower_->height();
    MOZ_ASSERT(curHeight >= 1);

    for (unsigned i = 0; i < JitcodeSkiplistTower::MAX_HEIGHT; i++) {
      if (i < curHeight) {
        MOZ_ASSERT(curTower[i] == curEntry);
        JitcodeGlobalEntry* nextEntry = curEntry->tower_->next(i);
        MOZ_ASSERT_IF(nextEntry, curEntry->compareTo(*nextEntry) < 0);
        curTower[i] = nextEntry;
      } else {
        MOZ_ASSERT_IF(curTower[i], curTower[i]->compareTo(*curEntry) > 0);
      }
    }
    curEntry = curEntry->tower_->next(0);
  }

  MOZ_ASSERT(count == skiplistSize_);
}
#endif  // DEBUG

void JitcodeGlobalTable::setAllEntriesAsExpired() {
  AutoSuppressProfilerSampling suppressSampling(TlsContext.get());
  for (Range r(*this); !r.empty(); r.popFront()) {
    auto entry = r.front();
    entry->setAsExpired();
  }
}

bool JitcodeGlobalTable::markIteratively(GCMarker* marker) {
  // JitcodeGlobalTable must keep entries that are in the sampler buffer
  // alive. This conditionality is akin to holding the entries weakly.
  //
  // If this table were marked at the beginning of the mark phase, then
  // sampling would require a read barrier for sampling in between
  // incremental GC slices. However, invoking read barriers from the sampler
  // is wildly unsafe. The sampler may run at any time, including during GC
  // itself.
  //
  // Instead, JitcodeGlobalTable is marked at the beginning of the sweep
  // phase, along with weak references. The key assumption is the
  // following. At the beginning of the sweep phase, any JS frames that the
  // sampler may put in its buffer that are not already there at the
  // beginning of the mark phase must have already been marked, as either 1)
  // the frame was on-stack at the beginning of the sweep phase, or 2) the
  // frame was pushed between incremental sweep slices. Frames of case 1)
  // are already marked. Frames of case 2) must have been reachable to have
  // been newly pushed, and thus are already marked.
  //
  // The approach above obviates the need for read barriers. The assumption
  // above is checked in JitcodeGlobalTable::lookupForSampler.

  MOZ_ASSERT(!JS::RuntimeHeapIsMinorCollecting());

  AutoSuppressProfilerSampling suppressSampling(TlsContext.get());

  // If the profiler is off, rangeStart will be Nothing() and all entries are
  // considered to be expired.
  Maybe<uint64_t> rangeStart =
      marker->runtime()->profilerSampleBufferRangeStart();

  bool markedAny = false;
  for (Range r(*this); !r.empty(); r.popFront()) {
    JitcodeGlobalEntry* entry = r.front();

    // If an entry is not sampled, reset its buffer position to the invalid
    // position, and conditionally mark the rest of the entry if its
    // JitCode is not already marked. This conditional marking ensures
    // that so long as the JitCode *may* be sampled, we keep any
    // information that may be handed out to the sampler, like tracked
    // types used by optimizations and scripts used for pc to line number
    // mapping, alive as well.
    if (!rangeStart || !entry->isSampled(*rangeStart)) {
      entry->setAsExpired();
      if (!entry->baseEntry().isJitcodeMarkedFromAnyThread(marker->runtime())) {
        continue;
      }
    }

    // The table is runtime-wide. Not all zones may be participating in
    // the GC.
    if (!entry->zone()->isCollecting() || entry->zone()->isGCFinished()) {
      continue;
    }

    markedAny |= entry->trace(marker);
  }

  return markedAny;
}

void JitcodeGlobalTable::traceWeak(JSRuntime* rt, JSTracer* trc) {
  AutoSuppressProfilerSampling suppressSampling(rt->mainContextFromOwnThread());
  for (Enum e(*this, rt); !e.empty(); e.popFront()) {
    JitcodeGlobalEntry* entry = e.front();

    if (!entry->zone()->isCollecting() || entry->zone()->isGCFinished()) {
      continue;
    }

    if (!TraceManuallyBarrieredWeakEdge(
            trc, &entry->baseEntry().jitcode_,
            "JitcodeGlobalTable::JitcodeGlobalEntry::jitcode_")) {
      e.removeFront();
    } else {
      entry->sweepChildren(rt);
    }
  }
}

bool JitcodeGlobalEntry::BaseEntry::traceJitcode(JSTracer* trc) {
  if (!IsMarkedUnbarriered(trc->runtime(), &jitcode_)) {
    TraceManuallyBarrieredEdge(trc, &jitcode_,
                               "jitcodglobaltable-baseentry-jitcode");
    return true;
  }
  return false;
}

bool JitcodeGlobalEntry::BaseEntry::isJitcodeMarkedFromAnyThread(
    JSRuntime* rt) {
  return IsMarkedUnbarriered(rt, &jitcode_);
}

bool JitcodeGlobalEntry::BaselineEntry::trace(JSTracer* trc) {
  if (!IsMarkedUnbarriered(trc->runtime(), &script_)) {
    TraceManuallyBarrieredEdge(trc, &script_,
                               "jitcodeglobaltable-baselineentry-script");
    return true;
  }
  return false;
}

void JitcodeGlobalEntry::BaselineEntry::sweepChildren() {
  MOZ_ALWAYS_FALSE(IsAboutToBeFinalizedUnbarriered(&script_));
}

bool JitcodeGlobalEntry::IonEntry::trace(JSTracer* trc) {
  bool tracedAny = false;

  JSRuntime* rt = trc->runtime();
  for (unsigned i = 0; i < numScripts(); i++) {
    if (!IsMarkedUnbarriered(rt, &sizedScriptList()->pairs[i].script)) {
      TraceManuallyBarrieredEdge(trc, &sizedScriptList()->pairs[i].script,
                                 "jitcodeglobaltable-ionentry-script");
      tracedAny = true;
    }
  }

  return tracedAny;
}

void JitcodeGlobalEntry::IonEntry::sweepChildren() {
  for (unsigned i = 0; i < numScripts(); i++) {
    MOZ_ALWAYS_FALSE(
        IsAboutToBeFinalizedUnbarriered(&sizedScriptList()->pairs[i].script));
  }
}

/* static */
void JitcodeRegionEntry::WriteHead(CompactBufferWriter& writer,
                                   uint32_t nativeOffset, uint8_t scriptDepth) {
  writer.writeUnsigned(nativeOffset);
  writer.writeByte(scriptDepth);
}

/* static */
void JitcodeRegionEntry::ReadHead(CompactBufferReader& reader,
                                  uint32_t* nativeOffset,
                                  uint8_t* scriptDepth) {
  *nativeOffset = reader.readUnsigned();
  *scriptDepth = reader.readByte();
}

/* static */
void JitcodeRegionEntry::WriteScriptPc(CompactBufferWriter& writer,
                                       uint32_t scriptIdx, uint32_t pcOffset) {
  writer.writeUnsigned(scriptIdx);
  writer.writeUnsigned(pcOffset);
}

/* static */
void JitcodeRegionEntry::ReadScriptPc(CompactBufferReader& reader,
                                      uint32_t* scriptIdx, uint32_t* pcOffset) {
  *scriptIdx = reader.readUnsigned();
  *pcOffset = reader.readUnsigned();
}

/* static */
void JitcodeRegionEntry::WriteDelta(CompactBufferWriter& writer,
                                    uint32_t nativeDelta, int32_t pcDelta) {
  if (pcDelta >= 0) {
    // 1 and 2-byte formats possible.

    //  NNNN-BBB0
    if (pcDelta <= ENC1_PC_DELTA_MAX && nativeDelta <= ENC1_NATIVE_DELTA_MAX) {
      uint8_t encVal = ENC1_MASK_VAL | (pcDelta << ENC1_PC_DELTA_SHIFT) |
                       (nativeDelta << ENC1_NATIVE_DELTA_SHIFT);
      writer.writeByte(encVal);
      return;
    }

    //  NNNN-NNNN BBBB-BB01
    if (pcDelta <= ENC2_PC_DELTA_MAX && nativeDelta <= ENC2_NATIVE_DELTA_MAX) {
      uint16_t encVal = ENC2_MASK_VAL | (pcDelta << ENC2_PC_DELTA_SHIFT) |
                        (nativeDelta << ENC2_NATIVE_DELTA_SHIFT);
      writer.writeByte(encVal & 0xff);
      writer.writeByte((encVal >> 8) & 0xff);
      return;
    }
  }

  //  NNNN-NNNN NNNB-BBBB BBBB-B011
  if (pcDelta >= ENC3_PC_DELTA_MIN && pcDelta <= ENC3_PC_DELTA_MAX &&
      nativeDelta <= ENC3_NATIVE_DELTA_MAX) {
    uint32_t encVal =
        ENC3_MASK_VAL |
        ((uint32_t(pcDelta) << ENC3_PC_DELTA_SHIFT) & ENC3_PC_DELTA_MASK) |
        (nativeDelta << ENC3_NATIVE_DELTA_SHIFT);
    writer.writeByte(encVal & 0xff);
    writer.writeByte((encVal >> 8) & 0xff);
    writer.writeByte((encVal >> 16) & 0xff);
    return;
  }

  //  NNNN-NNNN NNNN-NNNN BBBB-BBBB BBBB-B111
  if (pcDelta >= ENC4_PC_DELTA_MIN && pcDelta <= ENC4_PC_DELTA_MAX &&
      nativeDelta <= ENC4_NATIVE_DELTA_MAX) {
    uint32_t encVal =
        ENC4_MASK_VAL |
        ((uint32_t(pcDelta) << ENC4_PC_DELTA_SHIFT) & ENC4_PC_DELTA_MASK) |
        (nativeDelta << ENC4_NATIVE_DELTA_SHIFT);
    writer.writeByte(encVal & 0xff);
    writer.writeByte((encVal >> 8) & 0xff);
    writer.writeByte((encVal >> 16) & 0xff);
    writer.writeByte((encVal >> 24) & 0xff);
    return;
  }

  // Should never get here.
  MOZ_CRASH("pcDelta/nativeDelta values are too large to encode.");
}

/* static */
void JitcodeRegionEntry::ReadDelta(CompactBufferReader& reader,
                                   uint32_t* nativeDelta, int32_t* pcDelta) {
  // NB:
  // It's possible to get nativeDeltas with value 0 in two cases:
  //
  // 1. The last region's run.  This is because the region table's start
  // must be 4-byte aligned, and we must insert padding bytes to align the
  // payload section before emitting the table.
  //
  // 2. A zero-offset nativeDelta with a negative pcDelta.
  //
  // So if nativeDelta is zero, then pcDelta must be <= 0.

  //  NNNN-BBB0
  const uint32_t firstByte = reader.readByte();
  if ((firstByte & ENC1_MASK) == ENC1_MASK_VAL) {
    uint32_t encVal = firstByte;
    *nativeDelta = encVal >> ENC1_NATIVE_DELTA_SHIFT;
    *pcDelta = (encVal & ENC1_PC_DELTA_MASK) >> ENC1_PC_DELTA_SHIFT;
    MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
    return;
  }

  //  NNNN-NNNN BBBB-BB01
  const uint32_t secondByte = reader.readByte();
  if ((firstByte & ENC2_MASK) == ENC2_MASK_VAL) {
    uint32_t encVal = firstByte | secondByte << 8;
    *nativeDelta = encVal >> ENC2_NATIVE_DELTA_SHIFT;
    *pcDelta = (encVal & ENC2_PC_DELTA_MASK) >> ENC2_PC_DELTA_SHIFT;
    MOZ_ASSERT(*pcDelta != 0);
    MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
    return;
  }

  //  NNNN-NNNN NNNB-BBBB BBBB-B011
  const uint32_t thirdByte = reader.readByte();
  if ((firstByte & ENC3_MASK) == ENC3_MASK_VAL) {
    uint32_t encVal = firstByte | secondByte << 8 | thirdByte << 16;
    *nativeDelta = encVal >> ENC3_NATIVE_DELTA_SHIFT;

    uint32_t pcDeltaU = (encVal & ENC3_PC_DELTA_MASK) >> ENC3_PC_DELTA_SHIFT;
    // Fix sign if necessary.
    if (pcDeltaU > static_cast<uint32_t>(ENC3_PC_DELTA_MAX)) {
      pcDeltaU |= ~ENC3_PC_DELTA_MAX;
    }
    *pcDelta = pcDeltaU;
    MOZ_ASSERT(*pcDelta != 0);
    MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
    return;
  }

  //  NNNN-NNNN NNNN-NNNN BBBB-BBBB BBBB-B111
  MOZ_ASSERT((firstByte & ENC4_MASK) == ENC4_MASK_VAL);
  const uint32_t fourthByte = reader.readByte();
  uint32_t encVal =
      firstByte | secondByte << 8 | thirdByte << 16 | fourthByte << 24;
  *nativeDelta = encVal >> ENC4_NATIVE_DELTA_SHIFT;

  uint32_t pcDeltaU = (encVal & ENC4_PC_DELTA_MASK) >> ENC4_PC_DELTA_SHIFT;
  // fix sign if necessary
  if (pcDeltaU > static_cast<uint32_t>(ENC4_PC_DELTA_MAX)) {
    pcDeltaU |= ~ENC4_PC_DELTA_MAX;
  }
  *pcDelta = pcDeltaU;

  MOZ_ASSERT(*pcDelta != 0);
  MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
}

/* static */
uint32_t JitcodeRegionEntry::ExpectedRunLength(const NativeToBytecode* entry,
                                               const NativeToBytecode* end) {
  MOZ_ASSERT(entry < end);

  // We always use the first entry, so runLength starts at 1
  uint32_t runLength = 1;

  uint32_t curNativeOffset = entry->nativeOffset.offset();
  uint32_t curBytecodeOffset = entry->tree->script()->pcToOffset(entry->pc);

  for (auto nextEntry = entry + 1; nextEntry != end; nextEntry += 1) {
    // If the next run moves to a different inline site, stop the run.
    if (nextEntry->tree != entry->tree) {
      break;
    }

    uint32_t nextNativeOffset = nextEntry->nativeOffset.offset();
    uint32_t nextBytecodeOffset =
        nextEntry->tree->script()->pcToOffset(nextEntry->pc);
    MOZ_ASSERT(nextNativeOffset >= curNativeOffset);

    uint32_t nativeDelta = nextNativeOffset - curNativeOffset;
    int32_t bytecodeDelta =
        int32_t(nextBytecodeOffset) - int32_t(curBytecodeOffset);

    // If deltas are too large (very unlikely), stop the run.
    if (!IsDeltaEncodeable(nativeDelta, bytecodeDelta)) {
      break;
    }

    runLength++;

    // If the run has grown to its maximum length, stop the run.
    if (runLength == MAX_RUN_LENGTH) {
      break;
    }

    curNativeOffset = nextNativeOffset;
    curBytecodeOffset = nextBytecodeOffset;
  }

  return runLength;
}

struct JitcodeMapBufferWriteSpewer {
#ifdef JS_JITSPEW
  CompactBufferWriter* writer;
  uint32_t startPos;

  static const uint32_t DumpMaxBytes = 50;

  explicit JitcodeMapBufferWriteSpewer(CompactBufferWriter& w)
      : writer(&w), startPos(writer->length()) {}

  void spewAndAdvance(const char* name) {
    if (writer->oom()) {
      return;
    }

    uint32_t curPos = writer->length();
    const uint8_t* start = writer->buffer() + startPos;
    const uint8_t* end = writer->buffer() + curPos;
    const char* MAP = "0123456789ABCDEF";
    uint32_t bytes = end - start;

    char buffer[DumpMaxBytes * 3];
    for (uint32_t i = 0; i < bytes; i++) {
      buffer[i * 3] = MAP[(start[i] >> 4) & 0xf];
      buffer[i * 3 + 1] = MAP[(start[i] >> 0) & 0xf];
      buffer[i * 3 + 2] = ' ';
    }
    if (bytes >= DumpMaxBytes) {
      buffer[DumpMaxBytes * 3 - 1] = '\0';
    } else {
      buffer[bytes * 3 - 1] = '\0';
    }

    JitSpew(JitSpew_Profiling, "%s@%d[%d bytes] - %s", name, int(startPos),
            int(bytes), buffer);

    // Move to the end of the current buffer.
    startPos = writer->length();
  }
#else   // !JS_JITSPEW
  explicit JitcodeMapBufferWriteSpewer(CompactBufferWriter& w) {}
  void spewAndAdvance(const char* name) {}
#endif  // JS_JITSPEW
};

// Write a run, starting at the given NativeToBytecode entry, into the given
// buffer writer.
/* static */
bool JitcodeRegionEntry::WriteRun(CompactBufferWriter& writer,
                                  JSScript** scriptList,
                                  uint32_t scriptListSize, uint32_t runLength,
                                  const NativeToBytecode* entry) {
  MOZ_ASSERT(runLength > 0);
  MOZ_ASSERT(runLength <= MAX_RUN_LENGTH);

  // Calculate script depth.
  MOZ_ASSERT(entry->tree->depth() <= 0xff);
  uint8_t scriptDepth = entry->tree->depth();
  uint32_t regionNativeOffset = entry->nativeOffset.offset();

  JitcodeMapBufferWriteSpewer spewer(writer);

  // Write the head info.
  JitSpew(JitSpew_Profiling, "    Head Info: nativeOffset=%d scriptDepth=%d",
          int(regionNativeOffset), int(scriptDepth));
  WriteHead(writer, regionNativeOffset, scriptDepth);
  spewer.spewAndAdvance("      ");

  // Write each script/pc pair.
  {
    InlineScriptTree* curTree = entry->tree;
    jsbytecode* curPc = entry->pc;
    for (uint8_t i = 0; i < scriptDepth; i++) {
      // Find the index of the script within the list.
      // NB: scriptList is guaranteed to contain curTree->script()
      uint32_t scriptIdx = 0;
      for (; scriptIdx < scriptListSize; scriptIdx++) {
        if (scriptList[scriptIdx] == curTree->script()) {
          break;
        }
      }
      MOZ_ASSERT(scriptIdx < scriptListSize);

      uint32_t pcOffset = curTree->script()->pcToOffset(curPc);

      JitSpew(JitSpew_Profiling, "    Script/PC %d: scriptIdx=%d pcOffset=%d",
              int(i), int(scriptIdx), int(pcOffset));
      WriteScriptPc(writer, scriptIdx, pcOffset);
      spewer.spewAndAdvance("      ");

      MOZ_ASSERT_IF(i < scriptDepth - 1, curTree->hasCaller());
      curPc = curTree->callerPc();
      curTree = curTree->caller();
    }
  }

  // Start writing runs.
  uint32_t curNativeOffset = entry->nativeOffset.offset();
  uint32_t curBytecodeOffset = entry->tree->script()->pcToOffset(entry->pc);

  JitSpew(JitSpew_Profiling,
          "  Writing Delta Run from nativeOffset=%d bytecodeOffset=%d",
          int(curNativeOffset), int(curBytecodeOffset));

  // Skip first entry because it is implicit in the header.  Start at subsequent
  // entry.
  for (uint32_t i = 1; i < runLength; i++) {
    MOZ_ASSERT(entry[i].tree == entry->tree);

    uint32_t nextNativeOffset = entry[i].nativeOffset.offset();
    uint32_t nextBytecodeOffset =
        entry[i].tree->script()->pcToOffset(entry[i].pc);
    MOZ_ASSERT(nextNativeOffset >= curNativeOffset);

    uint32_t nativeDelta = nextNativeOffset - curNativeOffset;
    int32_t bytecodeDelta =
        int32_t(nextBytecodeOffset) - int32_t(curBytecodeOffset);
    MOZ_ASSERT(IsDeltaEncodeable(nativeDelta, bytecodeDelta));

    JitSpew(JitSpew_Profiling,
            "    RunEntry native: %d-%d [%d]  bytecode: %d-%d [%d]",
            int(curNativeOffset), int(nextNativeOffset), int(nativeDelta),
            int(curBytecodeOffset), int(nextBytecodeOffset),
            int(bytecodeDelta));
    WriteDelta(writer, nativeDelta, bytecodeDelta);

    // Spew the bytecode in these ranges.
    if (curBytecodeOffset < nextBytecodeOffset) {
      JitSpewStart(JitSpew_Profiling, "      OPS: ");
      uint32_t curBc = curBytecodeOffset;
      while (curBc < nextBytecodeOffset) {
        jsbytecode* pc = entry[i].tree->script()->offsetToPC(curBc);
#ifdef JS_JITSPEW
        JSOp op = JSOp(*pc);
        JitSpewCont(JitSpew_Profiling, "%s ", CodeName(op));
#endif
        curBc += GetBytecodeLength(pc);
      }
      JitSpewFin(JitSpew_Profiling);
    }
    spewer.spewAndAdvance("      ");

    curNativeOffset = nextNativeOffset;
    curBytecodeOffset = nextBytecodeOffset;
  }

  if (writer.oom()) {
    return false;
  }

  return true;
}

void JitcodeRegionEntry::unpack() {
  CompactBufferReader reader(data_, end_);
  ReadHead(reader, &nativeOffset_, &scriptDepth_);
  MOZ_ASSERT(scriptDepth_ > 0);

  scriptPcStack_ = reader.currentPosition();
  // Skip past script/pc stack
  for (unsigned i = 0; i < scriptDepth_; i++) {
    uint32_t scriptIdx, pcOffset;
    ReadScriptPc(reader, &scriptIdx, &pcOffset);
  }

  deltaRun_ = reader.currentPosition();
}

uint32_t JitcodeRegionEntry::findPcOffset(uint32_t queryNativeOffset,
                                          uint32_t startPcOffset) const {
  DeltaIterator iter = deltaIterator();
  uint32_t curNativeOffset = nativeOffset();
  uint32_t curPcOffset = startPcOffset;
  while (iter.hasMore()) {
    uint32_t nativeDelta;
    int32_t pcDelta;
    iter.readNext(&nativeDelta, &pcDelta);

    // The start address of the next delta-run entry is counted towards
    // the current delta-run entry, because return addresses should
    // associate with the bytecode op prior (the call) not the op after.
    if (queryNativeOffset <= curNativeOffset + nativeDelta) {
      break;
    }
    curNativeOffset += nativeDelta;
    curPcOffset += pcDelta;
  }
  return curPcOffset;
}

bool JitcodeIonTable::makeIonEntry(JSContext* cx, JitCode* code,
                                   uint32_t numScripts, JSScript** scripts,
                                   JitcodeGlobalEntry::IonEntry& out) {
  using SizedScriptList = JitcodeGlobalEntry::IonEntry::SizedScriptList;

  MOZ_ASSERT(numScripts > 0);

  // Create profiling strings for script, within vector.
  typedef js::Vector<char*, 32, SystemAllocPolicy> ProfilingStringVector;

  ProfilingStringVector profilingStrings;
  if (!profilingStrings.reserve(numScripts)) {
    return false;
  }

  // Cleanup allocations on failure.
  auto autoFreeProfilingStrings = mozilla::MakeScopeExit([&] {
    for (auto elem : profilingStrings) {
      js_free(elem);
    }
  });

  for (uint32_t i = 0; i < numScripts; i++) {
    UniqueChars str = GeckoProfilerRuntime::allocProfileString(cx, scripts[i]);
    if (!str) {
      return false;
    }
    if (!profilingStrings.append(str.release())) {
      return false;
    }
  }

  // Create SizedScriptList
  void* mem =
      (void*)cx->pod_malloc<uint8_t>(SizedScriptList::AllocSizeFor(numScripts));
  if (!mem) {
    return false;
  }

  // Keep allocated profiling strings.
  autoFreeProfilingStrings.release();

  SizedScriptList* scriptList =
      new (mem) SizedScriptList(numScripts, scripts, &profilingStrings[0]);
  out.init(code, code->raw(), code->rawEnd(), scriptList, this);
  return true;
}

uint32_t JitcodeIonTable::findRegionEntry(uint32_t nativeOffset) const {
  static const uint32_t LINEAR_SEARCH_THRESHOLD = 8;
  uint32_t regions = numRegions();
  MOZ_ASSERT(regions > 0);

  // For small region lists, just search linearly.
  if (regions <= LINEAR_SEARCH_THRESHOLD) {
    JitcodeRegionEntry previousEntry = regionEntry(0);
    for (uint32_t i = 1; i < regions; i++) {
      JitcodeRegionEntry nextEntry = regionEntry(i);
      MOZ_ASSERT(nextEntry.nativeOffset() >= previousEntry.nativeOffset());

      // See note in binary-search code below about why we use '<=' here
      // instead of '<'.  Short explanation: regions are closed at their
      // ending addresses, and open at their starting addresses.
      if (nativeOffset <= nextEntry.nativeOffset()) {
        return i - 1;
      }

      previousEntry = nextEntry;
    }
    // If nothing found, assume it falls within last region.
    return regions - 1;
  }

  // For larger ones, binary search the region table.
  uint32_t idx = 0;
  uint32_t count = regions;
  while (count > 1) {
    uint32_t step = count / 2;
    uint32_t mid = idx + step;
    JitcodeRegionEntry midEntry = regionEntry(mid);

    // A region memory range is closed at its ending address, not starting
    // address.  This is because the return address for calls must associate
    // with the call's bytecode PC, not the PC of the bytecode operator after
    // the call.
    //
    // So a query is < an entry if the query nativeOffset is <= the start
    // address of the entry, and a query is >= an entry if the query
    // nativeOffset is > the start address of an entry.
    if (nativeOffset <= midEntry.nativeOffset()) {
      // Target entry is below midEntry.
      count = step;
    } else {  // if (nativeOffset > midEntry.nativeOffset())
      // Target entry is at midEntry or above.
      idx = mid;
      count -= step;
    }
  }
  return idx;
}

/* static */
bool JitcodeIonTable::WriteIonTable(
    CompactBufferWriter& writer, JSScript** scriptList, uint32_t scriptListSize,
    const NativeToBytecode* start, const NativeToBytecode* end,
    uint32_t* tableOffsetOut, uint32_t* numRegionsOut) {
  MOZ_ASSERT(tableOffsetOut != nullptr);
  MOZ_ASSERT(numRegionsOut != nullptr);
  MOZ_ASSERT(writer.length() == 0);
  MOZ_ASSERT(scriptListSize > 0);

  JitSpew(JitSpew_Profiling,
          "Writing native to bytecode map for %s:%u:%u (%zu entries)",
          scriptList[0]->filename(), scriptList[0]->lineno(),
          scriptList[0]->column(), mozilla::PointerRangeSize(start, end));

  JitSpew(JitSpew_Profiling, "  ScriptList of size %d", int(scriptListSize));
  for (uint32_t i = 0; i < scriptListSize; i++) {
    JitSpew(JitSpew_Profiling, "  Script %d - %s:%u:%u", int(i),
            scriptList[i]->filename(), scriptList[i]->lineno(),
            scriptList[i]->column());
  }

  // Write out runs first.  Keep a vector tracking the positive offsets from
  // payload start to the run.
  const NativeToBytecode* curEntry = start;
  js::Vector<uint32_t, 32, SystemAllocPolicy> runOffsets;

  while (curEntry != end) {
    // Calculate the length of the next run.
    uint32_t runLength = JitcodeRegionEntry::ExpectedRunLength(curEntry, end);
    MOZ_ASSERT(runLength > 0);
    MOZ_ASSERT(runLength <= uintptr_t(end - curEntry));
    JitSpew(JitSpew_Profiling, "  Run at entry %d, length %d, buffer offset %d",
            int(curEntry - start), int(runLength), int(writer.length()));

    // Store the offset of the run.
    if (!runOffsets.append(writer.length())) {
      return false;
    }

    // Encode the run.
    if (!JitcodeRegionEntry::WriteRun(writer, scriptList, scriptListSize,
                                      runLength, curEntry)) {
      return false;
    }

    curEntry += runLength;
  }

  // Done encoding regions.  About to start table.  Ensure we are aligned to 4
  // bytes since table is composed of uint32_t values.
  uint32_t padding = sizeof(uint32_t) - (writer.length() % sizeof(uint32_t));
  if (padding == sizeof(uint32_t)) {
    padding = 0;
  }
  JitSpew(JitSpew_Profiling, "  Padding %d bytes after run @%d", int(padding),
          int(writer.length()));
  for (uint32_t i = 0; i < padding; i++) {
    writer.writeByte(0);
  }

  // Now at start of table.
  uint32_t tableOffset = writer.length();

  // The table being written at this point will be accessed directly via
  // uint32_t pointers, so all writes below use native endianness.

  // Write out numRegions
  JitSpew(JitSpew_Profiling, "  Writing numRuns=%d", int(runOffsets.length()));
  writer.writeNativeEndianUint32_t(runOffsets.length());

  // Write out region offset table.  The offsets in |runOffsets| are currently
  // forward offsets from the beginning of the buffer.  We convert them to
  // backwards offsets from the start of the table before writing them into
  // their table entries.
  for (uint32_t i = 0; i < runOffsets.length(); i++) {
    JitSpew(JitSpew_Profiling, "  Run %d offset=%d backOffset=%d @%d", int(i),
            int(runOffsets[i]), int(tableOffset - runOffsets[i]),
            int(writer.length()));
    writer.writeNativeEndianUint32_t(tableOffset - runOffsets[i]);
  }

  if (writer.oom()) {
    return false;
  }

  *tableOffsetOut = tableOffset;
  *numRegionsOut = runOffsets.length();
  return true;
}

}  // namespace jit
}  // namespace js

JS::ProfiledFrameHandle::ProfiledFrameHandle(JSRuntime* rt,
                                             js::jit::JitcodeGlobalEntry& entry,
                                             void* addr, const char* label,
                                             uint32_t depth)
    : rt_(rt),
      entry_(entry),
      addr_(addr),
      canonicalAddr_(nullptr),
      label_(label),
      depth_(depth) {
  if (!canonicalAddr_) {
    canonicalAddr_ = entry_.canonicalNativeAddrFor(rt_, addr_);
  }
}

JS_PUBLIC_API JS::ProfilingFrameIterator::FrameKind
JS::ProfiledFrameHandle::frameKind() const {
  if (entry_.isBaselineInterpreter()) {
    return JS::ProfilingFrameIterator::Frame_BaselineInterpreter;
  }
  if (entry_.isBaseline()) {
    return JS::ProfilingFrameIterator::Frame_Baseline;
  }
  return JS::ProfilingFrameIterator::Frame_Ion;
}

JS_PUBLIC_API uint64_t JS::ProfiledFrameHandle::realmID() const {
  return entry_.lookupRealmID(rt_, addr_);
}

JS_PUBLIC_API JS::ProfiledFrameRange JS::GetProfiledFrames(JSContext* cx,
                                                           void* addr) {
  JSRuntime* rt = cx->runtime();
  js::jit::JitcodeGlobalTable* table =
      rt->jitRuntime()->getJitcodeGlobalTable();
  js::jit::JitcodeGlobalEntry* entry = table->lookup(addr);

  ProfiledFrameRange result(rt, addr, entry);

  if (entry) {
    result.depth_ = entry->callStackAtAddr(rt, addr, result.labels_,
                                           MOZ_ARRAY_LENGTH(result.labels_));
  }
  return result;
}

JS::ProfiledFrameHandle JS::ProfiledFrameRange::Iter::operator*() const {
  // The iterator iterates in high depth to low depth order. index_ goes up,
  // and the depth we need to pass to ProfiledFrameHandle goes down.
  uint32_t depth = range_.depth_ - 1 - index_;
  return ProfiledFrameHandle(range_.rt_, *range_.entry_, range_.addr_,
                             range_.labels_[depth], depth);
}
