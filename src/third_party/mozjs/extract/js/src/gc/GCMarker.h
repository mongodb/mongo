/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCMarker_h
#define gc_GCMarker_h

#include "mozilla/Maybe.h"

#include "ds/OrderedHashTable.h"
#include "gc/Barrier.h"
#include "js/SliceBudget.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

class JSRope;

namespace js {

class AutoAccessAtomsZone;
class WeakMapBase;

static const size_t NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY = 4096;
static const size_t INCREMENTAL_MARK_STACK_BASE_CAPACITY = 32768;
static const size_t SMALL_MARK_STACK_BASE_CAPACITY = 256;

enum class SlotsOrElementsKind { Elements, FixedSlots, DynamicSlots };

namespace gc {

enum IncrementalProgress { NotFinished = 0, Finished };

class BarrierTracer;
struct Cell;

using BarrierBuffer = Vector<JS::GCCellPtr, 0, SystemAllocPolicy>;

struct EphemeronEdgeTableHashPolicy {
  using Lookup = Cell*;
  static HashNumber hash(const Lookup& v,
                         const mozilla::HashCodeScrambler& hcs) {
    return hcs.scramble(mozilla::HashGeneric(v));
  }
  static bool match(Cell* const& k, const Lookup& l) { return k == l; }
  static bool isEmpty(Cell* const& v) { return !v; }
  static void makeEmpty(Cell** vp) { *vp = nullptr; }
};

// Ephemeron edges have two source nodes and one target, and mark the target
// with the minimum (least-marked) color of the sources. Currently, one of
// those sources will always be a WeakMapBase, so this will refer to its color
// at the time the edge is traced through. The other source's color will be
// given by the current mark color of the GCMarker.
struct EphemeronEdge {
  CellColor color;
  Cell* target;

  EphemeronEdge(CellColor color_, Cell* cell) : color(color_), target(cell) {}
};

using EphemeronEdgeVector = Vector<EphemeronEdge, 2, js::SystemAllocPolicy>;

using EphemeronEdgeTable =
    OrderedHashMap<Cell*, EphemeronEdgeVector, EphemeronEdgeTableHashPolicy,
                   js::SystemAllocPolicy>;

/*
 * When the mark stack is full, the GC does not call js::TraceChildren to mark
 * the reachable "children" of the thing. Rather the thing is put aside and
 * js::TraceChildren is called later when the mark stack is empty.
 *
 * To implement such delayed marking of the children with minimal overhead for
 * the normal case of sufficient stack, we link arenas into a list using
 * Arena::setNextDelayedMarkingArena(). The head of the list is stored in
 * GCMarker::delayedMarkingList. GCMarker::delayMarkingChildren() adds arenas
 * to the list as necessary while markAllDelayedChildren() pops the arenas from
 * the stack until it is empty.
 */
class MarkStack {
 public:
  /*
   * We use a common mark stack to mark GC things of different types and use
   * the explicit tags to distinguish them when it cannot be deduced from
   * the context of push or pop operation.
   */
  enum Tag {
    SlotsOrElementsRangeTag,
    ObjectTag,
    JitCodeTag,
    ScriptTag,
    TempRopeTag,

    LastTag = TempRopeTag
  };

  static const uintptr_t TagMask = 7;
  static_assert(TagMask >= uintptr_t(LastTag),
                "The tag mask must subsume the tags.");
  static_assert(TagMask <= gc::CellAlignMask,
                "The tag mask must be embeddable in a Cell*.");

  class TaggedPtr {
    uintptr_t bits;

    Cell* ptr() const;

   public:
    TaggedPtr() = default;
    TaggedPtr(Tag tag, Cell* ptr);
    Tag tag() const;
    template <typename T>
    T* as() const;

    JSObject* asRangeObject() const;
    JSRope* asTempRope() const;

    void assertValid() const;
  };

  struct SlotsOrElementsRange {
    SlotsOrElementsRange(SlotsOrElementsKind kind, JSObject* obj, size_t start);
    void assertValid() const;

    SlotsOrElementsKind kind() const;
    size_t start() const;
    TaggedPtr ptr() const;

    static constexpr size_t StartShift = 2;
    static constexpr size_t KindMask = (1 << StartShift) - 1;

   private:
    uintptr_t startAndKind_;
    TaggedPtr ptr_;
  };

  explicit MarkStack(size_t maxCapacity = DefaultCapacity);
  ~MarkStack();

  static const size_t DefaultCapacity = SIZE_MAX;

  // The unit for MarkStack::capacity() is mark stack entries.
  size_t capacity() { return stack().length(); }

  size_t position() const { return topIndex_; }

  enum StackType { MainStack, AuxiliaryStack };
  [[nodiscard]] bool init(StackType which, bool incrementalGCEnabled);

  [[nodiscard]] bool setStackCapacity(StackType which,
                                      bool incrementalGCEnabled);

  size_t maxCapacity() const { return maxCapacity_; }
  void setMaxCapacity(size_t maxCapacity);

  template <typename T>
  [[nodiscard]] bool push(T* ptr);

  [[nodiscard]] bool push(JSObject* obj, SlotsOrElementsKind kind,
                          size_t start);
  [[nodiscard]] bool push(const SlotsOrElementsRange& array);

  // GCMarker::eagerlyMarkChildren uses unused marking stack as temporary
  // storage to hold rope pointers.
  [[nodiscard]] bool pushTempRope(JSRope* ptr);

  bool isEmpty() const { return topIndex_ == 0; }

  Tag peekTag() const;
  TaggedPtr popPtr();
  SlotsOrElementsRange popSlotsOrElementsRange();

  void clear() {
    // Fall back to the smaller initial capacity so we don't hold on to excess
    // memory between GCs.
    stack().clearAndFree();
    (void)stack().resize(NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY);
    topIndex_ = 0;
  }

  void poisonUnused();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  using StackVector = Vector<TaggedPtr, 0, SystemAllocPolicy>;
  const StackVector& stack() const { return stack_.ref(); }
  StackVector& stack() { return stack_.ref(); }

  [[nodiscard]] bool ensureSpace(size_t count);

  /* Grow the stack, ensuring there is space for at least count elements. */
  [[nodiscard]] bool enlarge(size_t count);

  [[nodiscard]] bool resize(size_t newCapacity);

  TaggedPtr* topPtr();

  const TaggedPtr& peekPtr() const;
  [[nodiscard]] bool pushTaggedPtr(Tag tag, Cell* ptr);

  // Index of the top of the stack.
  MainThreadOrGCTaskData<size_t> topIndex_;

  // The maximum stack capacity to grow to.
  MainThreadOrGCTaskData<size_t> maxCapacity_;

  // Vector containing allocated stack memory. Unused beyond topIndex_.
  MainThreadOrGCTaskData<StackVector> stack_;

#ifdef DEBUG
  mutable size_t iteratorCount_;
#endif

  friend class MarkStackIter;
};

class MarkStackIter {
  MarkStack& stack_;
  size_t pos_;

 public:
  explicit MarkStackIter(MarkStack& stack);
  ~MarkStackIter();

  bool done() const;
  MarkStack::Tag peekTag() const;
  MarkStack::TaggedPtr peekPtr() const;
  MarkStack::SlotsOrElementsRange peekSlotsOrElementsRange() const;
  void next();
  void nextPtr();
  void nextArray();

 private:
  size_t position() const;
};

} /* namespace gc */

enum MarkingState : uint8_t {
  // Have not yet started marking.
  NotActive,

  // Main marking mode. Weakmap marking will be populating the gcEphemeronEdges
  // tables but not consulting them. The state will transition to WeakMarking
  // until it is done, then back to RegularMarking.
  RegularMarking,

  // Same as RegularMarking except now every marked obj/script is immediately
  // looked up in the gcEphemeronEdges table to find edges generated by weakmap
  // keys, and traversing them to their values. Transitions back to
  // RegularMarking when done.
  WeakMarking,

  // Same as RegularMarking, but we OOMed (or obeyed a directive in the test
  // marking queue) and fell back to iterating until the next GC.
  IterativeMarking
};

class GCMarker final : public JSTracer {
 public:
  explicit GCMarker(JSRuntime* rt);
  [[nodiscard]] bool init();

  void setMaxCapacity(size_t maxCap) { stack.setMaxCapacity(maxCap); }
  size_t maxCapacity() const { return stack.maxCapacity(); }

  bool isActive() const { return state != MarkingState::NotActive; }

  void start();
  void stop();
  void reset();

  // If |thing| is unmarked, mark it and then traverse its children.
  template <typename T>
  void markAndTraverse(T* thing);

  // Traverse a GC thing's children, using a strategy depending on the type.
  // This can either processing them immediately or push them onto the mark
  // stack for later.
  template <typename T>
  void traverse(T* thing);

  // Calls traverse on target after making additional assertions.
  template <typename S, typename T>
  void markAndTraverseEdge(S source, T* target);
  template <typename S, typename T>
  void markAndTraverseEdge(S source, const T& target);

  // Helper methods that coerce their second argument to the base pointer
  // type.
  template <typename S>
  void markAndTraverseObjectEdge(S source, JSObject* target) {
    markAndTraverseEdge(source, target);
  }
  template <typename S>
  void markAndTraverseStringEdge(S source, JSString* target) {
    markAndTraverseEdge(source, target);
  }

  template <typename S, typename T>
  void checkTraversedEdge(S source, T* target);

#ifdef DEBUG
  // We can't check atom marking if the helper thread lock is already held by
  // the current thread. This allows us to disable the check.
  void setCheckAtomMarking(bool check);
#endif

  /*
   * Care must be taken changing the mark color from gray to black. The cycle
   * collector depends on the invariant that there are no black to gray edges
   * in the GC heap. This invariant lets the CC not trace through black
   * objects. If this invariant is violated, the cycle collector may free
   * objects that are still reachable.
   */
  void setMarkColor(gc::MarkColor newColor);
  void setMarkColorUnchecked(gc::MarkColor newColor);
  gc::MarkColor markColor() const { return color; }

  // Declare which color the main mark stack will be used for. The whole stack
  // must be empty when this is called.
  void setMainStackColor(gc::MarkColor newColor);

  bool enterWeakMarkingMode();
  void leaveWeakMarkingMode();

  // Do not use linear-time weak marking for the rest of this collection.
  // Currently, this will only be triggered by an OOM when updating needed data
  // structures.
  void abortLinearWeakMarking() {
    if (state == MarkingState::WeakMarking) {
      leaveWeakMarkingMode();
    }
    state = MarkingState::IterativeMarking;
  }

  void delayMarkingChildrenOnOOM(gc::Cell* cell);
  void delayMarkingChildren(gc::Cell* cell);

  // 'delegate' is no longer the delegate of 'key'.
  void severWeakDelegate(JSObject* key, JSObject* delegate);

  // 'delegate' is now the delegate of 'key'. Update weakmap marking state.
  void restoreWeakDelegate(JSObject* key, JSObject* delegate);

  bool isDrained();

  // The mark queue is a testing-only feature for controlling mark ordering and
  // yield timing.
  enum MarkQueueProgress {
    QueueYielded,   // End this incremental GC slice, if possible
    QueueComplete,  // Done with the queue
    QueueSuspended  // Continue the GC without ending the slice
  };
  MarkQueueProgress processMarkQueue();

  enum ShouldReportMarkTime : bool {
    ReportMarkTime = true,
    DontReportMarkTime = false
  };
  [[nodiscard]] bool markUntilBudgetExhausted(
      SliceBudget& budget, ShouldReportMarkTime reportTime = ReportMarkTime);

  void setIncrementalGCEnabled(bool enabled) {
    // Ignore failure to resize the stack and keep using the existing stack.
    (void)stack.setStackCapacity(gc::MarkStack::MainStack, enabled);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#ifdef DEBUG
  bool shouldCheckCompartments() { return strictCompartmentChecking; }
#endif

  // Mark through edges whose target color depends on the colors of two source
  // entities (eg a WeakMap and one of its keys), and push the target onto the
  // mark stack.
  void markEphemeronEdges(gc::EphemeronEdgeVector& edges);

  size_t getMarkCount() const { return markCount; }
  void clearMarkCount() { markCount = 0; }

  static GCMarker* fromTracer(JSTracer* trc) {
    MOZ_ASSERT(trc->isMarkingTracer());
    return static_cast<GCMarker*>(trc);
  }

  template <typename T>
  void markImplicitEdges(T* oldThing);

  bool isWeakMarking() const { return state == MarkingState::WeakMarking; }

 private:
#ifdef DEBUG
  void checkZone(void* p);
#else
  void checkZone(void* p) {}
#endif

  // Push an object onto the stack for later tracing and assert that it has
  // already been marked.
  inline void repush(JSObject* obj);

  // Process a marked thing's children by calling T::traceChildren().
  template <typename T>
  void traceChildren(T* thing);

  // Process a marked thing's children recursively using an iterative loop and
  // manual dispatch, for kinds where this is possible.
  template <typename T>
  void scanChildren(T* thing);

  // Push a marked thing onto the mark stack. Its children will be marked later.
  template <typename T>
  void pushThing(T* thing);

  template <typename T>
  void markImplicitEdgesHelper(T oldThing);
  void eagerlyMarkChildren(JSLinearString* str);
  void eagerlyMarkChildren(JSRope* rope);
  void eagerlyMarkChildren(JSString* str);
  void eagerlyMarkChildren(Shape* shape);
  void eagerlyMarkChildren(PropMap* map);
  void eagerlyMarkChildren(Scope* scope);

  // We may not have concrete types yet, so this has to be outside the header.
  template <typename T>
  void dispatchToTraceChildren(T* thing);

  // Mark the given GC thing, but do not trace its children. Return true
  // if the thing became marked.
  template <typename T>
  [[nodiscard]] bool mark(T* thing);

  template <typename T>
  inline void pushTaggedPtr(T* ptr);

  inline void pushValueRange(JSObject* obj, SlotsOrElementsKind kind,
                             size_t start, size_t end);

  gc::MarkStack& getStack(gc::MarkColor which) {
    return which == mainStackColor.ref() ? stack : auxStack;
  }
  const gc::MarkStack& getStack(gc::MarkColor which) const {
    return which == mainStackColor.ref() ? stack : auxStack;
  }

  gc::MarkStack& currentStack() {
    MOZ_ASSERT(currentStackPtr);
    return *currentStackPtr;
  }

  bool isMarkStackEmpty() { return stack.isEmpty() && auxStack.isEmpty(); }

  bool hasBlackEntries() const {
    return !getStack(gc::MarkColor::Black).isEmpty();
  }

  bool hasGrayEntries() const {
    return !getStack(gc::MarkColor::Gray).isEmpty();
  }

  inline void processMarkStackTop(SliceBudget& budget);

  void markDelayedChildren(gc::Arena* arena, gc::MarkColor color);
  [[nodiscard]] bool markAllDelayedChildren(SliceBudget& budget,
                                            ShouldReportMarkTime reportTime);
  bool processDelayedMarkingList(gc::MarkColor color, SliceBudget& budget);
  bool hasDelayedChildren() const { return !!delayedMarkingList; }
  void rebuildDelayedMarkingList();
  void appendToDelayedMarkingList(gc::Arena** listTail, gc::Arena* arena);

  template <typename F>
  void forEachDelayedMarkingArena(F&& f);

  gc::BarrierBuffer& barrierBuffer() { return barrierBuffer_.ref(); }

  bool traceBarrieredCells(SliceBudget& budget);
  friend class gc::GCRuntime;

  void traceBarrieredCell(JS::GCCellPtr cell);

  /*
   * List of cells encountered by the pre-write barrier whose children have yet
   * to be marked. These cells have already been marked black. They are "grey"
   * in the GC sense.
   */
  MainThreadOrGCTaskData<gc::BarrierBuffer> barrierBuffer_;
  friend class gc::BarrierTracer;

  /*
   * The mark stack. Pointers in this stack are "gray" in the GC sense, but may
   * mark the contained items either black or gray (in the CC sense) depending
   * on mainStackColor.
   */
  gc::MarkStack stack;

  /*
   * A smaller, auxiliary stack, currently only used to accumulate the rare
   * objects that need to be marked black during gray marking.
   */
  gc::MarkStack auxStack;

  /* The color is only applied to objects and functions. */
  MainThreadOrGCTaskData<gc::MarkColor> color;

  MainThreadOrGCTaskData<gc::MarkColor> mainStackColor;

  MainThreadOrGCTaskData<gc::MarkStack*> currentStackPtr;

  /* Pointer to the top of the stack of arenas we are delaying marking on. */
  MainThreadOrGCTaskData<js::gc::Arena*> delayedMarkingList;

  /* Whether more work has been added to the delayed marking list. */
  MainThreadOrGCTaskData<bool> delayedMarkingWorkAdded;

  /* The count of marked objects during GC. */
  size_t markCount;

  /* Track the state of marking. */
  MainThreadOrGCTaskData<MarkingState> state;

 public:
  /*
   * Whether weakmaps can be marked incrementally.
   *
   * JSGC_INCREMENTAL_WEAKMAP_ENABLED
   * pref: javascript.options.mem.incremental_weakmap
   */
  MainThreadOrGCTaskData<bool> incrementalWeakMapMarkingEnabled;

#ifdef DEBUG
 private:
  /* Count of arenas that are currently in the stack. */
  MainThreadOrGCTaskData<size_t> markLaterArenas;

  /* Assert that start and stop are called with correct ordering. */
  MainThreadOrGCTaskData<bool> started;

  /*
   * Whether to check that atoms traversed are present in atom marking
   * bitmap.
   */
  MainThreadOrGCTaskData<bool> checkAtomMarking;

  /* The test marking queue might want to be marking a particular color. */
  mozilla::Maybe<js::gc::MarkColor> queueMarkColor;

  /*
   * If this is true, all marked objects must belong to a compartment being
   * GCed. This is used to look for compartment bugs.
   */
  MainThreadOrGCTaskData<bool> strictCompartmentChecking;

 public:
  /*
   * The compartment and zone of the object whose trace hook is currently being
   * called, if any. Used to catch cross-compartment edges traced without use of
   * TraceCrossCompartmentEdge.
   */
  MainThreadOrGCTaskData<Compartment*> tracingCompartment;
  MainThreadOrGCTaskData<Zone*> tracingZone;

  /*
   * List of objects to mark at the beginning of a GC. May also contains string
   * directives to change mark color or wait until different phases of the GC.
   *
   * This is a WeakCache because not everything in this list is guaranteed to
   * end up marked (eg if you insert an object from an already-processed sweep
   * group in the middle of an incremental GC). Also, the mark queue is not
   * used during shutdown GCs. In either case, unmarked objects may need to be
   * discarded.
   */
  JS::WeakCache<GCVector<JS::Heap<JS::Value>, 0, SystemAllocPolicy>> markQueue;

  /* Position within the test mark queue. */
  size_t queuePos;
#endif  // DEBUG
};

namespace gc {

/*
 * Temporarily change the mark color while this class is on the stack.
 *
 * During incremental sweeping this also transitions zones in the
 * current sweep group into the Mark or MarkGray state as appropriate.
 */
class MOZ_RAII AutoSetMarkColor {
  GCMarker& marker_;
  MarkColor initialColor_;

 public:
  AutoSetMarkColor(GCMarker& marker, MarkColor newColor)
      : marker_(marker), initialColor_(marker.markColor()) {
    marker_.setMarkColor(newColor);
  }

  AutoSetMarkColor(GCMarker& marker, CellColor newColor)
      : AutoSetMarkColor(marker, newColor.asMarkColor()) {}

  ~AutoSetMarkColor() { marker_.setMarkColor(initialColor_); }
};

} /* namespace gc */

} /* namespace js */

#endif /* gc_GCMarker_h */
