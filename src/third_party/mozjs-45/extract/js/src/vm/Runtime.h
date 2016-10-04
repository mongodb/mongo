/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Runtime_h
#define vm_Runtime_h

#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/LinkedList.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Scoped.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include <setjmp.h>

#include "jsatom.h"
#include "jsclist.h"
#include "jsscript.h"

#ifdef XP_DARWIN
# include "asmjs/AsmJSSignalHandlers.h"
#endif
#include "builtin/AtomicsObject.h"
#include "ds/FixedSizeHash.h"
#include "frontend/ParseMaps.h"
#include "gc/GCRuntime.h"
#include "gc/Tracer.h"
#include "irregexp/RegExpStack.h"
#include "js/Debug.h"
#include "js/HashTable.h"
#ifdef DEBUG
# include "js/Proxy.h" // For AutoEnterPolicy
#endif
#include "js/TraceableVector.h"
#include "js/Vector.h"
#include "vm/CodeCoverage.h"
#include "vm/CommonPropertyNames.h"
#include "vm/DateTime.h"
#include "vm/MallocProvider.h"
#include "vm/SPSProfiler.h"
#include "vm/Stack.h"
#include "vm/Stopwatch.h"
#include "vm/Symbol.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100) /* Silence unreferenced formal parameter warnings */
#endif

namespace js {

class PerThreadData;
class ExclusiveContext;
class AutoKeepAtoms;
#ifdef JS_TRACE_LOGGING
class TraceLoggerThread;
#endif

/* Thread Local Storage slot for storing the runtime for a thread. */
extern mozilla::ThreadLocal<PerThreadData*> TlsPerThreadData;

} // namespace js

struct DtoaState;

#ifdef JS_SIMULATOR_ARM64
namespace vixl {
class Simulator;
}
#endif

namespace js {

extern MOZ_COLD void
ReportOutOfMemory(ExclusiveContext* cx);

extern MOZ_COLD void
ReportAllocationOverflow(ExclusiveContext* maybecx);

extern MOZ_COLD void
ReportOverRecursed(ExclusiveContext* cx);

class Activation;
class ActivationIterator;
class AsmJSActivation;
class AsmJSModule;
class MathCache;

namespace jit {
class JitRuntime;
class JitActivation;
struct PcScriptCache;
struct AutoFlushICache;
class CompileRuntime;

#ifdef JS_SIMULATOR_ARM64
typedef vixl::Simulator Simulator;
#elif defined(JS_SIMULATOR)
class Simulator;
#endif
} // namespace jit

/*
 * GetSrcNote cache to avoid O(n^2) growth in finding a source note for a
 * given pc in a script. We use the script->code pointer to tag the cache,
 * instead of the script address itself, so that source notes are always found
 * by offset from the bytecode with which they were generated.
 */
struct GSNCache {
    typedef HashMap<jsbytecode*,
                    jssrcnote*,
                    PointerHasher<jsbytecode*, 0>,
                    SystemAllocPolicy> Map;

    jsbytecode*     code;
    Map             map;

    GSNCache() : code(nullptr) { }

    void purge();
};

/*
 * ScopeCoordinateName cache to avoid O(n^2) growth in finding the name
 * associated with a given aliasedvar operation.
 */
struct ScopeCoordinateNameCache {
    typedef HashMap<uint32_t,
                    jsid,
                    DefaultHasher<uint32_t>,
                    SystemAllocPolicy> Map;

    Shape* shape;
    Map map;

    ScopeCoordinateNameCache() : shape(nullptr) {}
    void purge();
};

using ScriptAndCountsVector = TraceableVector<ScriptAndCounts, 0, SystemAllocPolicy>;

struct EvalCacheEntry
{
    JSLinearString* str;
    JSScript* script;
    JSScript* callerScript;
    jsbytecode* pc;
};

struct EvalCacheLookup
{
    explicit EvalCacheLookup(JSContext* cx) : str(cx), callerScript(cx) {}
    RootedLinearString str;
    RootedScript callerScript;
    JSVersion version;
    jsbytecode* pc;
};

struct EvalCacheHashPolicy
{
    typedef EvalCacheLookup Lookup;

    static HashNumber hash(const Lookup& l);
    static bool match(const EvalCacheEntry& entry, const EvalCacheLookup& l);
};

typedef HashSet<EvalCacheEntry, EvalCacheHashPolicy, SystemAllocPolicy> EvalCache;

struct LazyScriptHashPolicy
{
    struct Lookup {
        JSContext* cx;
        LazyScript* lazy;

        Lookup(JSContext* cx, LazyScript* lazy)
          : cx(cx), lazy(lazy)
        {}
    };

    static const size_t NumHashes = 3;

    static void hash(const Lookup& lookup, HashNumber hashes[NumHashes]);
    static bool match(JSScript* script, const Lookup& lookup);

    // Alternate methods for use when removing scripts from the hash without an
    // explicit LazyScript lookup.
    static void hash(JSScript* script, HashNumber hashes[NumHashes]);
    static bool match(JSScript* script, JSScript* lookup) { return script == lookup; }

    static void clear(JSScript** pscript) { *pscript = nullptr; }
    static bool isCleared(JSScript* script) { return !script; }
};

typedef FixedSizeHashSet<JSScript*, LazyScriptHashPolicy, 769> LazyScriptCache;

class PropertyIteratorObject;

class NativeIterCache
{
    static const size_t SIZE = size_t(1) << 8;

    /* Cached native iterators. */
    PropertyIteratorObject* data[SIZE];

    static size_t getIndex(uint32_t key) {
        return size_t(key) % SIZE;
    }

  public:
    /* Native iterator most recently started. */
    PropertyIteratorObject* last;

    NativeIterCache()
      : last(nullptr)
    {
        mozilla::PodArrayZero(data);
    }

    void purge() {
        last = nullptr;
        mozilla::PodArrayZero(data);
    }

    PropertyIteratorObject* get(uint32_t key) const {
        return data[getIndex(key)];
    }

    void set(uint32_t key, PropertyIteratorObject* iterobj) {
        data[getIndex(key)] = iterobj;
    }
};

/*
 * Cache for speeding up repetitive creation of objects in the VM.
 * When an object is created which matches the criteria in the 'key' section
 * below, an entry is filled with the resulting object.
 */
class NewObjectCache
{
    /* Statically asserted to be equal to sizeof(JSObject_Slots16) */
    static const unsigned MAX_OBJ_SIZE = 4 * sizeof(void*) + 16 * sizeof(Value);

    static void staticAsserts() {
        JS_STATIC_ASSERT(NewObjectCache::MAX_OBJ_SIZE == sizeof(JSObject_Slots16));
        JS_STATIC_ASSERT(gc::AllocKind::OBJECT_LAST == gc::AllocKind::OBJECT16_BACKGROUND);
    }

    struct Entry
    {
        /* Class of the constructed object. */
        const Class* clasp;

        /*
         * Key with one of three possible values:
         *
         * - Global for the object. The object must have a standard class for
         *   which the global's prototype can be determined, and the object's
         *   parent will be the global.
         *
         * - Prototype for the object (cannot be global). The object's parent
         *   will be the prototype's parent.
         *
         * - Type for the object. The object's parent will be the type's
         *   prototype's parent.
         */
        gc::Cell* key;

        /* Allocation kind for the constructed object. */
        gc::AllocKind kind;

        /* Number of bytes to copy from the template object. */
        uint32_t nbytes;

        /*
         * Template object to copy from, with the initial values of fields,
         * fixed slots (undefined) and private data (nullptr).
         */
        char templateObject[MAX_OBJ_SIZE];
    };

    Entry entries[41];  // TODO: reconsider size

  public:

    typedef int EntryIndex;

    NewObjectCache() { mozilla::PodZero(this); }
    void purge() { mozilla::PodZero(this); }

    /* Remove any cached items keyed on moved objects. */
    void clearNurseryObjects(JSRuntime* rt);

    /*
     * Get the entry index for the given lookup, return whether there was a hit
     * on an existing entry.
     */
    inline bool lookupProto(const Class* clasp, JSObject* proto, gc::AllocKind kind, EntryIndex* pentry);
    inline bool lookupGlobal(const Class* clasp, js::GlobalObject* global, gc::AllocKind kind,
                             EntryIndex* pentry);

    bool lookupGroup(js::ObjectGroup* group, gc::AllocKind kind, EntryIndex* pentry) {
        return lookup(group->clasp(), group, kind, pentry);
    }

    /*
     * Return a new object from a cache hit produced by a lookup method, or
     * nullptr if returning the object could possibly trigger GC (does not
     * indicate failure).
     */
    inline NativeObject* newObjectFromHit(JSContext* cx, EntryIndex entry, js::gc::InitialHeap heap);

    /* Fill an entry after a cache miss. */
    void fillProto(EntryIndex entry, const Class* clasp, js::TaggedProto proto,
                   gc::AllocKind kind, NativeObject* obj);

    inline void fillGlobal(EntryIndex entry, const Class* clasp, js::GlobalObject* global,
                           gc::AllocKind kind, NativeObject* obj);

    void fillGroup(EntryIndex entry, js::ObjectGroup* group, gc::AllocKind kind,
                   NativeObject* obj)
    {
        MOZ_ASSERT(obj->group() == group);
        return fill(entry, group->clasp(), group, kind, obj);
    }

    /* Invalidate any entries which might produce an object with shape/proto. */
    void invalidateEntriesForShape(JSContext* cx, HandleShape shape, HandleObject proto);

  private:
    EntryIndex makeIndex(const Class* clasp, gc::Cell* key, gc::AllocKind kind) {
        uintptr_t hash = (uintptr_t(clasp) ^ uintptr_t(key)) + size_t(kind);
        return hash % mozilla::ArrayLength(entries);
    }

    bool lookup(const Class* clasp, gc::Cell* key, gc::AllocKind kind, EntryIndex* pentry) {
        *pentry = makeIndex(clasp, key, kind);
        Entry* entry = &entries[*pentry];

        /* N.B. Lookups with the same clasp/key but different kinds map to different entries. */
        return entry->clasp == clasp && entry->key == key;
    }

    void fill(EntryIndex entry_, const Class* clasp, gc::Cell* key, gc::AllocKind kind,
              NativeObject* obj) {
        MOZ_ASSERT(unsigned(entry_) < mozilla::ArrayLength(entries));
        MOZ_ASSERT(entry_ == makeIndex(clasp, key, kind));
        Entry* entry = &entries[entry_];

        entry->clasp = clasp;
        entry->key = key;
        entry->kind = kind;

        entry->nbytes = gc::Arena::thingSize(kind);
        js_memcpy(&entry->templateObject, obj, entry->nbytes);
    }

    static void copyCachedToObject(NativeObject* dst, NativeObject* src, gc::AllocKind kind) {
        js_memcpy(dst, src, gc::Arena::thingSize(kind));
        Shape::writeBarrierPost(&dst->shape_, nullptr, dst->shape_);
        ObjectGroup::writeBarrierPost(&dst->group_, nullptr, dst->group_);
    }
};

/*
 * A FreeOp can do one thing: free memory. For convenience, it has delete_
 * convenience methods that also call destructors.
 *
 * FreeOp is passed to finalizers and other sweep-phase hooks so that we do not
 * need to pass a JSContext to those hooks.
 */
class FreeOp : public JSFreeOp
{
    Vector<void*, 0, SystemAllocPolicy> freeLaterList;
    ThreadType threadType;

  public:
    static FreeOp* get(JSFreeOp* fop) {
        return static_cast<FreeOp*>(fop);
    }

    explicit FreeOp(JSRuntime* rt, ThreadType thread = MainThread)
      : JSFreeOp(rt), threadType(thread)
    {}

    ~FreeOp() {
        for (size_t i = 0; i < freeLaterList.length(); i++)
            free_(freeLaterList[i]);
    }

    bool onBackgroundThread() {
        return threadType == BackgroundThread;
    }

    inline void free_(void* p);
    inline void freeLater(void* p);

    template <class T>
    inline void delete_(T* p) {
        if (p) {
            p->~T();
            free_(p);
        }
    }
};

} /* namespace js */

namespace JS {
struct RuntimeSizes;
} // namespace JS

/* Various built-in or commonly-used names pinned on first context. */
struct JSAtomState
{
#define PROPERTYNAME_FIELD(idpart, id, text) js::ImmutablePropertyNamePtr id;
    FOR_EACH_COMMON_PROPERTYNAME(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD
#define PROPERTYNAME_FIELD(name, code, init, clasp) js::ImmutablePropertyNamePtr name;
    JS_FOR_EACH_PROTOTYPE(PROPERTYNAME_FIELD)
#undef PROPERTYNAME_FIELD

    js::ImmutablePropertyNamePtr* wellKnownSymbolDescriptions() {
        return &Symbol_iterator;
    }
};

namespace js {

/*
 * Storage for well-known symbols. It's a separate struct from the Runtime so
 * that it can be shared across multiple runtimes. As in JSAtomState, each
 * field is a smart pointer that's immutable once initialized.
 * `rt->wellKnownSymbols->iterator` is convertible to Handle<Symbol*>.
 *
 * Well-known symbols are never GC'd. The description() of each well-known
 * symbol is a permanent atom.
 */
struct WellKnownSymbols
{
    js::ImmutableSymbolPtr iterator;
    js::ImmutableSymbolPtr match;
    js::ImmutableSymbolPtr species;
    js::ImmutableSymbolPtr toPrimitive;

    const ImmutableSymbolPtr& get(size_t u) const {
        MOZ_ASSERT(u < JS::WellKnownSymbolLimit);
        const ImmutableSymbolPtr* symbols = reinterpret_cast<const ImmutableSymbolPtr*>(this);
        return symbols[u];
    }

    const ImmutableSymbolPtr& get(JS::SymbolCode code) const {
        return get(size_t(code));
    }
};

#define NAME_OFFSET(name)       offsetof(JSAtomState, name)

inline HandlePropertyName
AtomStateOffsetToName(const JSAtomState& atomState, size_t offset)
{
    return *reinterpret_cast<js::ImmutablePropertyNamePtr*>((char*)&atomState + offset);
}

// There are several coarse locks in the enum below. These may be either
// per-runtime or per-process. When acquiring more than one of these locks,
// the acquisition must be done in the order below to avoid deadlocks.
enum RuntimeLock {
    ExclusiveAccessLock,
    HelperThreadStateLock,
    GCLock
};

#ifdef DEBUG
void AssertCurrentThreadCanLock(RuntimeLock which);
#else
inline void AssertCurrentThreadCanLock(RuntimeLock which) {}
#endif

inline bool
CanUseExtraThreads()
{
    extern bool gCanUseExtraThreads;
    return gCanUseExtraThreads;
}

void DisableExtraThreads();

/*
 * Encapsulates portions of the runtime/context that are tied to a
 * single active thread.  Instances of this structure can occur for
 * the main thread as |JSRuntime::mainThread|, for select operations
 * performed off thread, such as parsing.
 */
class PerThreadData : public PerThreadDataFriendFields
{
#ifdef DEBUG
    // Grant access to runtime_.
    friend void js::AssertCurrentThreadCanLock(RuntimeLock which);
#endif

    /*
     * Backpointer to the full shared JSRuntime* with which this
     * thread is associated.  This is private because accessing the
     * fields of this runtime can provoke race conditions, so the
     * intention is that access will be mediated through safe
     * functions like |runtimeFromMainThread| and |associatedWith()| below.
     */
    JSRuntime* runtime_;

  public:
#ifdef JS_TRACE_LOGGING
    TraceLoggerThread*  traceLogger;
#endif

    /* Pointer to the current AutoFlushICache. */
    js::jit::AutoFlushICache* autoFlushICache_;

  public:
    /* State used by jsdtoa.cpp. */
    DtoaState*          dtoaState;

    /*
     * When this flag is non-zero, any attempt to GC will be skipped. It is used
     * to suppress GC when reporting an OOM (see ReportOutOfMemory) and in
     * debugging facilities that cannot tolerate a GC and would rather OOM
     * immediately, such as utilities exposed to GDB. Setting this flag is
     * extremely dangerous and should only be used when in an OOM situation or
     * in non-exposed debugging facilities.
     */
    int32_t suppressGC;

#ifdef DEBUG
    // Whether this thread is actively Ion compiling.
    bool ionCompiling;

    // Whether this thread is actively Ion compiling in a context where a minor
    // GC could happen simultaneously. If this is true, this thread cannot use
    // any pointers into the nursery.
    bool ionCompilingSafeForMinorGC;

    // Whether this thread is currently sweeping GC things.
    bool gcSweeping;
#endif

    // Number of active bytecode compilation on this thread.
    unsigned activeCompilations;

    explicit PerThreadData(JSRuntime* runtime);
    ~PerThreadData();

    bool init();

    bool associatedWith(const JSRuntime* rt) { return runtime_ == rt; }
    inline JSRuntime* runtimeFromMainThread();
    inline JSRuntime* runtimeIfOnOwnerThread();

    inline bool exclusiveThreadsPresent();
    inline void addActiveCompilation();
    inline void removeActiveCompilation();

    // For threads which may be associated with different runtimes, depending
    // on the work they are doing.
    class MOZ_STACK_CLASS AutoEnterRuntime
    {
        PerThreadData* pt;

      public:
        AutoEnterRuntime(PerThreadData* pt, JSRuntime* rt)
          : pt(pt)
        {
            MOZ_ASSERT(!pt->runtime_);
            pt->runtime_ = rt;
        }

        ~AutoEnterRuntime() {
            pt->runtime_ = nullptr;
        }
    };

    js::jit::AutoFlushICache* autoFlushICache() const;
    void setAutoFlushICache(js::jit::AutoFlushICache* afc);

#ifdef JS_SIMULATOR
    js::jit::Simulator* simulator() const;
#endif
};

class AutoLockForExclusiveAccess;
} // namespace js

struct JSRuntime : public JS::shadow::Runtime,
                   public js::MallocProvider<JSRuntime>
{
    /*
     * Per-thread data for the main thread that is associated with
     * this JSRuntime, as opposed to any worker threads used in
     * parallel sections.  See definition of |PerThreadData| struct
     * above for more details.
     *
     * NB: This field is statically asserted to be at offset
     * sizeof(js::shadow::Runtime). See
     * PerThreadDataFriendFields::getMainThread.
     */
    js::PerThreadData mainThread;

    /*
     * If Baseline or Ion code is on the stack, and has called into C++, this
     * will be aligned to an exit frame.
     */
    uint8_t*            jitTop;

    /*
     * The current JSContext when entering JIT code. This field may only be used
     * from JIT code and C++ directly called by JIT code (otherwise it may refer
     * to the wrong JSContext).
     */
    JSContext*          jitJSContext;

     /*
     * Points to the most recent JitActivation pushed on the thread.
     * See JitActivation constructor in vm/Stack.cpp
     */
    js::jit::JitActivation* jitActivation;

    /* See comment for JSRuntime::interrupt_. */
  private:
    mozilla::Atomic<uintptr_t, mozilla::Relaxed> jitStackLimit_;
    void resetJitStackLimit();

    // Like jitStackLimit_, but not reset to trigger interrupts.
    uintptr_t jitStackLimitNoInterrupt_;

  public:
    void initJitStackLimit();

    uintptr_t jitStackLimit() const { return jitStackLimit_; }

    // For read-only JIT use:
    void* addressOfJitStackLimit() { return &jitStackLimit_; }
    static size_t offsetOfJitStackLimit() { return offsetof(JSRuntime, jitStackLimit_); }

    void* addressOfJitStackLimitNoInterrupt() { return &jitStackLimitNoInterrupt_; }

    // Information about the heap allocated backtrack stack used by RegExp JIT code.
    js::irregexp::RegExpStack regexpStack;

  private:
    friend class js::Activation;
    friend class js::ActivationIterator;
    friend class js::jit::JitActivation;
    friend class js::AsmJSActivation;
    friend class js::jit::CompileRuntime;
#ifdef DEBUG
    friend void js::AssertCurrentThreadCanLock(js::RuntimeLock which);
#endif

    /*
     * Points to the most recent activation running on the thread.
     * See Activation comment in vm/Stack.h.
     */
    js::Activation* activation_;

    /*
     * Points to the most recent profiling activation running on the
     * thread.
     */
    js::Activation * volatile profilingActivation_;

    /*
     * The profiler sampler generation after the latest sample.
     *
     * The lapCount indicates the number of largest number of 'laps'
     * (wrapping from high to low) that occurred when writing entries
     * into the sample buffer.  All JitcodeGlobalMap entries referenced
     * from a given sample are assigned the generation of the sample buffer
     * at the START of the run.  If multiple laps occur, then some entries
     * (towards the end) will be written out with the "wrong" generation.
     * The lapCount indicates the required fudge factor to use to compare
     * entry generations with the sample buffer generation.
     */
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> profilerSampleBufferGen_;
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> profilerSampleBufferLapCount_;

    /* See AsmJSActivation comment. */
    js::AsmJSActivation * volatile asmJSActivationStack_;

  public:
    /*
     * Youngest frame of a saved stack that will be picked up as an async stack
     * by any new Activation, and is nullptr when no async stack should be used.
     *
     * The JS::AutoSetAsyncStackForNewCalls class can be used to set this.
     *
     * New activations will reset this to nullptr on construction after getting
     * the current value, and will restore the previous value on destruction.
     */
    JS::PersistentRooted<js::SavedFrame*> asyncStackForNewActivations;

    /*
     * Value of asyncCause to be attached to asyncStackForNewActivations.
     */
    JS::PersistentRooted<JSString*> asyncCauseForNewActivations;

    /*
     * True if the async call was explicitly requested, e.g. via
     * callFunctionWithAsyncStack.
     */
    bool asyncCallIsExplicit;

    /* If non-null, report JavaScript entry points to this monitor. */
    JS::dbg::AutoEntryMonitor* entryMonitor;

    js::Activation* const* addressOfActivation() const {
        return &activation_;
    }
    static unsigned offsetOfActivation() {
        return offsetof(JSRuntime, activation_);
    }

    js::Activation* profilingActivation() const {
        return profilingActivation_;
    }
    void* addressOfProfilingActivation() {
        return (void*) &profilingActivation_;
    }
    static unsigned offsetOfProfilingActivation() {
        return offsetof(JSRuntime, profilingActivation_);
    }

    uint32_t profilerSampleBufferGen() {
        return profilerSampleBufferGen_;
    }
    void resetProfilerSampleBufferGen() {
        profilerSampleBufferGen_ = 0;
    }
    void setProfilerSampleBufferGen(uint32_t gen) {
        // May be called from sampler thread or signal handler; use
        // compareExchange to make sure we have monotonic increase.
        for (;;) {
            uint32_t curGen = profilerSampleBufferGen_;
            if (curGen >= gen)
                break;

            if (profilerSampleBufferGen_.compareExchange(curGen, gen))
                break;
        }
    }

    uint32_t profilerSampleBufferLapCount() {
        MOZ_ASSERT(profilerSampleBufferLapCount_ > 0);
        return profilerSampleBufferLapCount_;
    }
    void resetProfilerSampleBufferLapCount() {
        profilerSampleBufferLapCount_ = 1;
    }
    void updateProfilerSampleBufferLapCount(uint32_t lapCount) {
        MOZ_ASSERT(profilerSampleBufferLapCount_ > 0);

        // May be called from sampler thread or signal handler; use
        // compareExchange to make sure we have monotonic increase.
        for (;;) {
            uint32_t curLapCount = profilerSampleBufferLapCount_;
            if (curLapCount >= lapCount)
                break;

            if (profilerSampleBufferLapCount_.compareExchange(curLapCount, lapCount))
                break;
        }
    }

    js::AsmJSActivation* asmJSActivationStack() const {
        return asmJSActivationStack_;
    }
    static js::AsmJSActivation* innermostAsmJSActivation() {
        js::PerThreadData* ptd = js::TlsPerThreadData.get();
        return ptd ? ptd->runtimeFromMainThread()->asmJSActivationStack_ : nullptr;
    }

    js::Activation* activation() const {
        return activation_;
    }

    /*
     * If non-null, another runtime guaranteed to outlive this one and whose
     * permanent data may be used by this one where possible.
     */
    JSRuntime* parentRuntime;

  private:
    mozilla::Atomic<uint32_t, mozilla::Relaxed> interrupt_;

    /* Call this to accumulate telemetry data. */
    JSAccumulateTelemetryDataCallback telemetryCallback;
  public:
    // Accumulates data for Firefox telemetry. |id| is the ID of a JS_TELEMETRY_*
    // histogram. |key| provides an additional key to identify the histogram.
    // |sample| is the data to add to the histogram.
    void addTelemetry(int id, uint32_t sample, const char* key = nullptr);

    void setTelemetryCallback(JSRuntime* rt, JSAccumulateTelemetryDataCallback callback);

    enum InterruptMode {
        RequestInterruptUrgent,
        RequestInterruptCanWait
    };

    // Any thread can call requestInterrupt() to request that the main JS thread
    // stop running and call the interrupt callback (allowing the interrupt
    // callback to halt execution). To stop the main JS thread, requestInterrupt
    // sets two fields: interrupt_ (set to true) and jitStackLimit_ (set to
    // UINTPTR_MAX). The JS engine must continually poll one of these fields
    // and call handleInterrupt if either field has the interrupt value. (The
    // point of setting jitStackLimit_ to UINTPTR_MAX is that JIT code already
    // needs to guard on jitStackLimit_ in every function prologue to avoid
    // stack overflow, so we avoid a second branch on interrupt_ by setting
    // jitStackLimit_ to a value that is guaranteed to fail the guard.)
    //
    // Note that the writes to interrupt_ and jitStackLimit_ use a Relaxed
    // Atomic so, while the writes are guaranteed to eventually be visible to
    // the main thread, it can happen in any order. handleInterrupt calls the
    // interrupt callback if either is set, so it really doesn't matter as long
    // as the JS engine is continually polling at least one field. In corner
    // cases, this relaxed ordering could lead to an interrupt handler being
    // called twice in succession after a single requestInterrupt call, but
    // that's fine.
    void requestInterrupt(InterruptMode mode);
    bool handleInterrupt(JSContext* cx);

    MOZ_ALWAYS_INLINE bool hasPendingInterrupt() const {
        return interrupt_;
    }

    // For read-only JIT use:
    void* addressOfInterruptUint32() {
        static_assert(sizeof(interrupt_) == sizeof(uint32_t), "Assumed by JIT callers");
        return &interrupt_;
    }

    /* Set when handling a signal for a thread associated with this runtime. */
    bool handlingSignal;

    JSInterruptCallback interruptCallback;

#ifdef DEBUG
    void assertCanLock(js::RuntimeLock which);
#else
    void assertCanLock(js::RuntimeLock which) {}
#endif

  private:
    /*
     * Lock taken when using per-runtime or per-zone data that could otherwise
     * be accessed simultaneously by both the main thread and another thread
     * with an ExclusiveContext.
     *
     * Locking this only occurs if there is actually a thread other than the
     * main thread with an ExclusiveContext which could access such data.
     */
    PRLock* exclusiveAccessLock;
    mozilla::DebugOnly<PRThread*> exclusiveAccessOwner;
    mozilla::DebugOnly<bool> mainThreadHasExclusiveAccess;

    /* Number of non-main threads with an ExclusiveContext. */
    size_t numExclusiveThreads;

    friend class js::AutoLockForExclusiveAccess;

  public:
    void setUsedByExclusiveThread(JS::Zone* zone);
    void clearUsedByExclusiveThread(JS::Zone* zone);

#ifdef DEBUG
    bool currentThreadHasExclusiveAccess() {
        return (!numExclusiveThreads && mainThreadHasExclusiveAccess) ||
               exclusiveAccessOwner == PR_GetCurrentThread();
    }
#endif // DEBUG

    bool exclusiveThreadsPresent() const {
        return numExclusiveThreads > 0;
    }

    // How many compartments there are across all zones. This number includes
    // ExclusiveContext compartments, so it isn't necessarily equal to the
    // number of compartments visited by CompartmentsIter.
    size_t              numCompartments;

    /* Locale-specific callbacks for string conversion. */
    const JSLocaleCallbacks* localeCallbacks;

    /* Default locale for Internationalization API */
    char* defaultLocale;

    /* Default JSVersion. */
    JSVersion defaultVersion_;

    /* Futex state, used by futexWait and futexWake on the Atomics object */
    js::FutexRuntime fx;

  private:
    /* See comment for JS_AbortIfWrongThread in jsapi.h. */
    void* ownerThread_;
    size_t ownerThreadNative_;
    friend bool js::CurrentThreadCanAccessRuntime(JSRuntime* rt);
  public:

    size_t ownerThreadNative() const {
        return ownerThreadNative_;
    }

    /* Temporary arena pool used while compiling and decompiling. */
    static const size_t TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE = 4 * 1024;
    js::LifoAlloc tempLifoAlloc;

  private:
    js::jit::JitRuntime* jitRuntime_;

    /*
     * Self-hosting state cloned on demand into other compartments. Shared with the parent
     * runtime if there is one.
     */
    js::NativeObject* selfHostingGlobal_;

    static js::GlobalObject*
    createSelfHostingGlobal(JSContext* cx);

    /* Space for interpreter frames. */
    js::InterpreterStack interpreterStack_;

    js::jit::JitRuntime* createJitRuntime(JSContext* cx);

  public:
    js::jit::JitRuntime* getJitRuntime(JSContext* cx) {
        return jitRuntime_ ? jitRuntime_ : createJitRuntime(cx);
    }
    js::jit::JitRuntime* jitRuntime() const {
        return jitRuntime_;
    }
    bool hasJitRuntime() const {
        return !!jitRuntime_;
    }
    js::InterpreterStack& interpreterStack() {
        return interpreterStack_;
    }

    //-------------------------------------------------------------------------
    // Self-hosting support
    //-------------------------------------------------------------------------

    bool initSelfHosting(JSContext* cx);
    void finishSelfHosting();
    void markSelfHostingGlobal(JSTracer* trc);
    bool isSelfHostingGlobal(JSObject* global) {
        return global == selfHostingGlobal_;
    }
    bool isSelfHostingCompartment(JSCompartment* comp) const;
    bool isSelfHostingZone(const JS::Zone* zone) const;
    bool cloneSelfHostedFunctionScript(JSContext* cx, js::Handle<js::PropertyName*> name,
                                       js::Handle<JSFunction*> targetFun);
    bool cloneSelfHostedValue(JSContext* cx, js::Handle<js::PropertyName*> name,
                              js::MutableHandleValue vp);

    //-------------------------------------------------------------------------
    // Locale information
    //-------------------------------------------------------------------------

    /*
     * Set the default locale for the ECMAScript Internationalization API
     * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat).
     * Note that the Internationalization API encourages clients to
     * specify their own locales.
     * The locale string remains owned by the caller.
     */
    bool setDefaultLocale(const char* locale);

    /* Reset the default locale to OS defaults. */
    void resetDefaultLocale();

    /* Gets current default locale. String remains owned by context. */
    const char* getDefaultLocale();

    JSVersion defaultVersion() { return defaultVersion_; }
    void setDefaultVersion(JSVersion v) { defaultVersion_ = v; }

    /* Base address of the native stack for the current thread. */
    const uintptr_t     nativeStackBase;

    /* The native stack size limit that runtime should not exceed. */
    size_t              nativeStackQuota[js::StackKindCount];

    /* Context create/destroy callback. */
    JSContextCallback   cxCallback;
    void*              cxCallbackData;

    /* Compartment destroy callback. */
    JSDestroyCompartmentCallback destroyCompartmentCallback;

    /* Zone destroy callback. */
    JSZoneCallback destroyZoneCallback;

    /* Zone sweep callback. */
    JSZoneCallback sweepZoneCallback;

    /* Call this to get the name of a compartment. */
    JSCompartmentNameCallback compartmentNameCallback;

    js::ActivityCallback  activityCallback;
    void*                activityCallbackArg;
    void triggerActivityCallback(bool active);

    /* The request depth for this thread. */
    unsigned            requestDepth;

#ifdef DEBUG
    unsigned            checkRequestDepth;

    /*
     * To help embedders enforce their invariants, we allow them to specify in
     * advance which JSContext should be passed to JSAPI calls. If this is set
     * to a non-null value, the assertSameCompartment machinery does double-
     * duty (in debug builds) to verify that it matches the cx being used.
     */
    JSContext*         activeContext;
#endif

    /* Garbage collector state, used by jsgc.c. */
    js::gc::GCRuntime   gc;

    /* Garbage collector state has been successfully initialized. */
    bool                gcInitialized;

    int gcZeal() { return gc.zeal(); }

    void lockGC() {
        assertCanLock(js::GCLock);
        gc.lockGC();
    }

    void unlockGC() {
        gc.unlockGC();
    }

#ifdef JS_SIMULATOR
    js::jit::Simulator* simulator_;
#endif

  public:
#ifdef JS_SIMULATOR
    js::jit::Simulator* simulator() const;
    uintptr_t* addressOfSimulatorStackLimit();
#endif

    /* Strong references on scripts held for PCCount profiling API. */
    JS::PersistentRooted<js::ScriptAndCountsVector>* scriptAndCountsVector;

    /* Code coverage output. */
    js::coverage::LCovRuntime lcovOutput;

    /* Well-known numbers held for use by this runtime's contexts. */
    const js::Value     NaNValue;
    const js::Value     negativeInfinityValue;
    const js::Value     positiveInfinityValue;

    js::PropertyName*   emptyString;

    /* List of active contexts sharing this runtime. */
    mozilla::LinkedList<JSContext> contextList;

    bool hasContexts() const {
        return !contextList.isEmpty();
    }

    mozilla::UniquePtr<js::SourceHook> sourceHook;

    /* SPS profiling metadata */
    js::SPSProfiler     spsProfiler;

    /* If true, new scripts must be created with PC counter information. */
    bool                profilingScripts;

    /* Whether sampling should be enabled or not. */
  private:
    mozilla::Atomic<bool, mozilla::SequentiallyConsistent> suppressProfilerSampling;

  public:
    bool isProfilerSamplingEnabled() const {
        return !suppressProfilerSampling;
    }
    void disableProfilerSampling() {
        suppressProfilerSampling = true;
    }
    void enableProfilerSampling() {
        suppressProfilerSampling = false;
    }

    /* Had an out-of-memory error which did not populate an exception. */
    bool                hadOutOfMemory;

    /* We are currently deleting an object due to an initialization failure. */
    mozilla::DebugOnly<bool> handlingInitFailure;

    /* A context has been created on this runtime. */
    bool                haveCreatedContext;

    /*
     * Allow relazifying functions in compartments that are active. This is
     * only used by the relazifyFunctions() testing function.
     */
    bool                allowRelazificationForTesting;

    /* Linked list of all Debugger objects in the runtime. */
    mozilla::LinkedList<js::Debugger> debuggerList;

    /*
     * Head of circular list of all enabled Debuggers that have
     * onNewGlobalObject handler methods established.
     */
    JSCList             onNewGlobalObjectWatchers;

    /* Client opaque pointers */
    void*               data;

#if defined(XP_DARWIN) && defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    js::AsmJSMachExceptionHandler asmJSMachExceptionHandler;
#endif

  private:
    // Whether EnsureSignalHandlersInstalled succeeded in installing all the
    // relevant handlers for this platform.
    bool signalHandlersInstalled_;

    // Whether we should use them or they have been disabled for making
    // debugging easier. If signal handlers aren't installed, it is set to false.
    bool canUseSignalHandlers_;

  public:
    bool canUseSignalHandlers() const {
        return canUseSignalHandlers_;
    }
    void setCanUseSignalHandlers(bool enable) {
        canUseSignalHandlers_ = signalHandlersInstalled_ && enable;
    }

  private:
    js::FreeOp          defaultFreeOp_;

  public:
    js::FreeOp* defaultFreeOp() {
        return &defaultFreeOp_;
    }

    uint32_t            debuggerMutations;

    const JSSecurityCallbacks* securityCallbacks;
    const js::DOMCallbacks* DOMcallbacks;
    JSDestroyPrincipalsOp destroyPrincipals;
    JSReadPrincipalsOp readPrincipals;

    /* Optional error reporter. */
    JSErrorReporter     errorReporter;

    /* AsmJSCache callbacks are runtime-wide. */
    JS::AsmJSCacheOps   asmJSCacheOps;

    /* Head of the linked list of linked asm.js modules. */
    js::AsmJSModule*   linkedAsmJSModules;

    /*
     * The propertyRemovals counter is incremented for every JSObject::clear,
     * and for each JSObject::remove method call that frees a slot in the given
     * object. See js_NativeGet and js_NativeSet in jsobj.cpp.
     */
    uint32_t            propertyRemovals;

#if !EXPOSE_INTL_API
    /* Number localization, used by jsnum.cpp. */
    const char*         thousandsSeparator;
    const char*         decimalSeparator;
    const char*         numGrouping;
#endif

  private:
    js::MathCache* mathCache_;
    js::MathCache* createMathCache(JSContext* cx);
  public:
    js::MathCache* getMathCache(JSContext* cx) {
        return mathCache_ ? mathCache_ : createMathCache(cx);
    }
    js::MathCache* maybeGetMathCache() {
        return mathCache_;
    }

    js::GSNCache        gsnCache;
    js::ScopeCoordinateNameCache scopeCoordinateNameCache;
    js::NewObjectCache  newObjectCache;
    js::NativeIterCache nativeIterCache;
    js::UncompressedSourceCache uncompressedSourceCache;
    js::EvalCache       evalCache;
    js::LazyScriptCache lazyScriptCache;

    js::CompressedSourceSet compressedSourceSet;

    // Pool of maps used during parse/emit. This may be modified by threads
    // with an ExclusiveContext and requires a lock. Active compilations
    // prevent the pool from being purged during GCs.
  private:
    js::frontend::ParseMapPool parseMapPool_;
    unsigned activeCompilations_;
  public:
    js::frontend::ParseMapPool& parseMapPool() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        return parseMapPool_;
    }
    bool hasActiveCompilations() {
        return activeCompilations_ != 0;
    }
    void addActiveCompilation() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        activeCompilations_++;
    }
    void removeActiveCompilation() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        activeCompilations_--;
    }

    // Count of AutoKeepAtoms instances on the main thread's stack. When any
    // instances exist, atoms in the runtime will not be collected. Threads
    // with an ExclusiveContext do not increment this value, but the presence
    // of any such threads also inhibits collection of atoms. We don't scan the
    // stacks of exclusive threads, so we need to avoid collecting their
    // objects in another way. The only GC thing pointers they have are to
    // their exclusive compartment (which is not collected) or to the atoms
    // compartment. Therefore, we avoid collecting the atoms compartment when
    // exclusive threads are running.
  private:
    unsigned keepAtoms_;
    friend class js::AutoKeepAtoms;
  public:
    bool keepAtoms() {
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));
        return keepAtoms_ != 0 || exclusiveThreadsPresent();
    }

  private:
    const JSPrincipals* trustedPrincipals_;
  public:
    void setTrustedPrincipals(const JSPrincipals* p) { trustedPrincipals_ = p; }
    const JSPrincipals* trustedPrincipals() const { return trustedPrincipals_; }

  private:
    bool beingDestroyed_;
  public:
    bool isBeingDestroyed() const {
        return beingDestroyed_;
    }

  private:
    // Set of all atoms other than those in permanentAtoms and staticStrings.
    // Reading or writing this set requires the calling thread to have an
    // ExclusiveContext and hold a lock. Use AutoLockForExclusiveAccess.
    js::AtomSet* atoms_;

    // Compartment and associated zone containing all atoms in the runtime, as
    // well as runtime wide IonCode stubs. Modifying the contents of this
    // compartment requires the calling thread to have an ExclusiveContext and
    // hold a lock. Use AutoLockForExclusiveAccess.
    JSCompartment* atomsCompartment_;

    // Set of all live symbols produced by Symbol.for(). All such symbols are
    // allocated in the atomsCompartment. Reading or writing the symbol
    // registry requires the calling thread to have an ExclusiveContext and
    // hold a lock. Use AutoLockForExclusiveAccess.
    js::SymbolRegistry symbolRegistry_;

  public:
    bool initializeAtoms(JSContext* cx);
    void finishAtoms();

    void sweepAtoms();

    js::AtomSet& atoms() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        return *atoms_;
    }
    JSCompartment* atomsCompartment() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        return atomsCompartment_;
    }

    bool isAtomsCompartment(JSCompartment* comp) {
        return comp == atomsCompartment_;
    }

    // The atoms compartment is the only one in its zone.
    inline bool isAtomsZone(const JS::Zone* zone) const;

    bool activeGCInAtomsZone();

    js::SymbolRegistry& symbolRegistry() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        return symbolRegistry_;
    }

    // Permanent atoms are fixed during initialization of the runtime and are
    // not modified or collected until the runtime is destroyed. These may be
    // shared with another, longer living runtime through |parentRuntime| and
    // can be freely accessed with no locking necessary.

    // Permanent atoms pre-allocated for general use.
    js::StaticStrings* staticStrings;

    // Cached pointers to various permanent property names.
    JSAtomState* commonNames;

    // All permanent atoms in the runtime, other than those in staticStrings.
    // Unlike |atoms_|, access to this does not require
    // AutoLockForExclusiveAccess because it is frozen and thus read-only.
    js::FrozenAtomSet* permanentAtoms;

    bool transformToPermanentAtoms(JSContext* cx);

    // Cached well-known symbols (ES6 rev 24 6.1.5.1). Like permanent atoms,
    // these are shared with the parentRuntime, if any.
    js::WellKnownSymbols* wellKnownSymbols;

    const JSWrapObjectCallbacks*           wrapObjectCallbacks;
    js::PreserveWrapperCallback            preserveWrapperCallback;

    // Table of bytecode and other data that may be shared across scripts
    // within the runtime. This may be modified by threads with an
    // ExclusiveContext and requires a lock.
  private:
    js::ScriptDataTable scriptDataTable_;
  public:
    js::ScriptDataTable& scriptDataTable() {
        MOZ_ASSERT(currentThreadHasExclusiveAccess());
        return scriptDataTable_;
    }

    bool                jitSupportsFloatingPoint;
    bool                jitSupportsSimd;

    // Cache for jit::GetPcScript().
    js::jit::PcScriptCache* ionPcScriptCache;

    js::ScriptEnvironmentPreparer* scriptEnvironmentPreparer;

    js::CTypesActivityCallback  ctypesActivityCallback;

  private:
    static mozilla::Atomic<size_t> liveRuntimesCount;

  public:
    static bool hasLiveRuntimes() {
        return liveRuntimesCount > 0;
    }

    explicit JSRuntime(JSRuntime* parentRuntime);
    ~JSRuntime();

    bool init(uint32_t maxbytes, uint32_t maxNurseryBytes);

    JSRuntime* thisFromCtor() { return this; }

    /*
     * Call this after allocating memory held by GC things, to update memory
     * pressure counters or report the OOM error if necessary. If oomError and
     * cx is not null the function also reports OOM error.
     *
     * The function must be called outside the GC lock and in case of OOM error
     * the caller must ensure that no deadlock possible during OOM reporting.
     */
    void updateMallocCounter(size_t nbytes);
    void updateMallocCounter(JS::Zone* zone, size_t nbytes);

    void reportAllocationOverflow() { js::ReportAllocationOverflow(nullptr); }

    /*
     * This should be called after system malloc/calloc/realloc returns nullptr
     * to try to recove some memory or to report an error.  For realloc, the
     * original pointer must be passed as reallocPtr.
     *
     * The function must be called outside the GC lock.
     */
    JS_FRIEND_API(void*) onOutOfMemory(js::AllocFunction allocator, size_t nbytes,
                                       void* reallocPtr = nullptr, JSContext* maybecx = nullptr);

    /*  onOutOfMemory but can call the largeAllocationFailureCallback. */
    JS_FRIEND_API(void*) onOutOfMemoryCanGC(js::AllocFunction allocator, size_t nbytes,
                                            void* reallocPtr = nullptr);

    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::RuntimeSizes* runtime);

  private:
    JS::RuntimeOptions options_;
    const js::Class* windowProxyClass_;

    // Settings for how helper threads can be used.
    bool offthreadIonCompilationEnabled_;
    bool parallelParsingEnabled_;

    bool autoWritableJitCodeActive_;

  public:

    // Note: these values may be toggled dynamically (in response to about:config
    // prefs changing).
    void setOffthreadIonCompilationEnabled(bool value) {
        offthreadIonCompilationEnabled_ = value;
    }
    bool canUseOffthreadIonCompilation() const {
        return offthreadIonCompilationEnabled_;
    }
    void setParallelParsingEnabled(bool value) {
        parallelParsingEnabled_ = value;
    }
    bool canUseParallelParsing() const {
        return parallelParsingEnabled_;
    }

    void toggleAutoWritableJitCodeActive(bool b) {
        MOZ_ASSERT(autoWritableJitCodeActive_ != b, "AutoWritableJitCode should not be nested.");
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));
        autoWritableJitCodeActive_ = b;
    }

    const JS::RuntimeOptions& options() const {
        return options_;
    }
    JS::RuntimeOptions& options() {
        return options_;
    }

    const js::Class* maybeWindowProxyClass() const {
        return windowProxyClass_;
    }
    void setWindowProxyClass(const js::Class* clasp) {
        windowProxyClass_ = clasp;
    }

#ifdef DEBUG
  public:
    js::AutoEnterPolicy* enteredPolicy;
#endif

    /* See comment for JS::SetLargeAllocationFailureCallback in jsapi.h. */
    JS::LargeAllocationFailureCallback largeAllocationFailureCallback;
    void* largeAllocationFailureCallbackData;

    /* See comment for JS::SetOutOfMemoryCallback in jsapi.h. */
    JS::OutOfMemoryCallback oomCallback;
    void* oomCallbackData;

    /*
     * These variations of malloc/calloc/realloc will call the
     * large-allocation-failure callback on OOM and retry the allocation.
     */

    static const unsigned LARGE_ALLOCATION = 25 * 1024 * 1024;

    template <typename T>
    T* pod_callocCanGC(size_t numElems) {
        T* p = pod_calloc<T>(numElems);
        if (MOZ_LIKELY(!!p))
            return p;
        size_t bytes;
        if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes))) {
            reportAllocationOverflow();
            return nullptr;
        }
        return static_cast<T*>(onOutOfMemoryCanGC(js::AllocFunction::Calloc, bytes));
    }

    template <typename T>
    T* pod_reallocCanGC(T* p, size_t oldSize, size_t newSize) {
        T* p2 = pod_realloc<T>(p, oldSize, newSize);
        if (MOZ_LIKELY(!!p2))
            return p2;
        size_t bytes;
        if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(newSize, &bytes))) {
            reportAllocationOverflow();
            return nullptr;
        }
        return static_cast<T*>(onOutOfMemoryCanGC(js::AllocFunction::Realloc, bytes, p));
    }

    /*
     * Debugger.Memory functions like takeCensus use this embedding-provided
     * function to assess the size of malloc'd blocks of memory.
     */
    mozilla::MallocSizeOf debuggerMallocSizeOf;

    /* Last time at which an animation was played for this runtime. */
    int64_t lastAnimationTime;

  public:
    js::PerformanceMonitoring performanceMonitoring;
};

namespace js {

// When entering JIT code, the calling JSContext* is stored into the thread's
// PerThreadData. This function retrieves the JSContext with the pre-condition
// that the caller is JIT code or C++ called directly from JIT code. This
// function should not be called from arbitrary locations since the JSContext
// may be the wrong one.
static inline JSContext*
GetJSContextFromJitCode()
{
    JSContext* cx = js::TlsPerThreadData.get()->runtimeFromMainThread()->jitJSContext;
    MOZ_ASSERT(cx);
    return cx;
}

/*
 * Flags accompany script version data so that a) dynamically created scripts
 * can inherit their caller's compile-time properties and b) scripts can be
 * appropriately compared in the eval cache across global option changes. An
 * example of the latter is enabling the top-level-anonymous-function-is-error
 * option: subsequent evals of the same, previously-valid script text may have
 * become invalid.
 */
namespace VersionFlags {
static const unsigned MASK      = 0x0FFF; /* see JSVersion in jspubtd.h */
} /* namespace VersionFlags */

static inline JSVersion
VersionNumber(JSVersion version)
{
    return JSVersion(uint32_t(version) & VersionFlags::MASK);
}

static inline JSVersion
VersionExtractFlags(JSVersion version)
{
    return JSVersion(uint32_t(version) & ~VersionFlags::MASK);
}

static inline void
VersionCopyFlags(JSVersion* version, JSVersion from)
{
    *version = JSVersion(VersionNumber(*version) | VersionExtractFlags(from));
}

static inline bool
VersionHasFlags(JSVersion version)
{
    return !!VersionExtractFlags(version);
}

static inline bool
VersionIsKnown(JSVersion version)
{
    return VersionNumber(version) != JSVERSION_UNKNOWN;
}

inline void
FreeOp::free_(void* p)
{
    js_free(p);
}

inline void
FreeOp::freeLater(void* p)
{
    // FreeOps other than the defaultFreeOp() are constructed on the stack,
    // and won't hold onto the pointers to free indefinitely.
    MOZ_ASSERT(this != runtime()->defaultFreeOp());

    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!freeLaterList.append(p))
        oomUnsafe.crash("FreeOp::freeLater");
}

/*
 * RAII class that takes the GC lock while it is live.
 *
 * Note that the lock may be temporarily released by use of AutoUnlockGC when
 * passed a non-const reference to this class.
 */
class MOZ_RAII AutoLockGC
{
  public:
    explicit AutoLockGC(JSRuntime* rt
                        MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : runtime_(rt), wasUnlocked_(false)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        lock();
    }

    ~AutoLockGC() {
        unlock();
    }

    void lock() {
        runtime_->lockGC();
    }

    void unlock() {
        runtime_->unlockGC();
        wasUnlocked_ = true;
    }

#ifdef DEBUG
    bool wasUnlocked() {
        return wasUnlocked_;
    }
#endif

  private:
    JSRuntime* runtime_;
    mozilla::DebugOnly<bool> wasUnlocked_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    AutoLockGC(const AutoLockGC&) = delete;
    AutoLockGC& operator=(const AutoLockGC&) = delete;
};

class MOZ_RAII AutoUnlockGC
{
  public:
    explicit AutoUnlockGC(AutoLockGC& lock
                          MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : lock(lock)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        lock.unlock();
    }

    ~AutoUnlockGC() {
        lock.lock();
    }

  private:
    AutoLockGC& lock;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    AutoUnlockGC(const AutoUnlockGC&) = delete;
    AutoUnlockGC& operator=(const AutoUnlockGC&) = delete;
};

class MOZ_RAII AutoKeepAtoms
{
    PerThreadData* pt;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    explicit AutoKeepAtoms(PerThreadData* pt
                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : pt(pt)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        if (JSRuntime* rt = pt->runtimeIfOnOwnerThread()) {
            rt->keepAtoms_++;
        } else {
            // This should be a thread with an exclusive context, which will
            // always inhibit collection of atoms.
            MOZ_ASSERT(pt->exclusiveThreadsPresent());
        }
    }
    ~AutoKeepAtoms() {
        if (JSRuntime* rt = pt->runtimeIfOnOwnerThread()) {
            MOZ_ASSERT(rt->keepAtoms_);
            rt->keepAtoms_--;
            if (rt->gc.fullGCForAtomsRequested() && !rt->keepAtoms())
                rt->gc.triggerFullGCForAtoms();
        }
    }
};

inline JSRuntime*
PerThreadData::runtimeFromMainThread()
{
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
    return runtime_;
}

inline JSRuntime*
PerThreadData::runtimeIfOnOwnerThread()
{
    return (runtime_ && CurrentThreadCanAccessRuntime(runtime_)) ? runtime_ : nullptr;
}

inline bool
PerThreadData::exclusiveThreadsPresent()
{
    return runtime_->exclusiveThreadsPresent();
}

inline void
PerThreadData::addActiveCompilation()
{
    activeCompilations++;
    runtime_->addActiveCompilation();
}

inline void
PerThreadData::removeActiveCompilation()
{
    MOZ_ASSERT(activeCompilations);
    activeCompilations--;
    runtime_->removeActiveCompilation();
}

/************************************************************************/

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Value* vec, size_t len)
{
    mozilla::PodZero(vec, len);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Value* beg, Value* end)
{
    mozilla::PodZero(beg, end - beg);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(jsid* beg, jsid* end)
{
    for (jsid* id = beg; id != end; ++id)
        *id = INT_TO_JSID(0);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(jsid* vec, size_t len)
{
    MakeRangeGCSafe(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Shape** beg, Shape** end)
{
    mozilla::PodZero(beg, end - beg);
}

static MOZ_ALWAYS_INLINE void
MakeRangeGCSafe(Shape** vec, size_t len)
{
    mozilla::PodZero(vec, len);
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToUndefined(Value* beg, Value* end)
{
    for (Value* v = beg; v != end; ++v)
        v->setUndefined();
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToUndefined(Value* vec, size_t len)
{
    SetValueRangeToUndefined(vec, vec + len);
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToNull(Value* beg, Value* end)
{
    for (Value* v = beg; v != end; ++v)
        v->setNull();
}

static MOZ_ALWAYS_INLINE void
SetValueRangeToNull(Value* vec, size_t len)
{
    SetValueRangeToNull(vec, vec + len);
}

/*
 * Allocation policy that uses JSRuntime::pod_malloc and friends, so that
 * memory pressure is properly accounted for. This is suitable for
 * long-lived objects owned by the JSRuntime.
 *
 * Since it doesn't hold a JSContext (those may not live long enough), it
 * can't report out-of-memory conditions itself; the caller must check for
 * OOM and take the appropriate action.
 *
 * FIXME bug 647103 - replace these *AllocPolicy names.
 */
class RuntimeAllocPolicy
{
    JSRuntime* const runtime;

  public:
    MOZ_IMPLICIT RuntimeAllocPolicy(JSRuntime* rt) : runtime(rt) {}

    template <typename T>
    T* maybe_pod_malloc(size_t numElems) {
        return runtime->maybe_pod_malloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_calloc(size_t numElems) {
        return runtime->maybe_pod_calloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return runtime->maybe_pod_realloc<T>(p, oldSize, newSize);
    }

    template <typename T>
    T* pod_malloc(size_t numElems) {
        return runtime->pod_malloc<T>(numElems);
    }

    template <typename T>
    T* pod_calloc(size_t numElems) {
        return runtime->pod_calloc<T>(numElems);
    }

    template <typename T>
    T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return runtime->pod_realloc<T>(p, oldSize, newSize);
    }

    void free_(void* p) { js_free(p); }
    void reportAllocOverflow() const {}

    bool checkSimulatedOOM() const {
        return !js::oom::ShouldFailWithOOM();
    }
};

extern const JSSecurityCallbacks NullSecurityCallbacks;

// Debugging RAII class which marks the current thread as performing an Ion
// compilation, for use by CurrentThreadCan{Read,Write}CompilationData
class MOZ_RAII AutoEnterIonCompilation
{
  public:
    explicit AutoEnterIonCompilation(bool safeForMinorGC
                                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM) {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;

#ifdef DEBUG
        PerThreadData* pt = js::TlsPerThreadData.get();
        MOZ_ASSERT(!pt->ionCompiling);
        MOZ_ASSERT(!pt->ionCompilingSafeForMinorGC);
        pt->ionCompiling = true;
        pt->ionCompilingSafeForMinorGC = safeForMinorGC;
#endif
    }

    ~AutoEnterIonCompilation() {
#ifdef DEBUG
        PerThreadData* pt = js::TlsPerThreadData.get();
        MOZ_ASSERT(pt->ionCompiling);
        pt->ionCompiling = false;
        pt->ionCompilingSafeForMinorGC = false;
#endif
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * AutoInitGCManagedObject is a wrapper for use when initializing a object whose
 * lifetime is managed by the GC.  It ensures that the object is destroyed if
 * initialization fails but also allows us to assert the invariant that such
 * objects are only destroyed in this way or by the GC.
 *
 * It has a limited interface but is a drop-in replacement for UniquePtr<T> is
 * this situation.  For example:
 *
 *   AutoInitGCManagedObject<MyClass> ptr(cx->make_unique<MyClass>());
 *   if (!ptr) {
 *     ReportOutOfMemory(cx);
 *     return nullptr;
 *   }
 *
 *   if (!ptr->init(cx))
 *     return nullptr;    // Object destroyed here if init() failed.
 *
 *   object->setPrivate(ptr.release());
 *   // Initialization successful, ptr is now owned through another object.
 */
template <typename T>
class MOZ_STACK_CLASS AutoInitGCManagedObject
{
    typedef mozilla::UniquePtr<T, JS::DeletePolicy<T>> UniquePtrT;

    UniquePtrT ptr_;

  public:
    explicit AutoInitGCManagedObject(UniquePtrT&& ptr)
      : ptr_(mozilla::Move(ptr))
    {}

    ~AutoInitGCManagedObject() {
#ifdef DEBUG
        if (ptr_) {
            JSRuntime* rt = TlsPerThreadData.get()->runtimeFromMainThread();
            MOZ_ASSERT(!rt->handlingInitFailure);
            rt->handlingInitFailure = true;
            ptr_.reset(nullptr);
            rt->handlingInitFailure = false;
        }
#endif
    }

    T& operator*() const {
        return *get();
    }

    T* operator->() const {
        return get();
    }

    explicit operator bool() const {
        return get() != nullptr;
    }

    T* get() const {
        return ptr_.get();
    }

    T* release() {
        return ptr_.release();
    }

    AutoInitGCManagedObject(const AutoInitGCManagedObject<T>& other) = delete;
    AutoInitGCManagedObject& operator=(const AutoInitGCManagedObject<T>& other) = delete;
};

} /* namespace js */

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* vm_Runtime_h */
