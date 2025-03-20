/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonScript_h
#define jit_IonScript_h

#include "mozilla/MemoryReporting.h"  // MallocSizeOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"

#include "gc/Barrier.h"          // HeapPtr{JitCode,Object}
#include "jit/IonTypes.h"        // IonCompilationId
#include "jit/JitCode.h"         // JitCode
#include "jit/JitOptions.h"      // JitOptions
#include "js/TypeDecls.h"        // jsbytecode
#include "util/TrailingArray.h"  // TrailingArray

namespace js {
namespace jit {

class SnapshotWriter;
class RecoverWriter;
class SafepointWriter;
class CodegenSafepointIndex;
class SafepointIndex;
class OsiIndex;
class IonIC;

// An IonScript attaches Ion-generated information to a JSScript. The header
// structure is followed by several arrays of data. These trailing arrays have a
// layout based on offsets (bytes from 'this') stored in the IonScript header.
//
//    <IonScript itself>
//    --
//    PreBarriered<Value>[] constantTable()
//    uint8_t[]             runtimeData()
//    OsiIndex[]            osiIndex()
//    SafepointIndex[]      safepointIndex()
//    uint32_t[]            icIndex()
//    --
//    uint8_t[]             safepoints()
//    uint8_t[]             snapshots()
//    uint8_t[]             snapshotsRVATable()
//    uint8_t[]             recovers()
//
// Note: These are arranged in order of descending alignment requirements to
// avoid the need for padding. The `runtimeData` uses uint64_t alignement due to
// its use of mozilla::AlignedStorage2.
class alignas(8) IonScript final : public TrailingArray<IonScript> {
 private:
  // Offset (in bytes) from `this` to the start of each trailing array. Each
  // array ends where following one begins. There is no implicit padding (except
  // possible at very end).
  Offset constantTableOffset_ = 0;   // JS::Value aligned
  Offset runtimeDataOffset_ = 0;     // uint64_t aligned
  Offset nurseryObjectsOffset_ = 0;  // pointer aligned
  Offset osiIndexOffset_ = 0;
  Offset safepointIndexOffset_ = 0;
  Offset icIndexOffset_ = 0;
  Offset safepointsOffset_ = 0;
  Offset snapshotsOffset_ = 0;
  Offset rvaTableOffset_ = 0;
  Offset recoversOffset_ = 0;
  Offset allocBytes_ = 0;

  // Code pointer containing the actual method.
  HeapPtr<JitCode*> method_ = nullptr;

  // Entrypoint for OSR, or nullptr.
  jsbytecode* osrPc_ = nullptr;

  // Offset to OSR entrypoint from method_->raw(), or 0.
  uint32_t osrEntryOffset_ = 0;

  // Offset of the invalidation epilogue (which pushes this IonScript
  // and calls the invalidation thunk).
  uint32_t invalidateEpilogueOffset_ = 0;

  // The offset immediately after the IonScript immediate.
  // NOTE: technically a constant delta from
  // |invalidateEpilogueOffset_|, so we could hard-code this
  // per-platform if we want.
  uint32_t invalidateEpilogueDataOffset_ = 0;

  // Number of bailouts that have occurred for reasons that could be
  // fixed if we invalidated and recompiled.
  uint16_t numFixableBailouts_ = 0;

  // Number of bailouts that have occurred for reasons that can't be
  // fixed by recompiling: for example, bailing out to catch an exception.
  uint16_t numUnfixableBailouts_ = 0;

 public:
  enum class LICMState : uint8_t { NeverBailed, Bailed, BailedAndHitFallback };

 private:
  // Tracks the state of LICM bailouts.
  LICMState licmState_ = LICMState::NeverBailed;

  // Flag set if IonScript was compiled with profiling enabled.
  bool hasProfilingInstrumentation_ = false;

  // If true, this IonScript was active on the stack when we discarded JIT code
  // and inactive ICScripts. This means we should use the generic ICScripts for
  // inlined functions when we bail out.
  bool purgedICScripts_ = false;

  // Number of bytes this function reserves on the stack for slots spilled by
  // the register allocator.
  uint32_t localSlotsSize_ = 0;

  // Number of bytes used passed in as formal arguments or |this|.
  uint32_t argumentSlotsSize_ = 0;

  // Frame size is the value that can be added to the StackPointer along
  // with the frame prefix to get a valid JitFrameLayout.
  uint32_t frameSize_ = 0;

  // Number of references from invalidation records.
  uint32_t invalidationCount_ = 0;

  // Identifier of the compilation which produced this code.
  IonCompilationId compilationId_;

  // Number of times we tried to enter this script via OSR but failed due to
  // a LOOPENTRY pc other than osrPc_.
  uint32_t osrPcMismatchCounter_ = 0;

#ifdef DEBUG
  // A hash of the ICScripts used in this compilation.
  mozilla::HashNumber icHash_ = 0;
#endif

  // End of fields.

 private:
  // Layout helpers
  Offset constantTableOffset() const { return constantTableOffset_; }
  Offset runtimeDataOffset() const { return runtimeDataOffset_; }
  Offset nurseryObjectsOffset() const { return nurseryObjectsOffset_; }
  Offset osiIndexOffset() const { return osiIndexOffset_; }
  Offset safepointIndexOffset() const { return safepointIndexOffset_; }
  Offset icIndexOffset() const { return icIndexOffset_; }
  Offset safepointsOffset() const { return safepointsOffset_; }
  Offset snapshotsOffset() const { return snapshotsOffset_; }
  Offset rvaTableOffset() const { return rvaTableOffset_; }
  Offset recoversOffset() const { return recoversOffset_; }
  Offset endOffset() const { return allocBytes_; }

  // Hardcode size of incomplete types. These are verified in Ion.cpp.
  static constexpr size_t SizeOf_OsiIndex = 2 * sizeof(uint32_t);
  static constexpr size_t SizeOf_SafepointIndex = 2 * sizeof(uint32_t);

 public:
  //
  // Table of constants referenced in snapshots. (JS::Value alignment)
  //
  PreBarriered<Value>* constants() {
    // Nursery constants are manually barriered in CodeGenerator::link() so a
    // post barrier is not required..
    return offsetToPointer<PreBarriered<Value>>(constantTableOffset());
  }
  size_t numConstants() const {
    return numElements<PreBarriered<Value>>(constantTableOffset(),
                                            runtimeDataOffset());
  }

  //
  // IonIC data structures. (uint64_t alignment)
  //
  uint8_t* runtimeData() {
    return offsetToPointer<uint8_t>(runtimeDataOffset());
  }
  size_t runtimeSize() const {
    return numElements<uint8_t>(runtimeDataOffset(), nurseryObjectsOffset());
  }

  //
  // List of (originally) nursery-allocated objects referenced from JIT code.
  // (JSObject* alignment)
  //
  HeapPtr<JSObject*>* nurseryObjects() {
    return offsetToPointer<HeapPtr<JSObject*>>(nurseryObjectsOffset());
  }
  size_t numNurseryObjects() const {
    return numElements<HeapPtr<JSObject*>>(nurseryObjectsOffset(),
                                           osiIndexOffset());
  }
  void* addressOfNurseryObject(uint32_t index) {
    MOZ_ASSERT(index < numNurseryObjects());
    return &nurseryObjects()[index];
  }

  //
  // Map OSI-point displacement to snapshot.
  //
  OsiIndex* osiIndices() { return offsetToPointer<OsiIndex>(osiIndexOffset()); }
  const OsiIndex* osiIndices() const {
    return offsetToPointer<OsiIndex>(osiIndexOffset());
  }
  size_t numOsiIndices() const {
    return numElements<SizeOf_OsiIndex>(osiIndexOffset(),
                                        safepointIndexOffset());
  }

  //
  // Map code displacement to safepoint / OSI-patch-delta.
  //
  SafepointIndex* safepointIndices() {
    return offsetToPointer<SafepointIndex>(safepointIndexOffset());
  }
  const SafepointIndex* safepointIndices() const {
    return offsetToPointer<SafepointIndex>(safepointIndexOffset());
  }
  size_t numSafepointIndices() const {
    return numElements<SizeOf_SafepointIndex>(safepointIndexOffset(),
                                              icIndexOffset());
  }

  //
  // Offset into `runtimeData` for each (variable-length) IonIC.
  //
  uint32_t* icIndex() { return offsetToPointer<uint32_t>(icIndexOffset()); }
  size_t numICs() const {
    return numElements<uint32_t>(icIndexOffset(), safepointsOffset());
  }

  //
  // Safepoint table as a CompactBuffer.
  //
  const uint8_t* safepoints() const {
    return offsetToPointer<uint8_t>(safepointsOffset());
  }
  size_t safepointsSize() const {
    return numElements<uint8_t>(safepointsOffset(), snapshotsOffset());
  }

  //
  // Snapshot and RValueAllocation tables as CompactBuffers.
  //
  const uint8_t* snapshots() const {
    return offsetToPointer<uint8_t>(snapshotsOffset());
  }
  size_t snapshotsListSize() const {
    return numElements<uint8_t>(snapshotsOffset(), rvaTableOffset());
  }
  size_t snapshotsRVATableSize() const {
    return numElements<uint8_t>(rvaTableOffset(), recoversOffset());
  }

  //
  // Recover instruction table as a CompactBuffer.
  //
  const uint8_t* recovers() const {
    return offsetToPointer<uint8_t>(recoversOffset());
  }
  size_t recoversSize() const {
    return numElements<uint8_t>(recoversOffset(), endOffset());
  }

 private:
  IonScript(IonCompilationId compilationId, uint32_t localSlotsSize,
            uint32_t argumentSlotsSize, uint32_t frameSize);

 public:
  static IonScript* New(JSContext* cx, IonCompilationId compilationId,
                        uint32_t localSlotsSize, uint32_t argumentSlotsSize,
                        uint32_t frameSize, size_t snapshotsListSize,
                        size_t snapshotsRVATableSize, size_t recoversSize,
                        size_t constants, size_t nurseryObjects,
                        size_t safepointIndices, size_t osiIndices,
                        size_t icEntries, size_t runtimeSize,
                        size_t safepointsSize);

  static void Destroy(JS::GCContext* gcx, IonScript* script);

  void trace(JSTracer* trc);
  void traceWeak(JSTracer* trc);

  static inline size_t offsetOfInvalidationCount() {
    return offsetof(IonScript, invalidationCount_);
  }

 public:
  JitCode* method() const { return method_; }
  void setMethod(JitCode* code) {
    MOZ_ASSERT(!invalidated());
    method_ = code;
  }
  void setOsrPc(jsbytecode* osrPc) { osrPc_ = osrPc; }
  jsbytecode* osrPc() const { return osrPc_; }
  void setOsrEntryOffset(uint32_t offset) {
    MOZ_ASSERT(!osrEntryOffset_);
    osrEntryOffset_ = offset;
  }
  uint32_t osrEntryOffset() const { return osrEntryOffset_; }
  bool containsCodeAddress(uint8_t* addr) const {
    return method()->raw() <= addr &&
           addr <= method()->raw() + method()->instructionsSize();
  }
  bool containsReturnAddress(uint8_t* addr) const {
    // This accounts for an off by one error caused by the return address of a
    // bailout sitting outside the range of the containing function.
    return method()->raw() <= addr &&
           addr <= method()->raw() + method()->instructionsSize();
  }
  void setInvalidationEpilogueOffset(uint32_t offset) {
    MOZ_ASSERT(!invalidateEpilogueOffset_);
    invalidateEpilogueOffset_ = offset;
  }
  uint32_t invalidateEpilogueOffset() const {
    MOZ_ASSERT(invalidateEpilogueOffset_);
    return invalidateEpilogueOffset_;
  }
  void setInvalidationEpilogueDataOffset(uint32_t offset) {
    MOZ_ASSERT(!invalidateEpilogueDataOffset_);
    invalidateEpilogueDataOffset_ = offset;
  }
  uint32_t invalidateEpilogueDataOffset() const {
    MOZ_ASSERT(invalidateEpilogueDataOffset_);
    return invalidateEpilogueDataOffset_;
  }

  uint32_t numFixableBailouts() const { return numFixableBailouts_; }

  void incNumFixableBailouts() { numFixableBailouts_++; }
  void resetNumFixableBailouts() { numFixableBailouts_ = 0; }
  void incNumUnfixableBailouts() { numUnfixableBailouts_++; }

  bool shouldInvalidate() const {
    return numFixableBailouts_ >= JitOptions.frequentBailoutThreshold;
  }
  bool shouldInvalidateAndDisable() const {
    return numUnfixableBailouts_ >= JitOptions.frequentBailoutThreshold * 5;
  }

  LICMState licmState() const { return licmState_; }
  void setHadLICMBailout() {
    if (licmState_ == LICMState::NeverBailed) {
      licmState_ = LICMState::Bailed;
    }
  }
  void noteBaselineFallback() {
    if (licmState_ == LICMState::Bailed) {
      licmState_ = LICMState::BailedAndHitFallback;
    }
  }

  void setHasProfilingInstrumentation() { hasProfilingInstrumentation_ = true; }
  void clearHasProfilingInstrumentation() {
    hasProfilingInstrumentation_ = false;
  }
  bool hasProfilingInstrumentation() const {
    return hasProfilingInstrumentation_;
  }

  bool purgedICScripts() const { return purgedICScripts_; }
  void notePurgedICScripts() { purgedICScripts_ = true; }

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this);
  }
  PreBarriered<Value>& getConstant(size_t index) {
    MOZ_ASSERT(index < numConstants());
    return constants()[index];
  }
  uint32_t localSlotsSize() const { return localSlotsSize_; }
  uint32_t argumentSlotsSize() const { return argumentSlotsSize_; }
  uint32_t frameSize() const { return frameSize_; }
  const SafepointIndex* getSafepointIndex(uint32_t disp) const;
  const SafepointIndex* getSafepointIndex(uint8_t* retAddr) const {
    MOZ_ASSERT(containsCodeAddress(retAddr));
    return getSafepointIndex(retAddr - method()->raw());
  }
  const OsiIndex* getOsiIndex(uint32_t disp) const;
  const OsiIndex* getOsiIndex(uint8_t* retAddr) const;

  IonIC& getICFromIndex(uint32_t index) {
    MOZ_ASSERT(index < numICs());
    uint32_t offset = icIndex()[index];
    return getIC(offset);
  }
  inline IonIC& getIC(uint32_t offset) {
    MOZ_ASSERT(offset < runtimeSize());
    return *reinterpret_cast<IonIC*>(runtimeData() + offset);
  }
  void purgeICs(Zone* zone);
  void copySnapshots(const SnapshotWriter* writer);
  void copyRecovers(const RecoverWriter* writer);
  void copyConstants(const Value* vp);
  void copySafepointIndices(const CodegenSafepointIndex* si);
  void copyOsiIndices(const OsiIndex* oi);
  void copyRuntimeData(const uint8_t* data);
  void copyICEntries(const uint32_t* icEntries);
  void copySafepoints(const SafepointWriter* writer);

  bool invalidated() const { return invalidationCount_ != 0; }

  // Invalidate the current compilation.
  void invalidate(JSContext* cx, JSScript* script, bool resetUses,
                  const char* reason);

  size_t invalidationCount() const { return invalidationCount_; }
  void incrementInvalidationCount() { invalidationCount_++; }
  void decrementInvalidationCount(JS::GCContext* gcx) {
    MOZ_ASSERT(invalidationCount_);
    invalidationCount_--;
    if (!invalidationCount_) {
      Destroy(gcx, this);
    }
  }
  IonCompilationId compilationId() const { return compilationId_; }
  uint32_t incrOsrPcMismatchCounter() { return ++osrPcMismatchCounter_; }
  void resetOsrPcMismatchCounter() { osrPcMismatchCounter_ = 0; }

  size_t allocBytes() const { return allocBytes_; }

  static void preWriteBarrier(Zone* zone, IonScript* ionScript);

#ifdef DEBUG
  mozilla::HashNumber icHash() const { return icHash_; }
  void setICHash(mozilla::HashNumber hash) { icHash_ = hash; }
#endif
};

// Execution information for a basic block which may persist after the
// accompanying IonScript is destroyed, for use during profiling.
struct IonBlockCounts {
 private:
  uint32_t id_;

  // Approximate bytecode in the outer (not inlined) script this block
  // was generated from.
  uint32_t offset_;

  // File and line of the inner script this block was generated from.
  char* description_;

  // ids for successors of this block.
  uint32_t numSuccessors_;
  uint32_t* successors_;

  // Hit count for this block.
  uint64_t hitCount_;

  // Text information about the code generated for this block.
  char* code_;

 public:
  [[nodiscard]] bool init(uint32_t id, uint32_t offset, char* description,
                          uint32_t numSuccessors) {
    id_ = id;
    offset_ = offset;
    description_ = description;
    numSuccessors_ = numSuccessors;
    if (numSuccessors) {
      successors_ = js_pod_calloc<uint32_t>(numSuccessors);
      if (!successors_) {
        return false;
      }
    }
    return true;
  }

  void destroy() {
    js_free(description_);
    js_free(successors_);
    js_free(code_);
  }

  uint32_t id() const { return id_; }

  uint32_t offset() const { return offset_; }

  const char* description() const { return description_; }

  size_t numSuccessors() const { return numSuccessors_; }

  void setSuccessor(size_t i, uint32_t id) {
    MOZ_ASSERT(i < numSuccessors_);
    successors_[i] = id;
  }

  uint32_t successor(size_t i) const {
    MOZ_ASSERT(i < numSuccessors_);
    return successors_[i];
  }

  uint64_t* addressOfHitCount() { return &hitCount_; }

  uint64_t hitCount() const { return hitCount_; }

  void setCode(const char* code) {
    char* ncode = js_pod_malloc<char>(strlen(code) + 1);
    if (ncode) {
      strcpy(ncode, code);
      code_ = ncode;
    }
  }

  const char* code() const { return code_; }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(description_) + mallocSizeOf(successors_) +
           mallocSizeOf(code_);
  }
};

// Execution information for a compiled script which may persist after the
// IonScript is destroyed, for use during profiling.
struct IonScriptCounts {
 private:
  // Any previous invalidated compilation(s) for the script.
  IonScriptCounts* previous_ = nullptr;

  // Information about basic blocks in this script.
  size_t numBlocks_ = 0;
  IonBlockCounts* blocks_ = nullptr;

 public:
  IonScriptCounts() = default;

  ~IonScriptCounts() {
    for (size_t i = 0; i < numBlocks_; i++) {
      blocks_[i].destroy();
    }
    js_free(blocks_);
    // The list can be long in some corner cases (bug 1140084), so
    // unroll the recursion.
    IonScriptCounts* victims = previous_;
    while (victims) {
      IonScriptCounts* victim = victims;
      victims = victim->previous_;
      victim->previous_ = nullptr;
      js_delete(victim);
    }
  }

  [[nodiscard]] bool init(size_t numBlocks) {
    blocks_ = js_pod_calloc<IonBlockCounts>(numBlocks);
    if (!blocks_) {
      return false;
    }

    numBlocks_ = numBlocks;
    return true;
  }

  size_t numBlocks() const { return numBlocks_; }

  IonBlockCounts& block(size_t i) {
    MOZ_ASSERT(i < numBlocks_);
    return blocks_[i];
  }

  void setPrevious(IonScriptCounts* previous) { previous_ = previous; }

  IonScriptCounts* previous() const { return previous_; }

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t size = 0;
    auto currCounts = this;
    do {
      size += currCounts->sizeOfOneIncludingThis(mallocSizeOf);
      currCounts = currCounts->previous_;
    } while (currCounts);
    return size;
  }

  size_t sizeOfOneIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t size = mallocSizeOf(this) + mallocSizeOf(blocks_);
    for (size_t i = 0; i < numBlocks_; i++) {
      blocks_[i].sizeOfExcludingThis(mallocSizeOf);
    }
    return size;
  }
};

}  // namespace jit
}  // namespace js

namespace JS {

template <>
struct DeletePolicy<js::jit::IonScript> {
  explicit DeletePolicy(JSRuntime* rt) : rt_(rt) {}
  void operator()(const js::jit::IonScript* script);

 private:
  JSRuntime* rt_;
};

}  // namespace JS

#endif /* jit_IonScript_h */
