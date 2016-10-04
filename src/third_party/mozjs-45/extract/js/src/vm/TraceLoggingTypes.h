/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TraceLoggingTypes_h
#define TraceLoggingTypes_h

#include "jsalloc.h"
#include "jsstr.h"

#define TRACELOGGER_TREE_ITEMS(_)                     \
    _(AnnotateScripts)                                \
    _(Baseline)                                       \
    _(BaselineCompilation)                            \
    _(Engine)                                         \
    _(GC)                                             \
    _(GCAllocation)                                   \
    _(GCSweeping)                                     \
    _(Internal)                                       \
    _(Interpreter)                                    \
    _(InlinedScripts)                                 \
    _(IonCompilation)                                 \
    _(IonCompilationPaused)                           \
    _(IonLinking)                                     \
    _(IonMonkey)                                      \
    _(IrregexpCompile)                                \
    _(IrregexpExecute)                                \
    _(MinorGC)                                        \
    _(ParserCompileFunction)                          \
    _(ParserCompileLazy)                              \
    _(ParserCompileScript)                            \
    _(ParserCompileModule)                            \
    _(Scripts)                                        \
    _(VM)                                             \
                                                      \
    /* Specific passes during ion compilation */      \
    _(PruneUnusedBranches)                            \
    _(FoldTests)                                      \
    _(SplitCriticalEdges)                             \
    _(RenumberBlocks)                                 \
    _(ScalarReplacement)                              \
    _(DominatorTree)                                  \
    _(PhiAnalysis)                                    \
    _(MakeLoopsContiguous)                            \
    _(ApplyTypes)                                     \
    _(EagerSimdUnbox)                                 \
    _(AliasAnalysis)                                  \
    _(GVN)                                            \
    _(LICM)                                           \
    _(Sincos)                                         \
    _(RangeAnalysis)                                  \
    _(LoopUnrolling)                                  \
    _(EffectiveAddressAnalysis)                       \
    _(AlignmentMaskAnalysis)                          \
    _(EliminateDeadCode)                              \
    _(ReorderInstructions)                            \
    _(EdgeCaseAnalysis)                               \
    _(EliminateRedundantChecks)                       \
    _(AddKeepAliveInstructions)                       \
    _(GenerateLIR)                                    \
    _(RegisterAllocation)                             \
    _(GenerateCode)

#define TRACELOGGER_LOG_ITEMS(_)                      \
    _(Bailout)                                        \
    _(Invalidation)                                   \
    _(Disable)                                        \
    _(Enable)                                         \
    _(Stop)

// Predefined IDs for common operations. These IDs can be used
// without using TraceLogCreateTextId, because there are already created.
enum TraceLoggerTextId {
    TraceLogger_Error = 0,
#define DEFINE_TEXT_ID(textId) TraceLogger_ ## textId,
    TRACELOGGER_TREE_ITEMS(DEFINE_TEXT_ID)
    TraceLogger_LastTreeItem,
    TRACELOGGER_LOG_ITEMS(DEFINE_TEXT_ID)
#undef DEFINE_TEXT_ID
    TraceLogger_Last
};

inline const char*
TLTextIdString(TraceLoggerTextId id)
{
    switch (id) {
      case TraceLogger_Error:
        return "TraceLogger failed to process text";
#define NAME(textId) case TraceLogger_ ## textId: return #textId;
        TRACELOGGER_TREE_ITEMS(NAME)
        TRACELOGGER_LOG_ITEMS(NAME)
#undef NAME
      default:
        MOZ_CRASH();
    }
}

uint32_t
TLStringToTextId(JSLinearString* str);

inline bool
TLTextIdIsToggable(uint32_t id)
{
    if (id == TraceLogger_Error)
        return false;
    if (id == TraceLogger_Internal)
        return false;
    if (id == TraceLogger_Stop)
        return false;
    // Actually never used. But added here so it doesn't show as toggle
    if (id == TraceLogger_LastTreeItem)
        return false;
    if (id == TraceLogger_Last)
        return false;
    // Cannot toggle the logging of one engine on/off, because at the stop
    // event it is sometimes unknown which engine was running.
    if (id == TraceLogger_IonMonkey || id == TraceLogger_Baseline || id == TraceLogger_Interpreter)
        return false;
    return true;
}

inline bool
TLTextIdIsTreeEvent(uint32_t id)
{
    // Everything between TraceLogger_Error and TraceLogger_LastTreeItem are tree events and
    // atm also every custom event.
    return (id > TraceLogger_Error && id < TraceLogger_LastTreeItem) ||
           id >= TraceLogger_Last;
}

// The maximum amount of ram memory a continuous space structure can take (in bytes).
static const uint32_t CONTINUOUSSPACE_LIMIT = 200 * 1024 * 1024;

template <class T>
class ContinuousSpace {
    T* data_;
    uint32_t size_;
    uint32_t capacity_;

  public:
    ContinuousSpace ()
     : data_(nullptr)
    { }

    bool init() {
        capacity_ = 64;
        size_ = 0;
        data_ = (T*) js_malloc(capacity_ * sizeof(T));
        if (!data_)
            return false;

        return true;
    }

    ~ContinuousSpace()
    {
        js_free(data_);
        data_ = nullptr;
    }

    T* data() {
        return data_;
    }

    uint32_t capacity() {
        return capacity_;
    }

    uint32_t size() {
        return size_;
    }

    bool empty() {
        return size_ == 0;
    }

    uint32_t lastEntryId() {
        MOZ_ASSERT(!empty());
        return size_ - 1;
    }

    T& lastEntry() {
        return data()[lastEntryId()];
    }

    bool hasSpaceForAdd(uint32_t count = 1) {
        if (size_ + count <= capacity_)
            return true;
        return false;
    }

    bool ensureSpaceBeforeAdd(uint32_t count = 1) {
        MOZ_ASSERT(data_);
        if (hasSpaceForAdd(count))
            return true;

        uint32_t nCapacity = capacity_ * 2;
        if (size_ + count > nCapacity || nCapacity * sizeof(T) > CONTINUOUSSPACE_LIMIT) {
            nCapacity = size_ + count;

            // Limit the size of a continuous buffer.
            if (nCapacity * sizeof(T) > CONTINUOUSSPACE_LIMIT)
                return false;
        }

        T* entries = (T*) js_realloc(data_, nCapacity * sizeof(T));
        if (!entries)
            return false;

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

    void clear() {
        size_ = 0;
    }
};

// The layout of the event log in memory and in the log file.
// Readable by JS using TypedArrays.
struct EventEntry {
    uint64_t time;
    uint32_t textId;
    EventEntry(uint64_t time, uint32_t textId)
      : time(time), textId(textId)
    { }
};

#endif /* TraceLoggingTypes_h */
