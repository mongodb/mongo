/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Definitions for managing off-main-thread work using a process wide list
 * of worklist items and pool of threads. Worklist items are engine internal,
 * and are distinct from e.g. web workers.
 */

#ifndef vm_HelperThreads_h
#define vm_HelperThreads_h

#include "mozilla/GuardObjects.h"
#include "mozilla/PodOperations.h"

#include "jscntxt.h"
#include "jslock.h"

#include "frontend/TokenStream.h"
#include "jit/Ion.h"

namespace js {

struct HelperThread;
struct AsmJSParallelTask;
struct ParseTask;
namespace jit {
  class IonBuilder;
}

// Per-process state for off thread work items.
class GlobalHelperThreadState
{
  public:
    // Number of CPUs to treat this machine as having when creating threads.
    // May be accessed without locking.
    size_t cpuCount;

    // Number of threads to create. May be accessed without locking.
    size_t threadCount;

    typedef Vector<jit::IonBuilder*, 0, SystemAllocPolicy> IonBuilderVector;
    typedef Vector<AsmJSParallelTask*, 0, SystemAllocPolicy> AsmJSParallelTaskVector;
    typedef Vector<ParseTask*, 0, SystemAllocPolicy> ParseTaskVector;
    typedef Vector<SourceCompressionTask*, 0, SystemAllocPolicy> SourceCompressionTaskVector;
    typedef Vector<GCHelperState*, 0, SystemAllocPolicy> GCHelperStateVector;
    typedef Vector<GCParallelTask*, 0, SystemAllocPolicy> GCParallelTaskVector;
    typedef mozilla::LinkedList<jit::IonBuilder> IonBuilderList;

    // List of available threads, or null if the thread state has not been initialized.
    HelperThread* threads;

  private:
    // The lists below are all protected by |lock|.

    // Ion compilation worklist and finished jobs.
    IonBuilderVector ionWorklist_, ionFinishedList_;

    // List of IonBuilders using lazy linking pending to get linked.
    IonBuilderList ionLazyLinkList_;

    // AsmJS worklist and finished jobs.
    //
    // Simultaneous AsmJS compilations all service the same AsmJS module.
    // The main thread must pick up finished optimizations and perform codegen.
    // |asmJSCompilationInProgress| is used to avoid triggering compilations
    // for more than one module at a time.
    AsmJSParallelTaskVector asmJSWorklist_, asmJSFinishedList_;

  public:
    // For now, only allow a single parallel asm.js compilation to happen at a
    // time. This avoids race conditions on asmJSWorklist/asmJSFinishedList/etc.
    mozilla::Atomic<bool> asmJSCompilationInProgress;

  private:
    // Script parsing/emitting worklist and finished jobs.
    ParseTaskVector parseWorklist_, parseFinishedList_;

    // Parse tasks waiting for an atoms-zone GC to complete.
    ParseTaskVector parseWaitingOnGC_;

    // Source compression worklist.
    SourceCompressionTaskVector compressionWorklist_;

    // Runtimes which have sweeping / allocating work to do.
    GCHelperStateVector gcHelperWorklist_;

    // GC tasks needing to be done in parallel.
    GCParallelTaskVector gcParallelWorklist_;

  public:
    size_t maxIonCompilationThreads() const {
        return 1;
    }
    size_t maxAsmJSCompilationThreads() const {
        if (cpuCount < 2)
            return 2;
        return cpuCount;
    }

    GlobalHelperThreadState();

    void ensureInitialized();
    void finish();

    void lock();
    void unlock();

#ifdef DEBUG
    bool isLocked();
#endif

    enum CondVar {
        // For notifying threads waiting for work that they may be able to make progress.
        CONSUMER,

        // For notifying threads doing work that they may be able to make progress.
        PRODUCER,

        // For notifying threads doing work which are paused that they may be
        // able to resume making progress.
        PAUSE
    };

    void wait(CondVar which, uint32_t timeoutMillis = 0);
    void notifyAll(CondVar which);
    void notifyOne(CondVar which);

    // Helper method for removing items from the vectors below while iterating over them.
    template <typename T>
    void remove(T& vector, size_t* index)
    {
        vector[(*index)--] = vector.back();
        vector.popBack();
    }

    IonBuilderVector& ionWorklist() {
        MOZ_ASSERT(isLocked());
        return ionWorklist_;
    }
    IonBuilderVector& ionFinishedList() {
        MOZ_ASSERT(isLocked());
        return ionFinishedList_;
    }
    IonBuilderList& ionLazyLinkList() {
        MOZ_ASSERT(isLocked());
        return ionLazyLinkList_;
    }

    AsmJSParallelTaskVector& asmJSWorklist() {
        MOZ_ASSERT(isLocked());
        return asmJSWorklist_;
    }
    AsmJSParallelTaskVector& asmJSFinishedList() {
        MOZ_ASSERT(isLocked());
        return asmJSFinishedList_;
    }

    ParseTaskVector& parseWorklist() {
        MOZ_ASSERT(isLocked());
        return parseWorklist_;
    }
    ParseTaskVector& parseFinishedList() {
        MOZ_ASSERT(isLocked());
        return parseFinishedList_;
    }
    ParseTaskVector& parseWaitingOnGC() {
        MOZ_ASSERT(isLocked());
        return parseWaitingOnGC_;
    }

    SourceCompressionTaskVector& compressionWorklist() {
        MOZ_ASSERT(isLocked());
        return compressionWorklist_;
    }

    GCHelperStateVector& gcHelperWorklist() {
        MOZ_ASSERT(isLocked());
        return gcHelperWorklist_;
    }

    GCParallelTaskVector& gcParallelWorklist() {
        MOZ_ASSERT(isLocked());
        return gcParallelWorklist_;
    }

    bool canStartAsmJSCompile();
    bool canStartIonCompile();
    bool canStartParseTask();
    bool canStartCompressionTask();
    bool canStartGCHelperTask();
    bool canStartGCParallelTask();

    // Unlike the methods above, the value returned by this method can change
    // over time, even if the helper thread state lock is held throughout.
    bool pendingIonCompileHasSufficientPriority();

    jit::IonBuilder* highestPriorityPendingIonCompile(bool remove = false);
    HelperThread* lowestPriorityUnpausedIonCompileAtThreshold();
    HelperThread* highestPriorityPausedIonCompile();

    uint32_t harvestFailedAsmJSJobs() {
        MOZ_ASSERT(isLocked());
        uint32_t n = numAsmJSFailedJobs;
        numAsmJSFailedJobs = 0;
        return n;
    }
    void noteAsmJSFailure(void* func) {
        // Be mindful to signal the main thread after calling this function.
        MOZ_ASSERT(isLocked());
        if (!asmJSFailedFunction)
            asmJSFailedFunction = func;
        numAsmJSFailedJobs++;
    }
    bool asmJSFailed() const {
        return bool(numAsmJSFailedJobs);
    }
    void resetAsmJSFailureState() {
        numAsmJSFailedJobs = 0;
        asmJSFailedFunction = nullptr;
    }
    void* maybeAsmJSFailedFunction() const {
        return asmJSFailedFunction;
    }

    JSScript* finishParseTask(JSContext* maybecx, JSRuntime* rt, void* token);
    void mergeParseTaskCompartment(JSRuntime* rt, ParseTask* parseTask,
                                   Handle<GlobalObject*> global,
                                   JSCompartment* dest);
    bool compressionInProgress(SourceCompressionTask* task);
    SourceCompressionTask* compressionTaskForSource(ScriptSource* ss);

  private:

    /*
     * Lock protecting all mutable shared state accessed by helper threads, and
     * used by all condition variables.
     */
    PRLock* helperLock;
#ifdef DEBUG
    PRThread* lockOwner;
#endif

    /* Condvars for threads waiting/notifying each other. */
    PRCondVar* consumerWakeup;
    PRCondVar* producerWakeup;
    PRCondVar* pauseWakeup;

    PRCondVar* whichWakeup(CondVar which) {
        switch (which) {
          case CONSUMER: return consumerWakeup;
          case PRODUCER: return producerWakeup;
          case PAUSE: return pauseWakeup;
          default: MOZ_CRASH();
        }
    }

    /*
     * Number of AsmJS jobs that encountered failure for the active module.
     * Their parent is logically the main thread, and this number serves for harvesting.
     */
    uint32_t numAsmJSFailedJobs;

    /*
     * Function index |i| in |Module.function(i)| of first failed AsmJS function.
     * -1 if no function has failed.
     */
    void* asmJSFailedFunction;
};

static inline GlobalHelperThreadState&
HelperThreadState()
{
    extern GlobalHelperThreadState* gHelperThreadState;

    MOZ_ASSERT(gHelperThreadState);
    return *gHelperThreadState;
}

/* Individual helper thread, one allocated per core. */
struct HelperThread
{
    mozilla::Maybe<PerThreadData> threadData;
    PRThread* thread;

    /*
     * Indicate to a thread that it should terminate itself. This is only read
     * or written with the helper thread state lock held.
     */
    bool terminate;

    /*
     * Indicate to a thread that it should pause execution. This is only
     * written with the helper thread state lock held, but may be read from
     * without the lock held.
     */
    mozilla::Atomic<bool, mozilla::Relaxed> pause;

    /* Any builder currently being compiled by Ion on this thread. */
    jit::IonBuilder* ionBuilder;

    /* Any AsmJS data currently being optimized by Ion on this thread. */
    AsmJSParallelTask* asmData;

    /* Any source being parsed/emitted on this thread. */
    ParseTask* parseTask;

    /* Any source being compressed on this thread. */
    SourceCompressionTask* compressionTask;

    /* Any GC state for background sweeping or allocating being performed. */
    GCHelperState* gcHelperState;

    /* State required to perform a GC parallel task. */
    GCParallelTask* gcParallelTask;

    bool idle() const {
        return !ionBuilder &&
               !asmData &&
               !parseTask &&
               !compressionTask &&
               !gcHelperState &&
               !gcParallelTask;
    }

    void destroy();

    void handleAsmJSWorkload();
    void handleIonWorkload();
    void handleParseWorkload();
    void handleCompressionWorkload();
    void handleGCHelperWorkload();
    void handleGCParallelWorkload();

    static void ThreadMain(void* arg);
    void threadLoop();
};

/* Methods for interacting with helper threads. */

// Create data structures used by helper threads.
bool
CreateHelperThreadsState();

// Destroy data structures used by helper threads.
void
DestroyHelperThreadsState();

// Initialize helper threads unless already initialized.
void
EnsureHelperThreadsInitialized();

// This allows the JS shell to override GetCPUCount() when passed the
// --thread-count=N option.
void
SetFakeCPUCount(size_t count);

// Pause the current thread until it's pause flag is unset.
void
PauseCurrentHelperThread();

/* Perform MIR optimization and LIR generation on a single function. */
bool
StartOffThreadAsmJSCompile(ExclusiveContext* cx, AsmJSParallelTask* asmData);

/*
 * Schedule an Ion compilation for a script, given a builder which has been
 * generated and read everything needed from the VM state.
 */
bool
StartOffThreadIonCompile(JSContext* cx, jit::IonBuilder* builder);

/*
 * Cancel a scheduled or in progress Ion compilation for script. If script is
 * nullptr, all compilations for the compartment are cancelled.
 */
void
CancelOffThreadIonCompile(JSCompartment* compartment, JSScript* script);

/* Cancel all scheduled, in progress or finished parses for runtime. */
void
CancelOffThreadParses(JSRuntime* runtime);

/*
 * Start a parse/emit cycle for a stream of source. The characters must stay
 * alive until the compilation finishes.
 */
bool
StartOffThreadParseScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                          const char16_t* chars, size_t length,
                          JS::OffThreadCompileCallback callback, void* callbackData);

/*
 * Called at the end of GC to enqueue any Parse tasks that were waiting on an
 * atoms-zone GC to finish.
 */
void
EnqueuePendingParseTasksAfterGC(JSRuntime* rt);

/* Start a compression job for the specified token. */
bool
StartOffThreadCompression(ExclusiveContext* cx, SourceCompressionTask* task);

class AutoLockHelperThreadState
{
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    explicit AutoLockHelperThreadState(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        HelperThreadState().lock();
    }

    ~AutoLockHelperThreadState() {
        HelperThreadState().unlock();
    }
};

class AutoUnlockHelperThreadState
{
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:

    explicit AutoUnlockHelperThreadState(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        HelperThreadState().unlock();
    }

    ~AutoUnlockHelperThreadState()
    {
        HelperThreadState().lock();
    }
};

struct AsmJSParallelTask
{
    JSRuntime* runtime;     // Associated runtime.
    LifoAlloc lifo;         // Provider of all heap memory used for compilation.
    void* func;             // Really, a ModuleCompiler::Func*
    jit::MIRGenerator* mir; // Passed from main thread to helper.
    jit::LIRGraph* lir;     // Passed from helper to main thread.
    unsigned compileTime;

    explicit AsmJSParallelTask(size_t defaultChunkSize)
      : runtime(nullptr), lifo(defaultChunkSize), func(nullptr), mir(nullptr), lir(nullptr), compileTime(0)
    { }

    void init(JSRuntime* rt, void* func, jit::MIRGenerator* mir) {
        this->runtime = rt;
        this->func = func;
        this->mir = mir;
        this->lir = nullptr;
    }
};

struct ParseTask
{
    ExclusiveContext* cx;
    OwningCompileOptions options;
    const char16_t* chars;
    size_t length;
    LifoAlloc alloc;

    // Rooted pointer to the global object used by 'cx'.
    PersistentRootedObject exclusiveContextGlobal;

    // Callback invoked off the main thread when the parse finishes.
    JS::OffThreadCompileCallback callback;
    void* callbackData;

    // Holds the final script between the invocation of the callback and the
    // point where FinishOffThreadScript is called, which will destroy the
    // ParseTask.
    PersistentRootedScript script;

    // Any errors or warnings produced during compilation. These are reported
    // when finishing the script.
    Vector<frontend::CompileError*> errors;
    bool overRecursed;

    ParseTask(ExclusiveContext* cx, JSObject* exclusiveContextGlobal,
              JSContext* initCx, const char16_t* chars, size_t length,
              JS::OffThreadCompileCallback callback, void* callbackData);
    bool init(JSContext* cx, const ReadOnlyCompileOptions& options);

    void activate(JSRuntime* rt);
    bool finish(JSContext* cx);

    bool runtimeMatches(JSRuntime* rt) {
        return exclusiveContextGlobal->runtimeFromAnyThread() == rt;
    }

    ~ParseTask();
};

// Return whether, if a new parse task was started, it would need to wait for
// an in-progress GC to complete before starting.
extern bool
OffThreadParsingMustWaitForGC(JSRuntime* rt);

// Compression tasks are allocated on the stack by their triggering thread,
// which will block on the compression completing as the task goes out of scope
// to ensure it completes at the required time.
struct SourceCompressionTask
{
    friend class ScriptSource;
    friend struct HelperThread;

    // Thread performing the compression.
    HelperThread* helperThread;

  private:
    // Context from the triggering thread. Don't use this off thread!
    ExclusiveContext* cx;

    ScriptSource* ss;

    // Atomic flag to indicate to a helper thread that it should abort
    // compression on the source.
    mozilla::Atomic<bool, mozilla::Relaxed> abort_;

    // Stores the result of the compression.
    enum ResultType {
        OOM,
        Aborted,
        Success
    } result;
    void* compressed;
    size_t compressedBytes;
    HashNumber compressedHash;

  public:
    explicit SourceCompressionTask(ExclusiveContext* cx)
      : helperThread(nullptr), cx(cx), ss(nullptr), abort_(false),
        result(OOM), compressed(nullptr), compressedBytes(0), compressedHash(0)
    {}

    ~SourceCompressionTask()
    {
        complete();
    }

    ResultType work();
    bool complete();
    void abort() { abort_ = true; }
    bool active() const { return !!ss; }
    ScriptSource* source() { return ss; }
};

} /* namespace js */

#endif /* vm_HelperThreads_h */
