/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCMarker_h
#define gc_GCMarker_h

#include "mozilla/Maybe.h"
#include "mozilla/Variant.h"

#include "ds/OrderedHashTable.h"
#include "gc/Barrier.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "threading/ProtectedData.h"

class JSRope;

namespace js {

class GCMarker;
class SliceBudget;
class WeakMapBase;

static const size_t MARK_STACK_BASE_CAPACITY = 4096;

enum class SlotsOrElementsKind {
  Unused = 0,  // Must match SlotsOrElementsRangeTag
  Elements,
  FixedSlots,
  DynamicSlots
};

namespace gc {

enum IncrementalProgress { NotFinished = 0, Finished };

class AutoSetMarkColor;
class AutoUpdateMarkStackRanges;
struct Cell;
class MarkStackIter;
class ParallelMarker;
class UnmarkGrayTracer;

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
 * The mark stack. Pointers in this stack are "gray" in the GC sense, but
 * their references may be marked either black or gray (in the CC sense).
 *
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
    SlotsOrElementsRangeTag = 0,  // Must match SlotsOrElementsKind::Unused.
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
    uintptr_t asBits() const;
    Tag tag() const;
    uintptr_t tagUnchecked() const;
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

    void setStart(size_t newStart);
    void setEmpty();

   private:
    static constexpr size_t StartShift = 2;
    static constexpr size_t KindMask = (1 << StartShift) - 1;

    uintptr_t startAndKind_;
    TaggedPtr ptr_;
  };

  MarkStack();
  ~MarkStack();

  explicit MarkStack(const MarkStack& other);
  MarkStack& operator=(const MarkStack& other);

  MarkStack(MarkStack&& other);
  MarkStack& operator=(MarkStack&& other);

  // The unit for MarkStack::capacity() is mark stack words.
  size_t capacity() { return stack().length(); }

  size_t position() const { return topIndex_; }

  [[nodiscard]] bool init();
  [[nodiscard]] bool resetStackCapacity();

#ifdef JS_GC_ZEAL
  void setMaxCapacity(size_t maxCapacity);
#endif

  template <typename T>
  [[nodiscard]] bool push(T* ptr);

  [[nodiscard]] bool push(JSObject* obj, SlotsOrElementsKind kind,
                          size_t start);
  [[nodiscard]] bool push(const TaggedPtr& ptr);
  [[nodiscard]] bool push(const SlotsOrElementsRange& array);
  void infalliblePush(const TaggedPtr& ptr);
  void infalliblePush(const SlotsOrElementsRange& array);

  // GCMarker::eagerlyMarkChildren uses unused marking stack as temporary
  // storage to hold rope pointers.
  [[nodiscard]] bool pushTempRope(JSRope* ptr);

  bool isEmpty() const { return position() == 0; }
  bool hasEntries() const { return !isEmpty(); }

  Tag peekTag() const;
  TaggedPtr popPtr();
  SlotsOrElementsRange popSlotsOrElementsRange();

  void clearAndResetCapacity();
  void clearAndFreeStack();

  void poisonUnused();

  [[nodiscard]] bool ensureSpace(size_t count);

  static void moveWork(MarkStack& dst, MarkStack& src);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  using StackVector = Vector<TaggedPtr, 0, SystemAllocPolicy>;
  const StackVector& stack() const { return stack_.ref(); }
  StackVector& stack() { return stack_.ref(); }

  /* Grow the stack, ensuring there is space for at least count elements. */
  [[nodiscard]] bool enlarge(size_t count);

  [[nodiscard]] bool resize(size_t newCapacity);

  TaggedPtr* topPtr();

  const TaggedPtr& peekPtr() const;
  [[nodiscard]] bool pushTaggedPtr(Tag tag, Cell* ptr);

  bool indexIsEntryBase(size_t index) const;

  // Vector containing allocated stack memory. Unused beyond topIndex_.
  MainThreadOrGCTaskData<StackVector> stack_;

  // Index of the top of the stack.
  MainThreadOrGCTaskData<size_t> topIndex_;

#ifdef JS_GC_ZEAL
  // The maximum stack capacity to grow to.
  MainThreadOrGCTaskData<size_t> maxCapacity_{SIZE_MAX};
#endif

#ifdef DEBUG
  MainThreadOrGCTaskData<bool> elementsRangesAreValid;
  friend class js::GCMarker;
#endif

  friend class MarkStackIter;
};

static_assert(unsigned(SlotsOrElementsKind::Unused) ==
                  unsigned(MarkStack::SlotsOrElementsRangeTag),
              "To split the mark stack we depend on being able to tell the "
              "difference between SlotsOrElementsRange::startAndKind_ and a "
              "tagged SlotsOrElementsRange");

class MOZ_STACK_CLASS MarkStackIter {
  MarkStack& stack_;
  size_t pos_;

 public:
  explicit MarkStackIter(MarkStack& stack);

  bool done() const;
  void next();

  MarkStack::Tag peekTag() const;
  bool isSlotsOrElementsRange() const;
  MarkStack::SlotsOrElementsRange& slotsOrElementsRange();

 private:
  size_t position() const;
  MarkStack::TaggedPtr peekPtr() const;
};

// Bitmask of options to parameterize MarkingTracerT.
namespace MarkingOptions {
enum : uint32_t {
  // Set the compartment's hasMarkedCells flag for roots.
  MarkRootCompartments = 1,

  // The marking tracer is operating in parallel. Use appropriate atomic
  // accesses to update the mark bits correctly.
  ParallelMarking = 2,

  // Mark any implicit edges if we are in weak marking mode.
  MarkImplicitEdges = 4,
};
}  // namespace MarkingOptions

constexpr uint32_t NormalMarkingOptions = MarkingOptions::MarkImplicitEdges;

template <uint32_t markingOptions>
class MarkingTracerT
    : public GenericTracerImpl<MarkingTracerT<markingOptions>> {
 public:
  MarkingTracerT(JSRuntime* runtime, GCMarker* marker);
  virtual ~MarkingTracerT() = default;

  template <typename T>
  void onEdge(T** thingp, const char* name);
  friend class GenericTracerImpl<MarkingTracerT<markingOptions>>;

  GCMarker* getMarker();
};

using MarkingTracer = MarkingTracerT<NormalMarkingOptions>;
using RootMarkingTracer = MarkingTracerT<MarkingOptions::MarkRootCompartments>;
using ParallelMarkingTracer = MarkingTracerT<MarkingOptions::ParallelMarking>;

enum ShouldReportMarkTime : bool {
  ReportMarkTime = true,
  DontReportMarkTime = false
};

} /* namespace gc */

class GCMarker {
  enum MarkingState : uint8_t {
    // Have not yet started marking.
    NotActive,

    // Root marking mode. This sets the hasMarkedCells flag on compartments
    // containing objects and scripts, which is used to make sure we clean up
    // dead compartments.
    RootMarking,

    // Main marking mode. Weakmap marking will be populating the
    // gcEphemeronEdges tables but not consulting them. The state will
    // transition to WeakMarking until it is done, then back to RegularMarking.
    RegularMarking,

    // Like RegularMarking but with multiple threads running in parallel.
    ParallelMarking,

    // Same as RegularMarking except now every marked obj/script is immediately
    // looked up in the gcEphemeronEdges table to find edges generated by
    // weakmap keys, and traversing them to their values. Transitions back to
    // RegularMarking when done.
    WeakMarking,
  };

 public:
  explicit GCMarker(JSRuntime* rt);
  [[nodiscard]] bool init();

  JSRuntime* runtime() { return runtime_; }
  JSTracer* tracer() {
    return tracer_.match([](auto& t) -> JSTracer* { return &t; });
  }

#ifdef JS_GC_ZEAL
  void setMaxCapacity(size_t maxCap) { stack.setMaxCapacity(maxCap); }
#endif

  bool isActive() const { return state != NotActive; }
  bool isRegularMarking() const { return state == RegularMarking; }
  bool isParallelMarking() const { return state == ParallelMarking; }
  bool isWeakMarking() const { return state == WeakMarking; }

  gc::MarkColor markColor() const { return markColor_.ref(); }

  bool isDrained() const { return stack.isEmpty() && otherStack.isEmpty(); }

  bool hasEntriesForCurrentColor() { return stack.hasEntries(); }
  bool hasBlackEntries() const { return hasEntries(gc::MarkColor::Black); }
  bool hasGrayEntries() const { return hasEntries(gc::MarkColor::Gray); }
  bool hasEntries(gc::MarkColor color) const;

  bool canDonateWork() const;

  void start();
  void stop();
  void reset();

  [[nodiscard]] bool markUntilBudgetExhausted(
      SliceBudget& budget,
      gc::ShouldReportMarkTime reportTime = gc::ReportMarkTime);

  void setRootMarkingMode(bool newState);

  bool enterWeakMarkingMode();
  void leaveWeakMarkingMode();

  void enterParallelMarkingMode(gc::ParallelMarker* pm);
  void leaveParallelMarkingMode();

  // Do not use linear-time weak marking for the rest of this collection.
  // Currently, this will only be triggered by an OOM when updating needed data
  // structures.
  void abortLinearWeakMarking();

  // 'delegate' is no longer the delegate of 'key'.
  void severWeakDelegate(JSObject* key, JSObject* delegate);

  // 'delegate' is now the delegate of 'key'. Update weakmap marking state.
  void restoreWeakDelegate(JSObject* key, JSObject* delegate);

#ifdef DEBUG
  // We can't check atom marking if the helper thread lock is already held by
  // the current thread. This allows us to disable the check.
  void setCheckAtomMarking(bool check);

  bool shouldCheckCompartments() { return strictCompartmentChecking; }

  bool markOneObjectForTest(JSObject* obj);
#endif

  bool markCurrentColorInParallel(SliceBudget& budget);

  template <uint32_t markingOptions, gc::MarkColor>
  bool markOneColor(SliceBudget& budget);

  static void moveWork(GCMarker* dst, GCMarker* src);

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  static GCMarker* fromTracer(JSTracer* trc) {
    MOZ_ASSERT(trc->isMarkingTracer());
    auto* marker = reinterpret_cast<GCMarker*>(uintptr_t(trc) -
                                               offsetof(GCMarker, tracer_));
    MOZ_ASSERT(marker->tracer() == trc);
    return marker;
  }

  // Internal public methods, for ease of use by the rest of the GC:

  // If |thing| is unmarked, mark it and then traverse its children.
  template <uint32_t, typename T>
  void markAndTraverse(T* thing);

  template <typename T>
  void markImplicitEdges(T* oldThing);

 private:
  /*
   * Care must be taken changing the mark color from gray to black. The cycle
   * collector depends on the invariant that there are no black to gray edges
   * in the GC heap. This invariant lets the CC not trace through black
   * objects. If this invariant is violated, the cycle collector may free
   * objects that are still reachable.
   */
  void setMarkColor(gc::MarkColor newColor);
  friend class js::gc::AutoSetMarkColor;

  template <typename Tracer>
  void setMarkingStateAndTracer(MarkingState prev, MarkingState next);

  // The mutator can shift object elements which could invalidate any elements
  // index on the mark stack. Change the index to be relative to the elements
  // allocation (to ignore shifted elements) while the mutator is running.
  void updateRangesAtStartOfSlice();
  void updateRangesAtEndOfSlice();
  friend class gc::AutoUpdateMarkStackRanges;

  template <uint32_t markingOptions>
  bool processMarkStackTop(SliceBudget& budget);
  friend class gc::GCRuntime;

  // Helper methods that coerce their second argument to the base pointer
  // type.
  template <uint32_t markingOptions, typename S>
  void markAndTraverseObjectEdge(S source, JSObject* target) {
    markAndTraverseEdge<markingOptions>(source, target);
  }
  template <uint32_t markingOptions, typename S>
  void markAndTraverseStringEdge(S source, JSString* target) {
    markAndTraverseEdge<markingOptions>(source, target);
  }

  template <uint32_t markingOptions, typename S, typename T>
  void markAndTraverseEdge(S source, T* target);
  template <uint32_t markingOptions, typename S, typename T>
  void markAndTraverseEdge(S source, const T& target);

  template <typename S, typename T>
  void checkTraversedEdge(S source, T* target);

  // Mark the given GC thing, but do not trace its children. Return true
  // if the thing became marked.
  template <uint32_t markingOptions, typename T>
  [[nodiscard]] bool mark(T* thing);

  // Traverse a GC thing's children, using a strategy depending on the type.
  // This can either processing them immediately or push them onto the mark
  // stack for later.
#define DEFINE_TRAVERSE_METHOD(_1, Type, _2, _3) \
  template <uint32_t>                            \
  void traverse(Type* thing);
  JS_FOR_EACH_TRACEKIND(DEFINE_TRAVERSE_METHOD)
#undef DEFINE_TRAVERSE_METHOD

  // Process a marked thing's children by calling T::traceChildren().
  template <uint32_t markingOptions, typename T>
  void traceChildren(T* thing);

  // Process a marked thing's children recursively using an iterative loop and
  // manual dispatch, for kinds where this is possible.
  template <uint32_t markingOptions, typename T>
  void scanChildren(T* thing);

  // Push a marked thing onto the mark stack. Its children will be marked later.
  template <uint32_t markingOptions, typename T>
  void pushThing(T* thing);

  template <uint32_t markingOptions>
  void eagerlyMarkChildren(JSLinearString* str);
  template <uint32_t markingOptions>
  void eagerlyMarkChildren(JSRope* rope);
  template <uint32_t markingOptions>
  void eagerlyMarkChildren(JSString* str);
  template <uint32_t markingOptions>
  void eagerlyMarkChildren(Shape* shape);
  template <uint32_t markingOptions>
  void eagerlyMarkChildren(PropMap* map);
  template <uint32_t markingOptions>
  void eagerlyMarkChildren(Scope* scope);

  template <typename T>
  inline void pushTaggedPtr(T* ptr);

  inline void pushValueRange(JSObject* obj, SlotsOrElementsKind kind,
                             size_t start, size_t end);

  // Push an object onto the stack for later tracing and assert that it has
  // already been marked.
  inline void repush(JSObject* obj);

  template <typename T>
  void markImplicitEdgesHelper(T oldThing);

  // Mark through edges whose target color depends on the colors of two source
  // entities (eg a WeakMap and one of its keys), and push the target onto the
  // mark stack.
  void markEphemeronEdges(gc::EphemeronEdgeVector& edges,
                          gc::CellColor srcColor);
  friend class JS::Zone;

#ifdef DEBUG
  void checkZone(void* p);
#else
  void checkZone(void* p) {}
#endif

  template <uint32_t markingOptions>
  bool doMarking(SliceBudget& budget, gc::ShouldReportMarkTime reportTime);

  void delayMarkingChildrenOnOOM(gc::Cell* cell);

  /*
   * The JSTracer used for marking. This can change depending on the current
   * state.
   */
  mozilla::Variant<gc::MarkingTracer, gc::RootMarkingTracer,
                   gc::ParallelMarkingTracer>
      tracer_;

  JSRuntime* const runtime_;

  // The main mark stack, holding entries of color |markColor_|.
  gc::MarkStack stack;

  // The auxiliary mark stack, which may contain entries of the other color.
  gc::MarkStack otherStack;

  // Track whether we're using the main or auxiliary stack.
  MainThreadOrGCTaskData<bool> haveSwappedStacks;

  // The current mark stack color.
  MainThreadOrGCTaskData<gc::MarkColor> markColor_;

  MainThreadOrGCTaskData<gc::ParallelMarker*> parallelMarker_;

  Vector<JS::GCCellPtr, 0, SystemAllocPolicy> unmarkGrayStack;
  friend class gc::UnmarkGrayTracer;

  /* Track the state of marking. */
  MainThreadOrGCTaskData<MarkingState> state;

  /* Whether we successfully added all edges to the implicit edges table. */
  MainThreadOrGCTaskData<bool> haveAllImplicitEdges;

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
  /* Assert that start and stop are called with correct ordering. */
  MainThreadOrGCTaskData<bool> started;

  /*
   * Whether to check that atoms traversed are present in atom marking
   * bitmap.
   */
  MainThreadOrGCTaskData<bool> checkAtomMarking;

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
