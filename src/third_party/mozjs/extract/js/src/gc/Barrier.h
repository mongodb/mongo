/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Barrier_h
#define gc_Barrier_h

#include "mozilla/DebugOnly.h"

#include <type_traits>  // std::true_type

#include "NamespaceImports.h"

#include "gc/Cell.h"
#include "gc/StoreBuffer.h"
#include "js/ComparisonOperators.h"  // JS::detail::DefineComparisonOps
#include "js/HeapAPI.h"
#include "js/Id.h"
#include "js/RootingAPI.h"
#include "js/Value.h"
#include "util/Poison.h"

/*
 * [SMDOC] GC Barriers
 *
 * Several kinds of barrier are necessary to allow the GC to function correctly.
 * These are triggered by reading or writing to GC pointers in the heap and
 * serve to tell the collector about changes to the graph of reachable GC
 * things.
 *
 * Since it would be awkward to change every write to memory into a function
 * call, this file contains a bunch of C++ classes and templates that use
 * operator overloading to take care of barriers automatically. In most cases,
 * all that's necessary is to replace:
 *
 *     Type* field;
 *
 * with:
 *
 *     HeapPtr<Type> field;
 *
 * All heap-based GC pointers and tagged pointers must use one of these classes,
 * except in a couple of exceptional cases.
 *
 * These classes are designed to be used by the internals of the JS engine.
 * Barriers designed to be used externally are provided in js/RootingAPI.h.
 *
 * Overview
 * ========
 *
 * This file implements the following concrete classes:
 *
 * HeapPtr       General wrapper for heap-based pointers that provides pre- and
 *               post-write barriers. Most clients should use this.
 *
 * GCPtr         An optimisation of HeapPtr for objects which are only destroyed
 *               by GC finalization (this rules out use in Vector, for example).
 *
 * PreBarriered  Provides a pre-barrier but not a post-barrier. Necessary when
 *               generational GC updates are handled manually, e.g. for hash
 *               table keys that don't use MovableCellHasher.
 *
 * HeapSlot      Provides pre and post-barriers, optimised for use in JSObject
 *               slots and elements.
 *
 * WeakHeapPtr   Provides read and post-write barriers, for use with weak
 *               pointers.
 *
 * The following classes are implemented in js/RootingAPI.h (in the JS
 * namespace):
 *
 * Heap          General wrapper for external clients. Like HeapPtr but also
 *               handles cycle collector concerns. Most external clients should
 *               use this.
 *
 * TenuredHeap   Like Heap but doesn't allow nursery pointers. Allows storing
 *               flags in unused lower bits of the pointer.
 *
 * Which class to use?
 * -------------------
 *
 * Answer the following questions to decide which barrier class is right for
 * your use case:
 *
 * Is your code part of the JS engine?
 *   Yes, it's internal =>
 *     Is your pointer weak or strong?
 *       Strong =>
 *         Do you want automatic handling of nursery pointers?
 *           Yes, of course =>
 *             Can your object be destroyed outside of a GC?
 *               Yes => Use HeapPtr<T>
 *               No => Use GCPtr<T> (optimization)
 *           No, I'll do this myself => Use PreBarriered<T>
 *       Weak => Use WeakHeapPtr<T>
 *   No, it's external =>
 *     Can your pointer refer to nursery objects?
 *       Yes => Use JS::Heap<T>
 *       Never => Use JS::TenuredHeap<T> (optimization)
 *
 * Write barriers
 * ==============
 *
 * A write barrier is a mechanism used by incremental or generational GCs to
 * ensure that every value that needs to be marked is marked. In general, the
 * write barrier should be invoked whenever a write can cause the set of things
 * traced through by the GC to change. This includes:
 *
 *   - writes to object properties
 *   - writes to array slots
 *   - writes to fields like JSObject::shape_ that we trace through
 *   - writes to fields in private data
 *   - writes to non-markable fields like JSObject::private that point to
 *     markable data
 *
 * The last category is the trickiest. Even though the private pointer does not
 * point to a GC thing, changing the private pointer may change the set of
 * objects that are traced by the GC. Therefore it needs a write barrier.
 *
 * Every barriered write should have the following form:
 *
 *   <pre-barrier>
 *   obj->field = value; // do the actual write
 *   <post-barrier>
 *
 * The pre-barrier is used for incremental GC and the post-barrier is for
 * generational GC.
 *
 * Pre-write barrier
 * -----------------
 *
 * To understand the pre-barrier, let's consider how incremental GC works. The
 * GC itself is divided into "slices". Between each slice, JS code is allowed to
 * run. Each slice should be short so that the user doesn't notice the
 * interruptions. In our GC, the structure of the slices is as follows:
 *
 * 1. ... JS work, which leads to a request to do GC ...
 * 2. [first GC slice, which performs all root marking and (maybe) more marking]
 * 3. ... more JS work is allowed to run ...
 * 4. [GC mark slice, which runs entirely in
 *    GCRuntime::markUntilBudgetExhausted]
 * 5. ... more JS work ...
 * 6. [GC mark slice, which runs entirely in
 *    GCRuntime::markUntilBudgetExhausted]
 * 7. ... more JS work ...
 * 8. [GC marking finishes; sweeping done non-incrementally; GC is done]
 * 9. ... JS continues uninterrupted now that GC is finishes ...
 *
 * Of course, there may be a different number of slices depending on how much
 * marking is to be done.
 *
 * The danger inherent in this scheme is that the JS code in steps 3, 5, and 7
 * might change the heap in a way that causes the GC to collect an object that
 * is actually reachable. The write barrier prevents this from happening. We use
 * a variant of incremental GC called "snapshot at the beginning." This approach
 * guarantees the invariant that if an object is reachable in step 2, then we
 * will mark it eventually. The name comes from the idea that we take a
 * theoretical "snapshot" of all reachable objects in step 2; all objects in
 * that snapshot should eventually be marked. (Note that the write barrier
 * verifier code takes an actual snapshot.)
 *
 * The basic correctness invariant of a snapshot-at-the-beginning collector is
 * that any object reachable at the end of the GC (step 9) must either:
 *   (1) have been reachable at the beginning (step 2) and thus in the snapshot
 *   (2) or must have been newly allocated, in steps 3, 5, or 7.
 * To deal with case (2), any objects allocated during an incremental GC are
 * automatically marked black.
 *
 * This strategy is actually somewhat conservative: if an object becomes
 * unreachable between steps 2 and 8, it would be safe to collect it. We won't,
 * mainly for simplicity. (Also, note that the snapshot is entirely
 * theoretical. We don't actually do anything special in step 2 that we wouldn't
 * do in a non-incremental GC.
 *
 * It's the pre-barrier's job to maintain the snapshot invariant. Consider the
 * write "obj->field = value". Let the prior value of obj->field be
 * value0. Since it's possible that value0 may have been what obj->field
 * contained in step 2, when the snapshot was taken, the barrier marks
 * value0. Note that it only does this if we're in the middle of an incremental
 * GC. Since this is rare, the cost of the write barrier is usually just an
 * extra branch.
 *
 * In practice, we implement the pre-barrier differently based on the type of
 * value0. E.g., see JSObject::preWriteBarrier, which is used if obj->field is
 * a JSObject*. It takes value0 as a parameter.
 *
 * Post-write barrier
 * ------------------
 *
 * For generational GC, we want to be able to quickly collect the nursery in a
 * minor collection.  Part of the way this is achieved is to only mark the
 * nursery itself; tenured things, which may form the majority of the heap, are
 * not traced through or marked.  This leads to the problem of what to do about
 * tenured objects that have pointers into the nursery: if such things are not
 * marked, they may be discarded while there are still live objects which
 * reference them. The solution is to maintain information about these pointers,
 * and mark their targets when we start a minor collection.
 *
 * The pointers can be thought of as edges in an object graph, and the set of
 * edges from the tenured generation into the nursery is known as the remembered
 * set. Post barriers are used to track this remembered set.
 *
 * Whenever a slot which could contain such a pointer is written, we check
 * whether the pointed-to thing is in the nursery (if storeBuffer() returns a
 * buffer).  If so we add the cell into the store buffer, which is the
 * collector's representation of the remembered set.  This means that when we
 * come to do a minor collection we can examine the contents of the store buffer
 * and mark any edge targets that are in the nursery.
 *
 * Read barriers
 * =============
 *
 * Weak pointer read barrier
 * -------------------------
 *
 * Weak pointers must have a read barrier to prevent the referent from being
 * collected if it is read after the start of an incremental GC.
 *
 * The problem happens when, during an incremental GC, some code reads a weak
 * pointer and writes it somewhere on the heap that has been marked black in a
 * previous slice. Since the weak pointer will not otherwise be marked and will
 * be swept and finalized in the last slice, this will leave the pointer just
 * written dangling after the GC. To solve this, we immediately mark black all
 * weak pointers that get read between slices so that it is safe to store them
 * in an already marked part of the heap, e.g. in Rooted.
 *
 * Cycle collector read barrier
 * ----------------------------
 *
 * Heap pointers external to the engine may be marked gray. The JS API has an
 * invariant that no gray pointers may be passed, and this maintained by a read
 * barrier that calls ExposeGCThingToActiveJS on such pointers. This is
 * implemented by JS::Heap<T> in js/RootingAPI.h.
 *
 * Implementation Details
 * ======================
 *
 * One additional note: not all object writes need to be pre-barriered. Writes
 * to newly allocated objects do not need a pre-barrier. In these cases, we use
 * the "obj->field.init(value)" method instead of "obj->field = value". We use
 * the init naming idiom in many places to signify that a field is being
 * assigned for the first time.
 *
 * This file implements the following hierarchy of classes:
 *
 * BarrieredBase             base class of all barriers
 *  |  |
 *  | WriteBarriered         base class which provides common write operations
 *  |  |  |  |  |
 *  |  |  |  | PreBarriered  provides pre-barriers only
 *  |  |  |  |
 *  |  |  | GCPtr            provides pre- and post-barriers
 *  |  |  |
 *  |  | HeapPtr             provides pre- and post-barriers; is relocatable
 *  |  |                     and deletable for use inside C++ managed memory
 *  |  |
 *  | HeapSlot               similar to GCPtr, but tailored to slots storage
 *  |
 * ReadBarriered             base class which provides common read operations
 *  |
 * WeakHeapPtr               provides read barriers only
 *
 *
 * The implementation of the barrier logic is implemented in the
 * Cell/TenuredCell base classes, which are called via:
 *
 * WriteBarriered<T>::pre
 *  -> InternalBarrierMethods<T*>::preBarrier
 *      -> Cell::preWriteBarrier
 *  -> InternalBarrierMethods<Value>::preBarrier
 *  -> InternalBarrierMethods<jsid>::preBarrier
 *      -> InternalBarrierMethods<T*>::preBarrier
 *          -> Cell::preWriteBarrier
 *
 * GCPtr<T>::post and HeapPtr<T>::post
 *  -> InternalBarrierMethods<T*>::postBarrier
 *      -> gc::PostWriteBarrierImpl
 *  -> InternalBarrierMethods<Value>::postBarrier
 *      -> StoreBuffer::put
 *
 * Barriers for use outside of the JS engine call into the same barrier
 * implementations at InternalBarrierMethods<T>::post via an indirect call to
 * Heap(.+)PostWriteBarrier.
 *
 * These clases are designed to be used to wrap GC thing pointers or values that
 * act like them (i.e. JS::Value and jsid).  It is possible to use them for
 * other types by supplying the necessary barrier implementations but this
 * is not usually necessary and should be done with caution.
 */

namespace js {

class NativeObject;

namespace gc {

void ValueReadBarrier(const Value& v);
void ValuePreWriteBarrier(const Value& v);
void IdPreWriteBarrier(jsid id);
void CellPtrPreWriteBarrier(JS::GCCellPtr thing);

}  // namespace gc

#ifdef DEBUG

bool CurrentThreadIsTouchingGrayThings();

bool IsMarkedBlack(JSObject* obj);

#endif

struct MOZ_RAII AutoTouchingGrayThings {
#ifdef DEBUG
  AutoTouchingGrayThings();
  ~AutoTouchingGrayThings();
#else
  AutoTouchingGrayThings() {}
#endif
};

template <typename T>
struct InternalBarrierMethods {};

template <typename T>
struct InternalBarrierMethods<T*> {
  static bool isMarkable(const T* v) { return v != nullptr; }

  static void preBarrier(T* v) { gc::PreWriteBarrier(v); }

  static void postBarrier(T** vp, T* prev, T* next) {
    gc::PostWriteBarrier(vp, prev, next);
  }

  static void readBarrier(T* v) { gc::ReadBarrier(v); }

#ifdef DEBUG
  static void assertThingIsNotGray(T* v) { return T::assertThingIsNotGray(v); }
#endif
};

template <>
struct InternalBarrierMethods<Value> {
  static bool isMarkable(const Value& v) { return v.isGCThing(); }

  static void preBarrier(const Value& v) {
    if (v.isGCThing()) {
      gc::ValuePreWriteBarrier(v);
    }
  }

  static MOZ_ALWAYS_INLINE void postBarrier(Value* vp, const Value& prev,
                                            const Value& next) {
    MOZ_ASSERT(!CurrentThreadIsIonCompiling());
    MOZ_ASSERT(vp);

    // If the target needs an entry, add it.
    js::gc::StoreBuffer* sb;
    if ((next.isObject() || next.isString() || next.isBigInt()) &&
        (sb = next.toGCThing()->storeBuffer())) {
      // If we know that the prev has already inserted an entry, we can
      // skip doing the lookup to add the new entry. Note that we cannot
      // safely assert the presence of the entry because it may have been
      // added via a different store buffer.
      if ((prev.isObject() || prev.isString() || prev.isBigInt()) &&
          prev.toGCThing()->storeBuffer()) {
        return;
      }
      sb->putValue(vp);
      return;
    }
    // Remove the prev entry if the new value does not need it.
    if ((prev.isObject() || prev.isString() || prev.isBigInt()) &&
        (sb = prev.toGCThing()->storeBuffer())) {
      sb->unputValue(vp);
    }
  }

  static void readBarrier(const Value& v) {
    if (v.isGCThing()) {
      gc::ValueReadBarrier(v);
    }
  }

#ifdef DEBUG
  static void assertThingIsNotGray(const Value& v) {
    JS::AssertValueIsNotGray(v);
  }
#endif
};

template <>
struct InternalBarrierMethods<jsid> {
  static bool isMarkable(jsid id) { return id.isGCThing(); }
  static void preBarrier(jsid id) {
    if (id.isGCThing()) {
      gc::IdPreWriteBarrier(id);
    }
  }
  static void postBarrier(jsid* idp, jsid prev, jsid next) {}
#ifdef DEBUG
  static void assertThingIsNotGray(jsid id) { JS::AssertIdIsNotGray(id); }
#endif
};

template <typename T>
static inline void AssertTargetIsNotGray(const T& v) {
#ifdef DEBUG
  if (!CurrentThreadIsTouchingGrayThings()) {
    InternalBarrierMethods<T>::assertThingIsNotGray(v);
  }
#endif
}

// Base class of all barrier types.
//
// This is marked non-memmovable since post barriers added by derived classes
// can add pointers to class instances to the store buffer.
template <typename T>
class MOZ_NON_MEMMOVABLE BarrieredBase {
 protected:
  // BarrieredBase is not directly instantiable.
  explicit BarrieredBase(const T& v) : value(v) {}

  // BarrieredBase subclasses cannot be copy constructed by default.
  BarrieredBase(const BarrieredBase<T>& other) = default;

  // Storage for all barrier classes. |value| must be a GC thing reference
  // type: either a direct pointer to a GC thing or a supported tagged
  // pointer that can reference GC things, such as JS::Value or jsid. Nested
  // barrier types are NOT supported. See assertTypeConstraints.
  T value;

 public:
  using ElementType = T;

  // Note: this is public because C++ cannot friend to a specific template
  // instantiation. Friending to the generic template leads to a number of
  // unintended consequences, including template resolution ambiguity and a
  // circular dependency with Tracing.h.
  T* unbarrieredAddress() const { return const_cast<T*>(&value); }
};

// Base class for barriered pointer types that intercept only writes.
template <class T>
class WriteBarriered : public BarrieredBase<T>,
                       public WrappedPtrOperations<T, WriteBarriered<T>> {
 protected:
  using BarrieredBase<T>::value;

  // WriteBarriered is not directly instantiable.
  explicit WriteBarriered(const T& v) : BarrieredBase<T>(v) {}

 public:
  DECLARE_POINTER_CONSTREF_OPS(T);

  // Use this if the automatic coercion to T isn't working.
  const T& get() const { return this->value; }

  // Use this if you want to change the value without invoking barriers.
  // Obviously this is dangerous unless you know the barrier is not needed.
  void unbarrieredSet(const T& v) { this->value = v; }

  // For users who need to manually barrier the raw types.
  static void preWriteBarrier(const T& v) {
    InternalBarrierMethods<T>::preBarrier(v);
  }

 protected:
  void pre() { InternalBarrierMethods<T>::preBarrier(this->value); }
  MOZ_ALWAYS_INLINE void post(const T& prev, const T& next) {
    InternalBarrierMethods<T>::postBarrier(&this->value, prev, next);
  }
};

#define DECLARE_POINTER_ASSIGN_AND_MOVE_OPS(Wrapper, T) \
  DECLARE_POINTER_ASSIGN_OPS(Wrapper, T)                \
  Wrapper<T>& operator=(Wrapper<T>&& other) {           \
    setUnchecked(other.release());                      \
    return *this;                                       \
  }

/*
 * PreBarriered only automatically handles pre-barriers. Post-barriers must be
 * manually implemented when using this class. GCPtr and HeapPtr should be used
 * in all cases that do not require explicit low-level control of moving
 * behavior.
 *
 * This class is useful for example for HashMap keys where automatically
 * updating a moved nursery pointer would break the hash table.
 */
template <class T>
class PreBarriered : public WriteBarriered<T> {
 public:
  PreBarriered() : WriteBarriered<T>(JS::SafelyInitialized<T>()) {}
  /*
   * Allow implicit construction for use in generic contexts, such as
   * DebuggerWeakMap::markKeys.
   */
  MOZ_IMPLICIT PreBarriered(const T& v) : WriteBarriered<T>(v) {}

  explicit PreBarriered(const PreBarriered<T>& other)
      : WriteBarriered<T>(other.value) {}

  PreBarriered(PreBarriered<T>&& other) : WriteBarriered<T>(other.release()) {}

  ~PreBarriered() { this->pre(); }

  void init(const T& v) { this->value = v; }

  /* Use to set the pointer to nullptr. */
  void clear() { set(JS::SafelyInitialized<T>()); }

  DECLARE_POINTER_ASSIGN_AND_MOVE_OPS(PreBarriered, T);

 private:
  void set(const T& v) {
    AssertTargetIsNotGray(v);
    setUnchecked(v);
  }

  void setUnchecked(const T& v) {
    this->pre();
    this->value = v;
  }

  T release() {
    T tmp = this->value;
    this->value = JS::SafelyInitialized<T>();
    return tmp;
  }
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::PreBarriered<T>> : std::true_type {
  static const T& get(const js::PreBarriered<T>& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

/*
 * A pre- and post-barriered heap pointer, for use inside the JS engine.
 *
 * It must only be stored in memory that has GC lifetime. GCPtr must not be
 * used in contexts where it may be implicitly moved or deleted, e.g. most
 * containers.
 *
 * The post-barriers implemented by this class are faster than those
 * implemented by js::HeapPtr<T> or JS::Heap<T> at the cost of not
 * automatically handling deletion or movement.
 */
template <class T>
class GCPtr : public WriteBarriered<T> {
 public:
  GCPtr() : WriteBarriered<T>(JS::SafelyInitialized<T>()) {}

  explicit GCPtr(const T& v) : WriteBarriered<T>(v) {
    this->post(JS::SafelyInitialized<T>(), v);
  }

  explicit GCPtr(const GCPtr<T>& v) : WriteBarriered<T>(v) {
    this->post(JS::SafelyInitialized<T>(), v);
  }

#ifdef DEBUG
  ~GCPtr() {
    // No barriers are necessary as this only happens when the GC is sweeping.
    //
    // If this assertion fails you may need to make the containing object use a
    // HeapPtr instead, as this can be deleted from outside of GC.
    MOZ_ASSERT(CurrentThreadIsGCSweeping() || CurrentThreadIsGCFinalizing());

    Poison(this, JS_FREED_HEAP_PTR_PATTERN, sizeof(*this),
           MemCheckKind::MakeNoAccess);
  }
#endif

  void init(const T& v) {
    AssertTargetIsNotGray(v);
    this->value = v;
    this->post(JS::SafelyInitialized<T>(), v);
  }

  DECLARE_POINTER_ASSIGN_OPS(GCPtr, T);

 private:
  void set(const T& v) {
    AssertTargetIsNotGray(v);
    setUnchecked(v);
  }

  void setUnchecked(const T& v) {
    this->pre();
    T tmp = this->value;
    this->value = v;
    this->post(tmp, this->value);
  }

  /*
   * Unlike HeapPtr<T>, GCPtr<T> must be managed with GC lifetimes.
   * Specifically, the memory used by the pointer itself must be live until
   * at least the next minor GC. For that reason, move semantics are invalid
   * and are deleted here. Please note that not all containers support move
   * semantics, so this does not completely prevent invalid uses.
   */
  GCPtr(GCPtr<T>&&) = delete;
  GCPtr<T>& operator=(GCPtr<T>&&) = delete;
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::GCPtr<T>> : std::true_type {
  static const T& get(const js::GCPtr<T>& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

/*
 * A pre- and post-barriered heap pointer, for use inside the JS engine. These
 * heap pointers can be stored in C++ containers like GCVector and GCHashMap.
 *
 * The GC sometimes keeps pointers to pointers to GC things --- for example, to
 * track references into the nursery. However, C++ containers like GCVector and
 * GCHashMap usually reserve the right to relocate their elements any time
 * they're modified, invalidating all pointers to the elements. HeapPtr
 * has a move constructor which knows how to keep the GC up to date if it is
 * moved to a new location.
 *
 * However, because of this additional communication with the GC, HeapPtr
 * is somewhat slower, so it should only be used in contexts where this ability
 * is necessary.
 *
 * Obviously, JSObjects, JSStrings, and the like get tenured and compacted, so
 * whatever pointers they contain get relocated, in the sense used here.
 * However, since the GC itself is moving those values, it takes care of its
 * internal pointers to those pointers itself. HeapPtr is only necessary
 * when the relocation would otherwise occur without the GC's knowledge.
 */
template <class T>
class HeapPtr : public WriteBarriered<T> {
 public:
  HeapPtr() : WriteBarriered<T>(JS::SafelyInitialized<T>()) {}

  // Implicitly adding barriers is a reasonable default.
  MOZ_IMPLICIT HeapPtr(const T& v) : WriteBarriered<T>(v) {
    this->post(JS::SafelyInitialized<T>(), this->value);
  }

  MOZ_IMPLICIT HeapPtr(const HeapPtr<T>& other) : WriteBarriered<T>(other) {
    this->post(JS::SafelyInitialized<T>(), this->value);
  }

  HeapPtr(HeapPtr<T>&& other) : WriteBarriered<T>(other.release()) {
    this->post(JS::SafelyInitialized<T>(), this->value);
  }

  ~HeapPtr() {
    this->pre();
    this->post(this->value, JS::SafelyInitialized<T>());
  }

  void init(const T& v) {
    MOZ_ASSERT(this->value == JS::SafelyInitialized<T>());
    AssertTargetIsNotGray(v);
    this->value = v;
    this->post(JS::SafelyInitialized<T>(), this->value);
  }

  DECLARE_POINTER_ASSIGN_AND_MOVE_OPS(HeapPtr, T);

  /* Make this friend so it can access pre() and post(). */
  template <class T1, class T2>
  friend inline void BarrieredSetPair(Zone* zone, HeapPtr<T1*>& v1, T1* val1,
                                      HeapPtr<T2*>& v2, T2* val2);

 protected:
  void set(const T& v) {
    AssertTargetIsNotGray(v);
    setUnchecked(v);
  }

  void setUnchecked(const T& v) {
    this->pre();
    postBarrieredSet(v);
  }

  void postBarrieredSet(const T& v) {
    T tmp = this->value;
    this->value = v;
    this->post(tmp, this->value);
  }

  T release() {
    T tmp = this->value;
    postBarrieredSet(JS::SafelyInitialized<T>());
    return tmp;
  }
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::HeapPtr<T>> : std::true_type {
  static const T& get(const js::HeapPtr<T>& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

// Base class for barriered pointer types that intercept reads and writes.
template <typename T>
class ReadBarriered : public BarrieredBase<T> {
 protected:
  // ReadBarriered is not directly instantiable.
  explicit ReadBarriered(const T& v) : BarrieredBase<T>(v) {}

  void read() const { InternalBarrierMethods<T>::readBarrier(this->value); }
  void post(const T& prev, const T& next) {
    InternalBarrierMethods<T>::postBarrier(&this->value, prev, next);
  }
};

// Incremental GC requires that weak pointers have read barriers. See the block
// comment at the top of Barrier.h for a complete discussion of why.
//
// Note that this class also has post-barriers, so is safe to use with nursery
// pointers. However, when used as a hashtable key, care must still be taken to
// insert manual post-barriers on the table for rekeying if the key is based in
// any way on the address of the object.
template <typename T>
class WeakHeapPtr : public ReadBarriered<T>,
                    public WrappedPtrOperations<T, WeakHeapPtr<T>> {
 protected:
  using ReadBarriered<T>::value;

 public:
  WeakHeapPtr() : ReadBarriered<T>(JS::SafelyInitialized<T>()) {}

  // It is okay to add barriers implicitly.
  MOZ_IMPLICIT WeakHeapPtr(const T& v) : ReadBarriered<T>(v) {
    this->post(JS::SafelyInitialized<T>(), v);
  }

  // The copy constructor creates a new weak edge but the wrapped pointer does
  // not escape, so no read barrier is necessary.
  explicit WeakHeapPtr(const WeakHeapPtr& other) : ReadBarriered<T>(other) {
    this->post(JS::SafelyInitialized<T>(), value);
  }

  // Move retains the lifetime status of the source edge, so does not fire
  // the read barrier of the defunct edge.
  WeakHeapPtr(WeakHeapPtr&& other) : ReadBarriered<T>(other.release()) {
    this->post(JS::SafelyInitialized<T>(), value);
  }

  ~WeakHeapPtr() { this->post(this->value, JS::SafelyInitialized<T>()); }

  WeakHeapPtr& operator=(const WeakHeapPtr& v) {
    AssertTargetIsNotGray(v.value);
    T prior = this->value;
    this->value = v.value;
    this->post(prior, v.value);
    return *this;
  }

  const T& get() const {
    if (InternalBarrierMethods<T>::isMarkable(this->value)) {
      this->read();
    }
    return this->value;
  }

  const T& unbarrieredGet() const { return this->value; }

  explicit operator bool() const { return bool(this->value); }

  operator const T&() const { return get(); }

  const T& operator->() const { return get(); }

  void set(const T& v) {
    AssertTargetIsNotGray(v);
    setUnchecked(v);
  }

  void unbarrieredSet(const T& v) {
    AssertTargetIsNotGray(v);
    this->value = v;
  }

 private:
  void setUnchecked(const T& v) {
    T tmp = this->value;
    this->value = v;
    this->post(tmp, v);
  }

  T release() {
    T tmp = value;
    set(JS::SafelyInitialized<T>());
    return tmp;
  }
};

}  // namespace js

namespace JS {

namespace detail {

template <typename T>
struct DefineComparisonOps<js::WeakHeapPtr<T>> : std::true_type {
  static const T& get(const js::WeakHeapPtr<T>& v) {
    return v.unbarrieredGet();
  }
};

}  // namespace detail

}  // namespace JS

namespace js {

// A pre- and post-barriered Value that is specialized to be aware that it
// resides in a slots or elements vector. This allows it to be relocated in
// memory, but with substantially less overhead than a HeapPtr.
class HeapSlot : public WriteBarriered<Value> {
 public:
  enum Kind { Slot = 0, Element = 1 };

  void init(NativeObject* owner, Kind kind, uint32_t slot, const Value& v) {
    value = v;
    post(owner, kind, slot, v);
  }

  void destroy() { pre(); }

#ifdef DEBUG
  bool preconditionForSet(NativeObject* owner, Kind kind, uint32_t slot) const;
  void assertPreconditionForPostWriteBarrier(NativeObject* obj, Kind kind,
                                             uint32_t slot,
                                             const Value& target) const;
#endif

  MOZ_ALWAYS_INLINE void set(NativeObject* owner, Kind kind, uint32_t slot,
                             const Value& v) {
    MOZ_ASSERT(preconditionForSet(owner, kind, slot));
    pre();
    value = v;
    post(owner, kind, slot, v);
  }

 private:
  void post(NativeObject* owner, Kind kind, uint32_t slot,
            const Value& target) {
#ifdef DEBUG
    assertPreconditionForPostWriteBarrier(owner, kind, slot, target);
#endif
    if (this->value.isObject() || this->value.isString() ||
        this->value.isBigInt()) {
      gc::Cell* cell = this->value.toGCThing();
      if (cell->storeBuffer()) {
        cell->storeBuffer()->putSlot(owner, kind, slot, 1);
      }
    }
  }
};

}  // namespace js

namespace JS {

namespace detail {

template <>
struct DefineComparisonOps<js::HeapSlot> : std::true_type {
  static const Value& get(const js::HeapSlot& v) { return v.get(); }
};

}  // namespace detail

}  // namespace JS

namespace js {

class HeapSlotArray {
  HeapSlot* array;

 public:
  explicit HeapSlotArray(HeapSlot* array) : array(array) {}

  HeapSlot* begin() const { return array; }

  operator const Value*() const {
    static_assert(sizeof(GCPtr<Value>) == sizeof(Value));
    static_assert(sizeof(HeapSlot) == sizeof(Value));
    return reinterpret_cast<const Value*>(array);
  }
  operator HeapSlot*() const { return begin(); }

  HeapSlotArray operator+(int offset) const {
    return HeapSlotArray(array + offset);
  }
  HeapSlotArray operator+(uint32_t offset) const {
    return HeapSlotArray(array + offset);
  }
};

/*
 * This is a hack for RegExpStatics::updateFromMatch. It allows us to do two
 * barriers with only one branch to check if we're in an incremental GC.
 */
template <class T1, class T2>
static inline void BarrieredSetPair(Zone* zone, HeapPtr<T1*>& v1, T1* val1,
                                    HeapPtr<T2*>& v2, T2* val2) {
  AssertTargetIsNotGray(val1);
  AssertTargetIsNotGray(val2);
  if (T1::needPreWriteBarrier(zone)) {
    v1.pre();
    v2.pre();
  }
  v1.postBarrieredSet(val1);
  v2.postBarrieredSet(val2);
}

/*
 * ImmutableTenuredPtr is designed for one very narrow case: replacing
 * immutable raw pointers to GC-managed things, implicitly converting to a
 * handle type for ease of use. Pointers encapsulated by this type must:
 *
 *   be immutable (no incremental write barriers),
 *   never point into the nursery (no generational write barriers), and
 *   be traced via MarkRuntime (we use fromMarkedLocation).
 *
 * In short: you *really* need to know what you're doing before you use this
 * class!
 */
template <typename T>
class MOZ_HEAP_CLASS ImmutableTenuredPtr {
  T value;

 public:
  operator T() const { return value; }
  T operator->() const { return value; }

  // `ImmutableTenuredPtr<T>` is implicitly convertible to `Handle<T>`.
  //
  // In case you need to convert to `Handle<U>` where `U` is base class of `T`,
  // convert this to `Handle<T>` by `toHandle()` and then use implicit
  // conversion from `Handle<T>` to `Handle<U>`.
  operator Handle<T>() const { return toHandle(); }
  Handle<T> toHandle() const { return Handle<T>::fromMarkedLocation(&value); }

  void init(T ptr) {
    MOZ_ASSERT(ptr->isTenured());
    AssertTargetIsNotGray(ptr);
    value = ptr;
  }

  T get() const { return value; }
  const T* address() { return &value; }
};

#if MOZ_IS_GCC
template struct JS_PUBLIC_API MovableCellHasher<JSObject*>;
#endif

template <typename T>
struct MovableCellHasher<PreBarriered<T>> {
  using Key = PreBarriered<T>;
  using Lookup = T;

  static bool hasHash(const Lookup& l) {
    return MovableCellHasher<T>::hasHash(l);
  }
  static bool ensureHash(const Lookup& l) {
    return MovableCellHasher<T>::ensureHash(l);
  }
  static HashNumber hash(const Lookup& l) {
    return MovableCellHasher<T>::hash(l);
  }
  static bool match(const Key& k, const Lookup& l) {
    return MovableCellHasher<T>::match(k, l);
  }
};

template <typename T>
struct MovableCellHasher<HeapPtr<T>> {
  using Key = HeapPtr<T>;
  using Lookup = T;

  static bool hasHash(const Lookup& l) {
    return MovableCellHasher<T>::hasHash(l);
  }
  static bool ensureHash(const Lookup& l) {
    return MovableCellHasher<T>::ensureHash(l);
  }
  static HashNumber hash(const Lookup& l) {
    return MovableCellHasher<T>::hash(l);
  }
  static bool match(const Key& k, const Lookup& l) {
    return MovableCellHasher<T>::match(k, l);
  }
};

template <typename T>
struct MovableCellHasher<WeakHeapPtr<T>> {
  using Key = WeakHeapPtr<T>;
  using Lookup = T;

  static bool hasHash(const Lookup& l) {
    return MovableCellHasher<T>::hasHash(l);
  }
  static bool ensureHash(const Lookup& l) {
    return MovableCellHasher<T>::ensureHash(l);
  }
  static HashNumber hash(const Lookup& l) {
    return MovableCellHasher<T>::hash(l);
  }
  static bool match(const Key& k, const Lookup& l) {
    return MovableCellHasher<T>::match(k.unbarrieredGet(), l);
  }
};

/* Useful for hashtables with a HeapPtr as key. */
template <class T>
struct HeapPtrHasher {
  using Key = HeapPtr<T>;
  using Lookup = T;

  static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
  static bool match(const Key& k, Lookup l) { return k.get() == l; }
  static void rekey(Key& k, const Key& newKey) { k.unbarrieredSet(newKey); }
};

template <class T>
struct PreBarrieredHasher {
  using Key = PreBarriered<T>;
  using Lookup = T;

  static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
  static bool match(const Key& k, Lookup l) { return k.get() == l; }
  static void rekey(Key& k, const Key& newKey) { k.unbarrieredSet(newKey); }
};

/* Useful for hashtables with a WeakHeapPtr as key. */
template <class T>
struct WeakHeapPtrHasher {
  using Key = WeakHeapPtr<T>;
  using Lookup = T;

  static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
  static bool match(const Key& k, Lookup l) { return k.unbarrieredGet() == l; }
  static void rekey(Key& k, const Key& newKey) {
    k.set(newKey.unbarrieredGet());
  }
};

}  // namespace js

namespace mozilla {

template <class T>
struct DefaultHasher<js::HeapPtr<T>> : js::HeapPtrHasher<T> {};

template <class T>
struct DefaultHasher<js::GCPtr<T>> {
  // Not implemented. GCPtr can't be used as a hash table key because it has a
  // post barrier but doesn't support relocation.
};

template <class T>
struct DefaultHasher<js::PreBarriered<T>> : js::PreBarrieredHasher<T> {};

template <class T>
struct DefaultHasher<js::WeakHeapPtr<T>> : js::WeakHeapPtrHasher<T> {};

}  // namespace mozilla

namespace js {

class ArrayObject;
class DebugEnvironmentProxy;
class GlobalObject;
class PropertyName;
class Scope;
class ScriptSourceObject;
class Shape;
class BaseShape;
class GetterSetter;
class PropMap;
class WasmInstanceObject;
class WasmTableObject;

namespace jit {
class JitCode;
}  // namespace jit

using PreBarrieredId = PreBarriered<jsid>;
using PreBarrieredObject = PreBarriered<JSObject*>;
using PreBarrieredValue = PreBarriered<Value>;

using GCPtrNativeObject = GCPtr<NativeObject*>;
using GCPtrArrayObject = GCPtr<ArrayObject*>;
using GCPtrAtom = GCPtr<JSAtom*>;
using GCPtrBigInt = GCPtr<BigInt*>;
using GCPtrFunction = GCPtr<JSFunction*>;
using GCPtrLinearString = GCPtr<JSLinearString*>;
using GCPtrObject = GCPtr<JSObject*>;
using GCPtrScript = GCPtr<JSScript*>;
using GCPtrString = GCPtr<JSString*>;
using GCPtrShape = GCPtr<Shape*>;
using GCPtrGetterSetter = GCPtr<GetterSetter*>;
using GCPtrPropMap = GCPtr<PropMap*>;
using GCPtrValue = GCPtr<Value>;
using GCPtrId = GCPtr<jsid>;

using ImmutablePropertyNamePtr = ImmutableTenuredPtr<PropertyName*>;
using ImmutableSymbolPtr = ImmutableTenuredPtr<JS::Symbol*>;

using WeakHeapPtrDebugEnvironmentProxy = WeakHeapPtr<DebugEnvironmentProxy*>;
using WeakHeapPtrGlobalObject = WeakHeapPtr<GlobalObject*>;
using WeakHeapPtrObject = WeakHeapPtr<JSObject*>;
using WeakHeapPtrScript = WeakHeapPtr<JSScript*>;
using WeakHeapPtrScriptSourceObject = WeakHeapPtr<ScriptSourceObject*>;
using WeakHeapPtrShape = WeakHeapPtr<Shape*>;
using WeakHeapPtrJitCode = WeakHeapPtr<jit::JitCode*>;
using WeakHeapPtrSymbol = WeakHeapPtr<JS::Symbol*>;
using WeakHeapPtrWasmInstanceObject = WeakHeapPtr<WasmInstanceObject*>;
using WeakHeapPtrWasmTableObject = WeakHeapPtr<WasmTableObject*>;

using HeapPtrJitCode = HeapPtr<jit::JitCode*>;
using HeapPtrNativeObject = HeapPtr<NativeObject*>;
using HeapPtrObject = HeapPtr<JSObject*>;
using HeapPtrRegExpShared = HeapPtr<RegExpShared*>;
using HeapPtrValue = HeapPtr<Value>;

} /* namespace js */

#endif /* gc_Barrier_h */
