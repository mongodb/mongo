/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TraceLoggingTypes_h
#define TraceLoggingTypes_h

#include "mozilla/Assertions.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TimeStamp.h"

#include <cstddef>
#include <cstdint>

#include "js/AllocPolicy.h"
#include "js/Utility.h"

class JSLinearString;

// Tree items, meaning they have a start and stop and form a nested tree.
#define TRACELOGGER_TREE_ITEMS(_)              \
  _(AnnotateScripts)                           \
  _(Baseline)                                  \
  _(BaselineCompilation)                       \
  _(Engine)                                    \
  _(GC)                                        \
  _(GCAllocation)                              \
  _(GCUnmarking)                               \
  _(GCSweeping)                                \
  _(GCFree)                                    \
  _(Interpreter)                               \
  _(InlinedScripts)                            \
  _(IonAnalysis)                               \
  _(IonCompilation)                            \
  _(IonLinking)                                \
  _(IonMonkey)                                 \
  _(IrregexpCompile)                           \
  _(IrregexpExecute)                           \
  _(MinorGC)                                   \
  _(Frontend)                                  \
  _(ParsingFull)                               \
  _(ParsingSyntax)                             \
  _(BytecodeEmission)                          \
  _(BytecodeFoldConstants)                     \
  _(BytecodeNameFunctions)                     \
  _(DecodeScript)                              \
  _(EncodeScript)                              \
  _(Scripts)                                   \
  _(VM)                                        \
  _(CompressSource)                            \
  _(WasmCompilation)                           \
  _(Call)                                      \
                                               \
  /* Specific passes during ion compilation */ \
  _(PruneUnusedBranches)                       \
  _(FoldTests)                                 \
  _(FoldEmptyBlocks)                           \
  _(SplitCriticalEdges)                        \
  _(RenumberBlocks)                            \
  _(ScalarReplacement)                         \
  _(DominatorTree)                             \
  _(PhiAnalysis)                               \
  _(MakeLoopsContiguous)                       \
  _(ApplyTypes)                                \
  _(EagerSimdUnbox)                            \
  _(AliasAnalysis)                             \
  _(GVN)                                       \
  _(LICM)                                      \
  _(RangeAnalysis)                             \
  _(LoopUnrolling)                             \
  _(Sink)                                      \
  _(FoldLoadsWithUnbox)                        \
  _(RemoveUnnecessaryBitops)                   \
  _(FoldLinearArithConstants)                  \
  _(EffectiveAddressAnalysis)                  \
  _(AlignmentMaskAnalysis)                     \
  _(EliminateDeadCode)                         \
  _(ReorderInstructions)                       \
  _(EdgeCaseAnalysis)                          \
  _(EliminateRedundantChecks)                  \
  _(AddKeepAliveInstructions)                  \
  _(GenerateLIR)                               \
  _(RegisterAllocation)                        \
  _(GenerateCode)                              \
  _(VMSpecific)

// Log items, with timestamp only.
#define TRACELOGGER_LOG_ITEMS(_) \
  _(Bailout)                     \
  _(Invalidation)                \
  _(Disable)                     \
  _(Enable)                      \
  _(Stop)

// Predefined IDs for common operations. These IDs can be used
// without using TraceLogCreateTextId, because there are already created.
enum TraceLoggerTextId {
  TraceLogger_Error = 0,
  TraceLogger_Internal,
#define DEFINE_TEXT_ID(textId) TraceLogger_##textId,
  TRACELOGGER_TREE_ITEMS(DEFINE_TEXT_ID) TraceLogger_TreeItemEnd,
  TRACELOGGER_LOG_ITEMS(DEFINE_TEXT_ID)
#undef DEFINE_TEXT_ID
      TraceLogger_Last
};

inline const char* TLTextIdString(TraceLoggerTextId id) {
  switch (id) {
    case TraceLogger_Error:
      return "TraceLogger failed to process text";
    case TraceLogger_Internal:
    case TraceLogger_TreeItemEnd:
      return "TraceLogger internal event";
#define NAME(textId)         \
  case TraceLogger_##textId: \
    return #textId;
      TRACELOGGER_TREE_ITEMS(NAME)
      TRACELOGGER_LOG_ITEMS(NAME)
#undef NAME
    default:
      MOZ_CRASH();
  }
}

uint32_t TLStringToTextId(JSLinearString* str);

// Return whether a given item id can be enabled/disabled.
inline bool TLTextIdIsTogglable(uint32_t id) {
  if (id == TraceLogger_Error) {
    return false;
  }
  if (id == TraceLogger_Internal) {
    return false;
  }
  if (id == TraceLogger_Stop) {
    return false;
  }
  // Actually never used. But added here so it doesn't show as toggle
  if (id == TraceLogger_TreeItemEnd) {
    return false;
  }
  if (id == TraceLogger_Last) {
    return false;
  }
  // Cannot toggle the logging of one engine on/off, because at the stop
  // event it is sometimes unknown which engine was running.
  if (id == TraceLogger_IonMonkey || id == TraceLogger_Baseline ||
      id == TraceLogger_Interpreter) {
    return false;
  }
  return true;
}

inline bool TLTextIdIsEnumEvent(uint32_t id) { return id < TraceLogger_Last; }

inline bool TLTextIdIsScriptEvent(uint32_t id) {
  return !TLTextIdIsEnumEvent(id);
}

inline bool TLTextIdIsTreeEvent(uint32_t id) {
  // Everything between TraceLogger_Error and TraceLogger_TreeItemEnd are tree
  // events and atm also every custom event.
  return (id > TraceLogger_Error && id < TraceLogger_TreeItemEnd) ||
         id >= TraceLogger_Last;
}

inline bool TLTextIdIsLogEvent(uint32_t id) {
  // These id's do not have start & stop events.
  return (id > TraceLogger_TreeItemEnd && id < TraceLogger_Last);
}

inline bool TLTextIdIsInternalEvent(uint32_t id) {
  // Id's used for bookkeeping.  Does not correspond to real events.
  return (id == TraceLogger_Error || id == TraceLogger_Last ||
          id == TraceLogger_TreeItemEnd || id == TraceLogger_Internal ||
          id == TraceLogger_Stop);
}

template <class T>
class ContinuousSpace {
  T* data_;
  uint32_t size_;
  uint32_t capacity_;

  // The maximum number of bytes of RAM a continuous space structure can take.
  static const uint32_t LIMIT = 200 * 1024 * 1024;

 public:
  ContinuousSpace() : data_(nullptr), size_(0), capacity_(0) {}

  bool init() {
    capacity_ = 64;
    size_ = 0;
    data_ = js_pod_malloc<T>(capacity_);
    if (!data_) {
      return false;
    }

    return true;
  }

  ~ContinuousSpace() {
    js_free(data_);
    data_ = nullptr;
  }

  static uint32_t maxSize() { return LIMIT / sizeof(T); }

  T* data() { return data_; }

  uint32_t capacity() const { return capacity_; }

  uint32_t size() const { return size_; }

  bool empty() const { return size_ == 0; }

  uint32_t lastEntryId() const {
    MOZ_ASSERT(!empty());
    return size_ - 1;
  }

  T& lastEntry() { return data()[lastEntryId()]; }

  bool hasSpaceForAdd(uint32_t count = 1) {
    if (size_ + count <= capacity_) {
      return true;
    }
    return false;
  }

  bool ensureSpaceBeforeAdd(uint32_t count = 1) {
    MOZ_ASSERT(data_);
    if (hasSpaceForAdd(count)) {
      return true;
    }

    // Limit the size of a continuous buffer.
    if (size_ + count > maxSize()) {
      return false;
    }

    uint32_t nCapacity = capacity_ * 2;
    nCapacity = (nCapacity < maxSize()) ? nCapacity : maxSize();

    T* entries = js_pod_realloc<T>(data_, capacity_, nCapacity);
    if (!entries) {
      return false;
    }

    data_ = entries;
    capacity_ = nCapacity;

    return true;
  }

  T& operator[](size_t i) {
    MOZ_ASSERT(i < size_);
    return data()[i];
  }

  void push(T& data) {
    MOZ_ASSERT(size_ < capacity_);
    data()[size_++] = data;
  }

  T& pushUninitialized() {
    MOZ_ASSERT(size_ < capacity_);
    return data()[size_++];
  }

  void pop() {
    MOZ_ASSERT(!empty());
    size_--;
  }

  void clear() { size_ = 0; }

  bool reset() {
    size_t oldCapacity = data_ ? capacity_ : 0;
    capacity_ = 64;
    size_ = 0;
    data_ = js_pod_realloc<T>(data_, oldCapacity, capacity_);

    if (!data_) {
      return false;
    }

    return true;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(data_);
  }
};

// The layout of the event log in memory and in the log file.
// Readable by JS using TypedArrays.
struct EventEntry {
  mozilla::TimeStamp time;
  uint32_t textId;
  EventEntry() : textId(0) {}
};

#endif /* TraceLoggingTypes_h */
