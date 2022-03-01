/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Realm_h
#define vm_Realm_h

#include "mozilla/Atomics.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Tuple.h"
#include "mozilla/Variant.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <stddef.h>

#include "builtin/Array.h"
#include "gc/Barrier.h"
#include "js/GCVariant.h"
#include "js/UniquePtr.h"
#include "vm/ArrayBufferObject.h"
#include "vm/Compartment.h"
#include "vm/NativeObject.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseLookup.h"  // js::PromiseLookup
#include "vm/RegExpShared.h"
#include "vm/SavedStacks.h"
#include "vm/Time.h"
#include "wasm/WasmRealm.h"

namespace js {

namespace coverage {
class LCovRealm;
}  // namespace coverage

namespace jit {
class JitRealm;
}  // namespace jit

class AutoRestoreRealmDebugMode;
class GlobalObject;
class GlobalLexicalEnvironmentObject;
class MapObject;
class NonSyntacticLexicalEnvironmentObject;
class ScriptSourceObject;
class SetObject;
struct NativeIterator;

/*
 * A single-entry cache for some base-10 double-to-string conversions. This
 * helps date-format-xparb.js.  It also avoids skewing the results for
 * v8-splay.js when measured by the SunSpider harness, where the splay tree
 * initialization (which includes many repeated double-to-string conversions)
 * is erroneously included in the measurement; see bug 562553.
 */
class DtoaCache {
  double d;
  int base;
  JSLinearString* s;  // if s==nullptr, d and base are not valid

 public:
  DtoaCache() : s(nullptr) {}
  void purge() { s = nullptr; }

  JSLinearString* lookup(int base, double d) {
    return this->s && base == this->base && d == this->d ? this->s : nullptr;
  }

  void cache(int base, double d, JSLinearString* s) {
    this->base = base;
    this->d = d;
    this->s = s;
  }

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkCacheAfterMovingGC();
#endif
};

// Cache to speed up the group/shape lookup in ProxyObject::create. A proxy's
// shape is only determined by the Class + proto, so a small cache for this is
// very effective in practice.
class NewProxyCache {
  struct Entry {
    Shape* shape;
  };
  static const size_t NumEntries = 4;
  mozilla::UniquePtr<Entry[], JS::FreePolicy> entries_;

 public:
  MOZ_ALWAYS_INLINE bool lookup(const JSClass* clasp, TaggedProto proto,
                                Shape** shape) const {
    if (!entries_) {
      return false;
    }
    for (size_t i = 0; i < NumEntries; i++) {
      const Entry& entry = entries_[i];
      if (entry.shape && entry.shape->getObjectClass() == clasp &&
          entry.shape->proto() == proto) {
        *shape = entry.shape;
        return true;
      }
    }
    return false;
  }
  void add(Shape* shape) {
    MOZ_ASSERT(shape);
    if (!entries_) {
      entries_.reset(js_pod_calloc<Entry>(NumEntries));
      if (!entries_) {
        return;
      }
    } else {
      for (size_t i = NumEntries - 1; i > 0; i--) {
        entries_[i] = entries_[i - 1];
      }
    }
    entries_[0].shape = shape;
  }
  void purge() { entries_.reset(); }
};

// [SMDOC] Object MetadataBuilder API
//
// We must ensure that all newly allocated JSObjects get their metadata
// set. However, metadata builders may require the new object be in a sane
// state (eg, have its reserved slots initialized so they can get the
// sizeOfExcludingThis of the object). Therefore, for objects of certain
// JSClasses (those marked with JSCLASS_DELAY_METADATA_BUILDER), it is not safe
// for the allocation paths to call the object metadata builder
// immediately. Instead, the JSClass-specific "constructor" C++ function up the
// stack makes a promise that it will ensure that the new object has its
// metadata set after the object is initialized.
//
// To help those constructor functions keep their promise of setting metadata,
// each compartment is in one of three states at any given time:
//
// * ImmediateMetadata: Allocators should set new object metadata immediately,
//                      as usual.
//
// * DelayMetadata: Allocators should *not* set new object metadata, it will be
//                  handled after reserved slots are initialized by custom code
//                  for the object's JSClass. The newly allocated object's
//                  JSClass *must* have the JSCLASS_DELAY_METADATA_BUILDER flag
//                  set.
//
// * PendingMetadata: This object has been allocated and is still pending its
//                    metadata. This should never be the case when we begin an
//                    allocation, as a constructor function was supposed to have
//                    set the metadata of the previous object *before*
//                    allocating another object.
//
// The js::AutoSetNewObjectMetadata RAII class provides an ergonomic way for
// constructor functions to navigate state transitions, and its instances
// collectively maintain a stack of previous states. The stack is required to
// support the lazy resolution and allocation of global builtin constructors and
// prototype objects. The initial (and intuitively most common) state is
// ImmediateMetadata.
//
// Without the presence of internal errors (such as OOM), transitions between
// the states are as follows:
//
//     ImmediateMetadata                 .----- previous state on stack
//           |                           |          ^
//           | via constructor           |          |
//           |                           |          | via setting the new
//           |        via constructor    |          | object's metadata
//           |   .-----------------------'          |
//           |   |                                  |
//           V   V                                  |
//     DelayMetadata -------------------------> PendingMetadata
//                         via allocation
//
// In the presence of internal errors, we do not set the new object's metadata
// (if it was even allocated) and reset to the previous state on the stack.

// See below in namespace JS for the template specialization for
// ImmediateMetadata and DelayMetadata.
struct ImmediateMetadata {};
struct DelayMetadata {};
using PendingMetadata = JSObject*;

using NewObjectMetadataState =
    mozilla::Variant<ImmediateMetadata, DelayMetadata, PendingMetadata>;

class MOZ_RAII AutoSetNewObjectMetadata {
  JSContext* cx_;
  Rooted<NewObjectMetadataState> prevState_;

  AutoSetNewObjectMetadata(const AutoSetNewObjectMetadata& aOther) = delete;
  void operator=(const AutoSetNewObjectMetadata& aOther) = delete;

 public:
  explicit AutoSetNewObjectMetadata(JSContext* cx);
  ~AutoSetNewObjectMetadata();
};

class PropertyIteratorObject;

struct IteratorHashPolicy {
  struct Lookup {
    Shape** shapes;
    size_t numShapes;
    HashNumber shapesHash;

    Lookup(Shape** shapes, size_t numShapes, HashNumber shapesHash)
        : shapes(shapes), numShapes(numShapes), shapesHash(shapesHash) {
      MOZ_ASSERT(numShapes > 0);
    }
  };
  static HashNumber hash(const Lookup& lookup) { return lookup.shapesHash; }
  static bool match(PropertyIteratorObject* obj, const Lookup& lookup);
};

class DebugEnvironments;
class ObjectWeakMap;
class WeakMapBase;

// ObjectRealm stores various tables and other state associated with particular
// objects in a realm. To make sure the correct ObjectRealm is used for an
// object, use of the ObjectRealm::get(obj) static method is required.
class ObjectRealm {
  using NativeIteratorSentinel =
      js::UniquePtr<js::NativeIterator, JS::FreePolicy>;
  NativeIteratorSentinel iteratorSentinel_;

  // All non-syntactic lexical environments in the realm. These are kept in a
  // map because when loading scripts into a non-syntactic environment, we
  // need to use the same lexical environment to persist lexical bindings.
  js::UniquePtr<js::ObjectWeakMap> nonSyntacticLexicalEnvironments_;

  ObjectRealm(const ObjectRealm&) = delete;
  void operator=(const ObjectRealm&) = delete;

 public:
  // List of potentially active iterators that may need deleted property
  // suppression.
  js::NativeIterator* enumerators = nullptr;

  // Map from array buffers to views sharing that storage.
  JS::WeakCache<js::InnerViewTable> innerViews;

  // Keep track of the metadata objects which can be associated with each JS
  // object. Both keys and values are in this realm.
  js::UniquePtr<js::ObjectWeakMap> objectMetadataTable;

  using IteratorCache =
      js::HashSet<js::PropertyIteratorObject*, js::IteratorHashPolicy,
                  js::ZoneAllocPolicy>;
  IteratorCache iteratorCache;

  static inline ObjectRealm& get(const JSObject* obj);

  explicit ObjectRealm(JS::Zone* zone);
  ~ObjectRealm();

  [[nodiscard]] bool init(JSContext* cx);

  void finishRoots();
  void trace(JSTracer* trc);
  void sweepAfterMinorGC();
  void traceWeakNativeIterators(JSTracer* trc);

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* innerViewsArg,
                              size_t* objectMetadataTablesArg,
                              size_t* nonSyntacticLexicalEnvironmentsArg);

  MOZ_ALWAYS_INLINE bool objectMaybeInIteration(JSObject* obj);

  js::NonSyntacticLexicalEnvironmentObject*
  getOrCreateNonSyntacticLexicalEnvironment(JSContext* cx,
                                            js::HandleObject enclosing);
  js::NonSyntacticLexicalEnvironmentObject*
  getOrCreateNonSyntacticLexicalEnvironment(JSContext* cx,
                                            js::HandleObject enclosing,
                                            js::HandleObject key,
                                            js::HandleObject thisv);
  js::NonSyntacticLexicalEnvironmentObject* getNonSyntacticLexicalEnvironment(
      JSObject* key) const;
};

}  // namespace js

namespace JS {
template <>
struct GCPolicy<js::ImmediateMetadata>
    : public IgnoreGCPolicy<js::ImmediateMetadata> {};
template <>
struct GCPolicy<js::DelayMetadata> : public IgnoreGCPolicy<js::DelayMetadata> {
};
}  // namespace JS

class JS::Realm : public JS::shadow::Realm {
  JS::Zone* zone_;
  JSRuntime* runtime_;

  const JS::RealmCreationOptions creationOptions_;
  JS::RealmBehaviors behaviors_;

  friend struct ::JSContext;
  js::WeakHeapPtrGlobalObject global_;

  // The global lexical environment. This is stored here instead of in
  // GlobalObject for easier/faster JIT access.
  js::WeakHeapPtr<js::GlobalLexicalEnvironmentObject*> lexicalEnv_;

  // Note: this is private to enforce use of ObjectRealm::get(obj).
  js::ObjectRealm objects_;
  friend js::ObjectRealm& js::ObjectRealm::get(const JSObject*);

  // The global environment record's [[VarNames]] list that contains all
  // names declared using FunctionDeclaration, GeneratorDeclaration, and
  // VariableDeclaration declarations in global code in this realm.
  // Names are only removed from this list by a |delete IdentifierReference|
  // that successfully removes that global property.
  using VarNamesSet =
      GCHashSet<js::HeapPtr<JSAtom*>, js::DefaultHasher<JSAtom*>,
                js::ZoneAllocPolicy>;
  VarNamesSet varNames_;

  friend class js::AutoSetNewObjectMetadata;
  js::NewObjectMetadataState objectMetadataState_{js::ImmediateMetadata()};

  // Random number generator for Math.random().
  mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG>
      randomNumberGenerator_;

  // Random number generator for randomHashCodeScrambler().
  mozilla::non_crypto::XorShift128PlusRNG randomKeyGenerator_;

  JSPrincipals* principals_ = nullptr;

  js::UniquePtr<js::jit::JitRealm> jitRealm_;

  // Bookkeeping information for debug scope objects.
  js::UniquePtr<js::DebugEnvironments> debugEnvs_;

  js::SavedStacks savedStacks_;

  // Used by memory reporters and invalid otherwise.
  JS::RealmStats* realmStats_ = nullptr;

  const js::AllocationMetadataBuilder* allocationMetadataBuilder_ = nullptr;
  void* realmPrivate_ = nullptr;

  js::WeakHeapPtr<js::ArgumentsObject*> mappedArgumentsTemplate_{nullptr};
  js::WeakHeapPtr<js::ArgumentsObject*> unmappedArgumentsTemplate_{nullptr};
  js::WeakHeapPtr<js::PlainObject*> iterResultTemplate_{nullptr};
  js::WeakHeapPtr<js::PlainObject*> iterResultWithoutPrototypeTemplate_{
      nullptr};

  // There are two ways to enter a realm:
  //
  // (1) AutoRealm (and JSAutoRealm, JS::EnterRealm)
  // (2) When calling a cross-realm (but same-compartment) function in JIT
  //     code.
  //
  // This field only accounts for (1), to keep the JIT code as simple as
  // possible.
  //
  // An important invariant is that the JIT can only switch to a different
  // realm within the same compartment, so whenever that happens there must
  // always be a same-compartment realm with enterRealmDepthIgnoringJit_ > 0.
  // This lets us set Compartment::hasEnteredRealm without walking the
  // stack.
  unsigned enterRealmDepthIgnoringJit_ = 0;

 public:
  // Various timers for collecting time spent delazifying, jit compiling,
  // executing, etc
  JS::JSTimers timers;

  struct DebuggerVectorEntry {
    // The debugger relies on iterating through the DebuggerVector to know what
    // debuggers to notify about certain actions, which it does using this
    // pointer. We need an explicit Debugger* because the JSObject* from
    // the DebuggerDebuggeeLink to the Debugger is only set some of the time.
    // This `Debugger*` pointer itself could also live on the
    // DebuggerDebuggeeLink itself, but that would then require all of the
    // places that iterate over the realm's DebuggerVector to also traverse
    // the CCW which seems like it would be needlessly complicated.
    js::WeakHeapPtr<js::Debugger*> dbg;

    // This links to the debugger's DebuggerDebuggeeLink object, via a CCW.
    // Tracing this link from the realm allows the debugger to define
    // whether pieces of the debugger should be held live by a given realm.
    js::HeapPtr<JSObject*> debuggerLink;

    DebuggerVectorEntry(js::Debugger* dbg_, JSObject* link);
  };
  using DebuggerVector =
      js::Vector<DebuggerVectorEntry, 0, js::ZoneAllocPolicy>;

 private:
  DebuggerVector debuggers_;

  enum {
    IsDebuggee = 1 << 0,
    DebuggerObservesAllExecution = 1 << 1,
    DebuggerObservesAsmJS = 1 << 2,
    DebuggerObservesCoverage = 1 << 3,
  };
  unsigned debugModeBits_ = 0;
  friend class js::AutoRestoreRealmDebugMode;

  bool isSelfHostingRealm_ = false;
  bool isSystem_ = false;

  js::UniquePtr<js::coverage::LCovRealm> lcovRealm_ = nullptr;

 public:
  // WebAssembly state for the realm.
  js::wasm::Realm wasm;

  js::RegExpRealm regExps;

  js::DtoaCache dtoaCache;
  js::NewProxyCache newProxyCache;
  js::ArraySpeciesLookup arraySpeciesLookup;
  js::PromiseLookup promiseLookup;

  /*
   * Lazily initialized script source object to use for scripts cloned
   * from the self-hosting global.
   */
  js::WeakHeapPtrScriptSourceObject selfHostingScriptSource{nullptr};

  // Last time at which an animation was played for this realm.
  js::MainThreadData<mozilla::TimeStamp> lastAnimationTime;

  /*
   * For generational GC, record whether a write barrier has added this
   * realm's global to the store buffer since the last minor GC.
   *
   * This is used to avoid calling into the VM every time a nursery object is
   * written to a property of the global.
   */
  uint32_t globalWriteBarriered = 0;

#ifdef DEBUG
  bool firedOnNewGlobalObject = false;
#endif

  // True if all incoming wrappers have been nuked. This happens when
  // NukeCrossCompartmentWrappers is called with the NukeAllReferences option.
  // This prevents us from creating new wrappers for the compartment.
  bool nukedIncomingWrappers = false;

 private:
  void updateDebuggerObservesFlag(unsigned flag);

  Realm(const Realm&) = delete;
  void operator=(const Realm&) = delete;

 public:
  Realm(JS::Compartment* comp, const JS::RealmOptions& options);
  ~Realm();

  [[nodiscard]] bool init(JSContext* cx, JSPrincipals* principals);
  void destroy(JSFreeOp* fop);
  void clearTables();

  void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* realmObject, size_t* realmTables,
                              size_t* innerViewsArg,
                              size_t* objectMetadataTablesArg,
                              size_t* savedStacksSet, size_t* varNamesSet,
                              size_t* nonSyntacticLexicalEnvironmentsArg,
                              size_t* jitRealm);

  JS::Zone* zone() { return zone_; }
  const JS::Zone* zone() const { return zone_; }

  JSRuntime* runtimeFromMainThread() const {
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
    return runtime_;
  }

  // Note: Unrestricted access to the runtime from an arbitrary thread
  // can easily lead to races. Use this method very carefully.
  JSRuntime* runtimeFromAnyThread() const { return runtime_; }

  const JS::RealmCreationOptions& creationOptions() const {
    return creationOptions_;
  }

  // NOTE: Do not provide accessor for mutable reference.
  // Modifying RealmBehaviors after creating a realm can result in
  // inconsistency.
  const JS::RealmBehaviors& behaviors() const { return behaviors_; }

  void setNonLive() { behaviors_.setNonLive(); }

  /* Whether to preserve JIT code on non-shrinking GCs. */
  bool preserveJitCode() { return creationOptions_.preserveJitCode(); }

  bool isSelfHostingRealm() const { return isSelfHostingRealm_; }
  void setIsSelfHostingRealm();

  /* The global object for this realm.
   *
   * Note: the global_ field is null briefly during GC, after the global
   * object is collected; but when that happens the Realm is destroyed during
   * the same GC.)
   *
   * In contrast, JSObject::global() is infallible because marking a JSObject
   * always marks its global as well.
   */
  inline js::GlobalObject* maybeGlobal() const;

  /* An unbarriered getter for use while tracing. */
  js::GlobalObject* unsafeUnbarrieredMaybeGlobal() const {
    return global_.unbarrieredGet();
  }

  inline js::GlobalLexicalEnvironmentObject* unbarrieredLexicalEnvironment()
      const;

  /* True if a global object exists, but it's being collected. */
  inline bool globalIsAboutToBeFinalized();

  /* True if a global exists and it's not being collected. */
  inline bool hasLiveGlobal() const;

  inline void initGlobal(js::GlobalObject& global,
                         js::GlobalLexicalEnvironmentObject& lexicalEnv);

  /*
   * This method traces data that is live iff we know that this realm's
   * global is still live.
   */
  void traceGlobal(JSTracer* trc);

  void traceWeakObjects(JSTracer* trc);
  void fixupGlobal();

  /*
   * This method traces Realm-owned GC roots that are considered live
   * regardless of whether the realm's global is still live.
   */
  void traceRoots(JSTracer* trc,
                  js::gc::GCRuntime::TraceOrMarkRuntime traceOrMark);
  /*
   * This method clears out tables of roots in preparation for the final GC.
   */
  void finishRoots();

  void sweepAfterMinorGC();
  void sweepDebugEnvironments();
  void traceWeakObjectRealm(JSTracer* trc);
  void traceWeakRegExps(JSTracer* trc);
  void traceWeakSelfHostingScriptSource(JSTracer* trc);
  void traceWeakTemplateObjects(JSTracer* trc);

  void clearScriptCounts();
  void clearScriptLCov();

  void purge();

  void fixupAfterMovingGC(JSTracer* trc);

  // Add a name to [[VarNames]].  Reports OOM on failure.
  [[nodiscard]] bool addToVarNames(JSContext* cx, JS::Handle<JSAtom*> name);
  void tracekWeakVarNames(JSTracer* trc);

  void removeFromVarNames(JS::Handle<JSAtom*> name) { varNames_.remove(name); }

  // Whether the given name is in [[VarNames]].
  bool isInVarNames(JS::Handle<JSAtom*> name) { return varNames_.has(name); }

  void enter() { enterRealmDepthIgnoringJit_++; }
  void leave() {
    MOZ_ASSERT(enterRealmDepthIgnoringJit_ > 0);
    enterRealmDepthIgnoringJit_--;
  }
  bool hasBeenEnteredIgnoringJit() const {
    return enterRealmDepthIgnoringJit_ > 0;
  }
  bool shouldTraceGlobal() const {
    // If we entered this realm in JIT code, there must be a script and
    // function on the stack for this realm, so the global will definitely
    // be traced and it's safe to return false here.
    return hasBeenEnteredIgnoringJit();
  }

  bool hasAllocationMetadataBuilder() const {
    return allocationMetadataBuilder_;
  }
  const js::AllocationMetadataBuilder* getAllocationMetadataBuilder() const {
    return allocationMetadataBuilder_;
  }
  const void* addressOfMetadataBuilder() const {
    return &allocationMetadataBuilder_;
  }
  bool isRecordingAllocations();
  void setAllocationMetadataBuilder(
      const js::AllocationMetadataBuilder* builder);
  void forgetAllocationMetadataBuilder();
  void setNewObjectMetadata(JSContext* cx, JS::HandleObject obj);

  bool hasObjectPendingMetadata() const {
    return objectMetadataState_.is<js::PendingMetadata>();
  }
  void setObjectPendingMetadata(JSContext* cx, JSObject* obj) {
    if (!cx->isHelperThreadContext()) {
      MOZ_ASSERT(objectMetadataState_.is<js::DelayMetadata>());
      objectMetadataState_ =
          js::NewObjectMetadataState(js::PendingMetadata(obj));
    }
  }

  void* realmPrivate() const { return realmPrivate_; }
  void setRealmPrivate(void* p) { realmPrivate_ = p; }

  // This should only be called when it is non-null, i.e. during memory
  // reporting.
  JS::RealmStats& realmStats() {
    // We use MOZ_RELEASE_ASSERT here because in bug 1132502 there was some
    // (inconclusive) evidence that realmStats_ can be nullptr unexpectedly.
    MOZ_RELEASE_ASSERT(realmStats_);
    return *realmStats_;
  }
  void nullRealmStats() {
    MOZ_ASSERT(realmStats_);
    realmStats_ = nullptr;
  }
  void setRealmStats(JS::RealmStats* newStats) {
    MOZ_ASSERT(!realmStats_ && newStats);
    realmStats_ = newStats;
  }

  inline bool marked() const;

  /*
   * The principals associated with this realm. Note that the same several
   * realms may share the same principals and that a realm may change
   * principals during its lifetime (e.g. in case of lazy parsing).
   */
  JSPrincipals* principals() { return principals_; }
  void setPrincipals(JSPrincipals* principals) { principals_ = principals; }

  bool isSystem() const { return isSystem_; }

  static const size_t IterResultObjectValueSlot = 0;
  static const size_t IterResultObjectDoneSlot = 1;
  js::PlainObject* getOrCreateIterResultTemplateObject(JSContext* cx);
  js::PlainObject* getOrCreateIterResultWithoutPrototypeTemplateObject(
      JSContext* cx);

 private:
  enum class WithObjectPrototype { No, Yes };
  js::PlainObject* createIterResultTemplateObject(
      JSContext* cx, WithObjectPrototype withProto);

 public:
  js::ArgumentsObject* getOrCreateArgumentsTemplateObject(JSContext* cx,
                                                          bool mapped);
  js::ArgumentsObject* maybeArgumentsTemplateObject(bool mapped) const;

  //
  // The Debugger observes execution on a frame-by-frame basis. The
  // invariants of Realm's debug mode bits, JSScript::isDebuggee,
  // InterpreterFrame::isDebuggee, and BaselineFrame::isDebuggee are
  // enumerated below.
  //
  // 1. When a realm's isDebuggee() == true, relazification and lazy
  //    parsing are disabled.
  //
  //    Whether AOT wasm is disabled is togglable by the Debugger API. By
  //    default it is disabled. See debuggerObservesAsmJS below.
  //
  // 2. When a realm's debuggerObservesAllExecution() == true, all of
  //    the realm's scripts are considered debuggee scripts.
  //
  // 3. A script is considered a debuggee script either when, per above, its
  //    realm is observing all execution, or if it has breakpoints set.
  //
  // 4. A debuggee script always pushes a debuggee frame.
  //
  // 5. A debuggee frame calls all slow path Debugger hooks in the
  //    Interpreter and Baseline. A debuggee frame implies that its script's
  //    BaselineScript, if extant, has been compiled with debug hook calls.
  //
  // 6. A debuggee script or a debuggee frame (i.e., during OSR) ensures
  //    that the compiled BaselineScript is compiled with debug hook calls
  //    when attempting to enter Baseline.
  //
  // 7. A debuggee script or a debuggee frame (i.e., during OSR) does not
  //    attempt to enter Ion.
  //
  // Note that a debuggee frame may exist without its script being a
  // debuggee script. e.g., Debugger.Frame.prototype.eval only marks the
  // frame in which it is evaluating as a debuggee frame.
  //

  // True if this realm's global is a debuggee of some Debugger
  // object.
  bool isDebuggee() const { return !!(debugModeBits_ & IsDebuggee); }

  void setIsDebuggee();
  void unsetIsDebuggee();

  DebuggerVector& getDebuggers() { return debuggers_; };

  // True if this compartment's global is a debuggee of some Debugger
  // object with a live hook that observes all execution; e.g.,
  // onEnterFrame.
  bool debuggerObservesAllExecution() const {
    static const unsigned Mask = IsDebuggee | DebuggerObservesAllExecution;
    return (debugModeBits_ & Mask) == Mask;
  }
  void updateDebuggerObservesAllExecution() {
    updateDebuggerObservesFlag(DebuggerObservesAllExecution);
  }

  // True if this realm's global is a debuggee of some Debugger object
  // whose allowUnobservedAsmJS flag is false.
  //
  // Note that since AOT wasm functions cannot bail out, this flag really
  // means "observe wasm from this point forward". We cannot make
  // already-compiled wasm code observable to Debugger.
  bool debuggerObservesAsmJS() const {
    static const unsigned Mask = IsDebuggee | DebuggerObservesAsmJS;
    return (debugModeBits_ & Mask) == Mask;
  }
  void updateDebuggerObservesAsmJS() {
    updateDebuggerObservesFlag(DebuggerObservesAsmJS);
  }

  // True if this realm's global is a debuggee of some Debugger object
  // whose collectCoverageInfo flag is true.
  bool debuggerObservesCoverage() const {
    static const unsigned Mask = DebuggerObservesCoverage;
    return (debugModeBits_ & Mask) == Mask;
  }
  void updateDebuggerObservesCoverage();

  // Returns true if the Debugger API is collecting code coverage data for this
  // realm or if the process-wide LCov option is enabled.
  bool collectCoverageForDebug() const;

  // Get or allocate the associated LCovRealm.
  js::coverage::LCovRealm* lcovRealm();

  // Initializes randomNumberGenerator if needed.
  mozilla::non_crypto::XorShift128PlusRNG& getOrCreateRandomNumberGenerator();

  const mozilla::non_crypto::XorShift128PlusRNG*
  addressOfRandomNumberGenerator() const {
    return randomNumberGenerator_.ptr();
  }

  mozilla::HashCodeScrambler randomHashCodeScrambler();

  bool ensureJitRealmExists(JSContext* cx);
  void traceWeakEdgesInJitRealm(JSTracer* trc);

  js::jit::JitRealm* jitRealm() { return jitRealm_.get(); }

  js::DebugEnvironments* debugEnvs() { return debugEnvs_.get(); }
  js::UniquePtr<js::DebugEnvironments>& debugEnvsRef() { return debugEnvs_; }

  js::SavedStacks& savedStacks() { return savedStacks_; }

  // Recompute the probability with which this realm should record
  // profiling data (stack traces, allocations log, etc.) about each
  // allocation. We first consult the JS runtime to see if it is recording
  // allocations, and if not then check the probabilities requested by the
  // Debugger instances observing us, if any.
  void chooseAllocationSamplingProbability() {
    savedStacks_.chooseSamplingProbability(this);
  }

  void traceWeakSavedStacks(JSTracer* trc);

  static constexpr size_t offsetOfCompartment() {
    return offsetof(JS::Realm, compartment_);
  }
  static constexpr size_t offsetOfRegExps() {
    return offsetof(JS::Realm, regExps);
  }
  static constexpr size_t offsetOfDebugModeBits() {
    return offsetof(JS::Realm, debugModeBits_);
  }
  static constexpr uint32_t debugModeIsDebuggeeBit() { return IsDebuggee; }

  static constexpr size_t offsetOfActiveLexicalEnvironment() {
    static_assert(sizeof(lexicalEnv_) == sizeof(uintptr_t),
                  "JIT code assumes field is pointer-sized");
    return offsetof(JS::Realm, lexicalEnv_);
  }
};

inline js::Handle<js::GlobalObject*> JSContext::global() const {
  /*
   * It's safe to use |unbarrieredGet()| here because any realm that is on-stack
   * will be marked automatically, so there's no need for a read barrier on
   * it. Once the realm is popped, the handle is no longer safe to use.
   */
  MOZ_ASSERT(realm_, "Caller needs to enter a realm first");
  return js::Handle<js::GlobalObject*>::fromMarkedLocation(
      realm_->global_.unbarrieredAddress());
}

namespace js {

class MOZ_RAII AssertRealmUnchanged {
 public:
  explicit AssertRealmUnchanged(JSContext* cx)
      : cx(cx), oldRealm(cx->realm()) {}

  ~AssertRealmUnchanged() { MOZ_ASSERT(cx->realm() == oldRealm); }

 protected:
  JSContext* const cx;
  JS::Realm* const oldRealm;
};

// AutoRealm can be used to enter the realm of a JSObject, JSScript or
// ObjectGroup. It must not be used with cross-compartment wrappers, because
// CCWs are not associated with a single realm.
class AutoRealm {
  JSContext* const cx_;
  JS::Realm* const origin_;

 public:
  template <typename T>
  inline AutoRealm(JSContext* cx, const T& target);
  inline ~AutoRealm();

  JSContext* context() const { return cx_; }
  JS::Realm* origin() const { return origin_; }

 protected:
  inline AutoRealm(JSContext* cx, JS::Realm* target);

 private:
  AutoRealm(const AutoRealm&) = delete;
  AutoRealm& operator=(const AutoRealm&) = delete;
};

class MOZ_RAII AutoAllocInAtomsZone {
  JSContext* const cx_;
  JS::Realm* const origin_;
  AutoAllocInAtomsZone(const AutoAllocInAtomsZone&) = delete;
  AutoAllocInAtomsZone& operator=(const AutoAllocInAtomsZone&) = delete;

 public:
  inline explicit AutoAllocInAtomsZone(JSContext* cx);
  inline ~AutoAllocInAtomsZone();
};

// During GC we sometimes need to enter a realm when we may have been allocating
// in the the atoms zone. This leaves the atoms zone temporarily. This happens
// in embedding callbacks and when we need to mark object groups as pretenured.
class MOZ_RAII AutoMaybeLeaveAtomsZone {
  JSContext* const cx_;
  bool wasInAtomsZone_;
  AutoMaybeLeaveAtomsZone(const AutoMaybeLeaveAtomsZone&) = delete;
  AutoMaybeLeaveAtomsZone& operator=(const AutoMaybeLeaveAtomsZone&) = delete;

 public:
  inline explicit AutoMaybeLeaveAtomsZone(JSContext* cx);
  inline ~AutoMaybeLeaveAtomsZone();
};

// Enter a realm directly. Only use this where there's no target GC thing
// to pass to AutoRealm or where you need to avoid the assertions in
// JS::Compartment::enterCompartmentOf().
class AutoRealmUnchecked : protected AutoRealm {
 public:
  inline AutoRealmUnchecked(JSContext* cx, JS::Realm* target);
};

/*
 * Use this to change the behavior of an AutoRealm slightly on error. If
 * the exception happens to be an Error object, copy it to the origin
 * compartment instead of wrapping it.
 */
class ErrorCopier {
  mozilla::Maybe<AutoRealm>& ar;

 public:
  explicit ErrorCopier(mozilla::Maybe<AutoRealm>& ar) : ar(ar) {}
  ~ErrorCopier();
};

} /* namespace js */

#endif /* vm_Realm_h */
