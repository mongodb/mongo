/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCMarker_h
#define gc_GCMarker_h

#include "ds/OrderedHashTable.h"
#include "js/SliceBudget.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

namespace js {

class WeakMapBase;

static const size_t NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY = 4096;
static const size_t INCREMENTAL_MARK_STACK_BASE_CAPACITY = 32768;

namespace gc {

struct Cell;

struct WeakKeyTableHashPolicy {
    typedef JS::GCCellPtr Lookup;
    static HashNumber hash(const Lookup& v, const mozilla::HashCodeScrambler&) {
        return mozilla::HashGeneric(v.asCell());
    }
    static bool match(const JS::GCCellPtr& k, const Lookup& l) { return k == l; }
    static bool isEmpty(const JS::GCCellPtr& v) { return !v; }
    static void makeEmpty(JS::GCCellPtr* vp) { *vp = nullptr; }
};

struct WeakMarkable {
    WeakMapBase* weakmap;
    JS::GCCellPtr key;

    WeakMarkable(WeakMapBase* weakmapArg, JS::GCCellPtr keyArg)
      : weakmap(weakmapArg), key(keyArg) {}
};

using WeakEntryVector = Vector<WeakMarkable, 2, js::SystemAllocPolicy>;

using WeakKeyTable = OrderedHashMap<JS::GCCellPtr,
                                    WeakEntryVector,
                                    WeakKeyTableHashPolicy,
                                    js::SystemAllocPolicy>;

/*
 * When the native stack is low, the GC does not call js::TraceChildren to mark
 * the reachable "children" of the thing. Rather the thing is put aside and
 * js::TraceChildren is called later with more space on the C stack.
 *
 * To implement such delayed marking of the children with minimal overhead for
 * the normal case of sufficient native stack, the code adds a field per arena.
 * The field markingDelay->link links all arenas with delayed things into a
 * stack list with the pointer to stack top in GCMarker::unmarkedArenaStackTop.
 * GCMarker::delayMarkingChildren adds arenas to the stack as necessary while
 * markDelayedChildren pops the arenas from the stack until it empties.
 */
class MarkStack
{
  public:
    /*
     * We use a common mark stack to mark GC things of different types and use
     * the explicit tags to distinguish them when it cannot be deduced from
     * the context of push or pop operation.
     */
    enum Tag {
        ValueArrayTag,
        ObjectTag,
        GroupTag,
        SavedValueArrayTag,
        JitCodeTag,
        ScriptTag,
        TempRopeTag,

        LastTag = TempRopeTag
    };

    static const uintptr_t TagMask = 7;
    static_assert(TagMask >= uintptr_t(LastTag), "The tag mask must subsume the tags.");
    static_assert(TagMask <= gc::CellAlignMask, "The tag mask must be embeddable in a Cell*.");

    class TaggedPtr
    {
        uintptr_t bits;

        Cell* ptr() const;

      public:
        TaggedPtr(Tag tag, Cell* ptr);
        Tag tag() const;
        template <typename T> T* as() const;

        JSObject* asValueArrayObject() const;
        JSObject* asSavedValueArrayObject() const;
        JSRope* asTempRope() const;
    };

    struct ValueArray
    {
        ValueArray(JSObject* obj, HeapSlot* start, HeapSlot* end);

        HeapSlot* end;
        HeapSlot* start;
        TaggedPtr ptr;
    };

    struct SavedValueArray
    {
        SavedValueArray(JSObject* obj, size_t index, HeapSlot::Kind kind);

        uintptr_t kind;
        uintptr_t index;
        TaggedPtr ptr;
    };

    explicit MarkStack(size_t maxCapacity = DefaultCapacity);
    ~MarkStack();

    static const size_t DefaultCapacity = SIZE_MAX;

    size_t capacity() { return end_ - stack_; }

    size_t position() const {
        auto result = tos_ - stack_;
        MOZ_ASSERT(result >= 0);
        return size_t(result);
    }

    void setStack(TaggedPtr* stack, size_t tosIndex, size_t capacity);

    MOZ_MUST_USE bool init(JSGCMode gcMode);

    void setBaseCapacity(JSGCMode mode);
    size_t maxCapacity() const { return maxCapacity_; }
    void setMaxCapacity(size_t maxCapacity);

    template <typename T>
    MOZ_MUST_USE bool push(T* ptr);

    MOZ_MUST_USE bool push(JSObject* obj, HeapSlot* start, HeapSlot* end);
    MOZ_MUST_USE bool push(const ValueArray& array);
    MOZ_MUST_USE bool push(const SavedValueArray& array);

    // GCMarker::eagerlyMarkChildren uses unused marking stack as temporary
    // storage to hold rope pointers.
    MOZ_MUST_USE bool pushTempRope(JSRope* ptr);

    bool isEmpty() const {
        return tos_ == stack_;
    }

    Tag peekTag() const;
    TaggedPtr popPtr();
    ValueArray popValueArray();
    SavedValueArray popSavedValueArray();

    void reset();

    void setGCMode(JSGCMode gcMode);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  private:
    MOZ_MUST_USE bool ensureSpace(size_t count);

    /* Grow the stack, ensuring there is space for at least count elements. */
    MOZ_MUST_USE bool enlarge(size_t count);

    const TaggedPtr& peekPtr() const;
    MOZ_MUST_USE bool pushTaggedPtr(Tag tag, Cell* ptr);

    ActiveThreadData<TaggedPtr*> stack_;
    ActiveThreadData<TaggedPtr*> tos_;
    ActiveThreadData<TaggedPtr*> end_;

    // The capacity we start with and reset() to.
    ActiveThreadData<size_t> baseCapacity_;
    ActiveThreadData<size_t> maxCapacity_;

#ifdef DEBUG
    mutable size_t iteratorCount_;
#endif

    friend class MarkStackIter;
};

class MarkStackIter
{
    const MarkStack& stack_;
    MarkStack::TaggedPtr* pos_;

  public:
    explicit MarkStackIter(const MarkStack& stack);
    ~MarkStackIter();

    bool done() const;
    MarkStack::Tag peekTag() const;
    MarkStack::TaggedPtr peekPtr() const;
    MarkStack::ValueArray peekValueArray() const;
    void next();
    void nextPtr();
    void nextArray();

    // Mutate the current ValueArray to a SavedValueArray.
    void saveValueArray(NativeObject* obj, uintptr_t index, HeapSlot::Kind kind);

  private:
    size_t position() const;
};

} /* namespace gc */

class GCMarker : public JSTracer
{
  public:
    explicit GCMarker(JSRuntime* rt);
    MOZ_MUST_USE bool init(JSGCMode gcMode);

    void setMaxCapacity(size_t maxCap) { stack.setMaxCapacity(maxCap); }
    size_t maxCapacity() const { return stack.maxCapacity(); }

    void start();
    void stop();
    void reset();

    // Mark the given GC thing and traverse its children at some point.
    template <typename T> void traverse(T thing);

    // Calls traverse on target after making additional assertions.
    template <typename S, typename T> void traverseEdge(S source, T* target);
    template <typename S, typename T> void traverseEdge(S source, const T& target);

    // Notes a weak graph edge for later sweeping.
    template <typename T> void noteWeakEdge(T* edge);

    /*
     * Care must be taken changing the mark color from gray to black. The cycle
     * collector depends on the invariant that there are no black to gray edges
     * in the GC heap. This invariant lets the CC not trace through black
     * objects. If this invariant is violated, the cycle collector may free
     * objects that are still reachable.
     */
    void setMarkColorGray() {
        MOZ_ASSERT(isDrained());
        MOZ_ASSERT(color == gc::MarkColor::Black);
        color = gc::MarkColor::Gray;
    }
    void setMarkColorBlack() {
        MOZ_ASSERT(isDrained());
        MOZ_ASSERT(color == gc::MarkColor::Gray);
        color = gc::MarkColor::Black;
    }
    gc::MarkColor markColor() const { return color; }

    void enterWeakMarkingMode();
    void leaveWeakMarkingMode();
    void abortLinearWeakMarking() {
        leaveWeakMarkingMode();
        linearWeakMarkingDisabled_ = true;
    }

    void delayMarkingArena(gc::Arena* arena);
    void delayMarkingChildren(const void* thing);
    void markDelayedChildren(gc::Arena* arena);
    MOZ_MUST_USE bool markDelayedChildren(SliceBudget& budget);
    bool hasDelayedChildren() const {
        return !!unmarkedArenaStackTop;
    }

    bool isDrained() {
        return isMarkStackEmpty() && !unmarkedArenaStackTop;
    }

    MOZ_MUST_USE bool drainMarkStack(SliceBudget& budget);

    void setGCMode(JSGCMode mode) { stack.setGCMode(mode); }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                               const AutoLockForExclusiveAccess& lock) const;

#ifdef DEBUG

    bool shouldCheckCompartments() { return strictCompartmentChecking; }

    JS::Zone* stackContainsCrossZonePointerTo(const gc::Cell* cell) const;

#endif

    void markEphemeronValues(gc::Cell* markedCell, gc::WeakEntryVector& entry);

    static GCMarker* fromTracer(JSTracer* trc) {
        MOZ_ASSERT(trc->isMarkingTracer());
        return static_cast<GCMarker*>(trc);
    }

  private:
#ifdef DEBUG
    void checkZone(void* p);
#else
    void checkZone(void* p) {}
#endif

    // Push an object onto the stack for later tracing and assert that it has
    // already been marked.
    inline void repush(JSObject* obj);

    template <typename T> void markAndTraceChildren(T* thing);
    template <typename T> void markAndPush(T* thing);
    template <typename T> void markAndScan(T* thing);
    template <typename T> void markImplicitEdgesHelper(T oldThing);
    template <typename T> void markImplicitEdges(T* oldThing);
    void eagerlyMarkChildren(JSLinearString* str);
    void eagerlyMarkChildren(JSRope* rope);
    void eagerlyMarkChildren(JSString* str);
    void eagerlyMarkChildren(LazyScript *thing);
    void eagerlyMarkChildren(Shape* shape);
    void eagerlyMarkChildren(Scope* scope);
    void lazilyMarkChildren(ObjectGroup* group);

    // We may not have concrete types yet, so this has to be outside the header.
    template <typename T>
    void dispatchToTraceChildren(T* thing);

    // Mark the given GC thing, but do not trace its children. Return true
    // if the thing became marked.
    template <typename T>
    MOZ_MUST_USE bool mark(T* thing);

    template <typename T>
    inline void pushTaggedPtr(T* ptr);

    inline void pushValueArray(JSObject* obj, HeapSlot* start, HeapSlot* end);

    bool isMarkStackEmpty() {
        return stack.isEmpty();
    }

    MOZ_MUST_USE bool restoreValueArray(const gc::MarkStack::SavedValueArray& array,
                                        HeapSlot** vpp, HeapSlot** endp);
    void saveValueRanges();
    inline void processMarkStackTop(SliceBudget& budget);

    /* The mark stack. Pointers in this stack are "gray" in the GC sense. */
    gc::MarkStack stack;

    /* The color is only applied to objects and functions. */
    ActiveThreadData<gc::MarkColor> color;

    /* Pointer to the top of the stack of arenas we are delaying marking on. */
    ActiveThreadData<js::gc::Arena*> unmarkedArenaStackTop;

    /*
     * If the weakKeys table OOMs, disable the linear algorithm and fall back
     * to iterating until the next GC.
     */
    ActiveThreadData<bool> linearWeakMarkingDisabled_;

#ifdef DEBUG
    /* Count of arenas that are currently in the stack. */
    ActiveThreadData<size_t> markLaterArenas;

    /* Assert that start and stop are called with correct ordering. */
    ActiveThreadData<bool> started;

    /*
     * If this is true, all marked objects must belong to a compartment being
     * GCed. This is used to look for compartment bugs.
     */
    ActiveThreadData<bool> strictCompartmentChecking;
#endif // DEBUG
};

} /* namespace js */

// Exported for Tracer.cpp
inline bool ThingIsPermanentAtomOrWellKnownSymbol(js::gc::Cell* thing) { return false; }
bool ThingIsPermanentAtomOrWellKnownSymbol(JSString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSFlatString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSLinearString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSAtom*);
bool ThingIsPermanentAtomOrWellKnownSymbol(js::PropertyName*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JS::Symbol*);

#endif /* gc_GCMarker_h */
