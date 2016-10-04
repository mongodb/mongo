/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/HelperThreads.h"

#include "mozilla/DebugOnly.h"

#include "jsnativestack.h"
#include "jsnum.h" // For FIX_FPU()

#include "asmjs/WasmIonCompile.h"
#include "frontend/BytecodeCompiler.h"
#include "gc/GCInternals.h"
#include "jit/IonBuilder.h"
#include "vm/Debugger.h"
#include "vm/Time.h"
#include "vm/TraceLogging.h"

#include "jscntxtinlines.h"
#include "jscompartmentinlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::DebugOnly;

namespace js {

GlobalHelperThreadState* gHelperThreadState = nullptr;

} // namespace js

bool
js::CreateHelperThreadsState()
{
    MOZ_ASSERT(!gHelperThreadState);
    gHelperThreadState = js_new<GlobalHelperThreadState>();
    return gHelperThreadState != nullptr;
}

void
js::DestroyHelperThreadsState()
{
    MOZ_ASSERT(gHelperThreadState);
    gHelperThreadState->finish();
    js_delete(gHelperThreadState);
    gHelperThreadState = nullptr;
}

bool
js::EnsureHelperThreadsInitialized()
{
    MOZ_ASSERT(gHelperThreadState);
    return gHelperThreadState->ensureInitialized();
}

static size_t
ThreadCountForCPUCount(size_t cpuCount)
{
    // Create additional threads on top of the number of cores available, to
    // provide some excess capacity in case threads pause each other.
    static const uint32_t EXCESS_THREADS = 4;
    return cpuCount + EXCESS_THREADS;
}

void
js::SetFakeCPUCount(size_t count)
{
    // This must be called before the threads have been initialized.
    MOZ_ASSERT(!HelperThreadState().threads);

    HelperThreadState().cpuCount = count;
    HelperThreadState().threadCount = ThreadCountForCPUCount(count);
}

bool
js::StartOffThreadWasmCompile(ExclusiveContext* cx, wasm::CompileTask* task)
{
    AutoLockHelperThreadState lock;

    // Don't append this task if another failed.
    if (HelperThreadState().wasmFailed())
        return false;

    if (!HelperThreadState().wasmWorklist().append(task))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER);
    return true;
}

bool
js::StartOffThreadIonCompile(JSContext* cx, jit::IonBuilder* builder)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().ionWorklist().append(builder))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER);
    return true;
}

/*
 * Move an IonBuilder for which compilation has either finished, failed, or
 * been cancelled into the global finished compilation list. All off thread
 * compilations which are started must eventually be finished.
 */
static void
FinishOffThreadIonCompile(jit::IonBuilder* builder)
{
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!HelperThreadState().ionFinishedList().append(builder))
        oomUnsafe.crash("FinishOffThreadIonCompile");
}

static inline bool
CompiledScriptMatches(JSCompartment* compartment, JSScript* script, JSScript* target)
{
    if (script)
        return target == script;
    if (compartment)
        return target->compartment() == compartment;
    return true;
}

void
js::CancelOffThreadIonCompile(JSCompartment* compartment, JSScript* script)
{
    if (compartment && !compartment->jitCompartment())
        return;

    AutoLockHelperThreadState lock;

    if (!HelperThreadState().threads)
        return;

    /* Cancel any pending entries for which processing hasn't started. */
    GlobalHelperThreadState::IonBuilderVector& worklist = HelperThreadState().ionWorklist();
    for (size_t i = 0; i < worklist.length(); i++) {
        jit::IonBuilder* builder = worklist[i];
        if (CompiledScriptMatches(compartment, script, builder->script())) {
            FinishOffThreadIonCompile(builder);
            HelperThreadState().remove(worklist, &i);
        }
    }

    /* Wait for in progress entries to finish up. */
    for (size_t i = 0; i < HelperThreadState().threadCount; i++) {
        HelperThread& helper = HelperThreadState().threads[i];
        while (helper.ionBuilder() &&
               CompiledScriptMatches(compartment, script, helper.ionBuilder()->script()))
        {
            helper.ionBuilder()->cancel();
            if (helper.pause) {
                helper.pause = false;
                HelperThreadState().notifyAll(GlobalHelperThreadState::PAUSE);
            }
            HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
        }
    }

    /* Cancel code generation for any completed entries. */
    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList();
    for (size_t i = 0; i < finished.length(); i++) {
        jit::IonBuilder* builder = finished[i];
        if (CompiledScriptMatches(compartment, script, builder->script())) {
            jit::FinishOffThreadBuilder(nullptr, builder);
            HelperThreadState().remove(finished, &i);
        }
    }

    /* Cancel lazy linking for pending builders (attached to the ionScript). */
    jit::IonBuilder* builder = HelperThreadState().ionLazyLinkList().getFirst();
    while (builder) {
        jit::IonBuilder* next = builder->getNext();
        if (CompiledScriptMatches(compartment, script, builder->script())) {
            builder->script()->baselineScript()->removePendingIonBuilder(builder->script());
            jit::FinishOffThreadBuilder(nullptr, builder);
        }
        builder = next;
    }
}

static const JSClass parseTaskGlobalClass = {
    "internal-parse-task-global", JSCLASS_GLOBAL_FLAGS,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    JS_GlobalObjectTraceHook
};

ParseTask::ParseTask(ExclusiveContext* cx, JSObject* exclusiveContextGlobal, JSContext* initCx,
                     const char16_t* chars, size_t length,
                     JS::OffThreadCompileCallback callback, void* callbackData)
  : cx(cx), options(initCx), chars(chars), length(length),
    alloc(JSRuntime::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    exclusiveContextGlobal(initCx->runtime(), exclusiveContextGlobal),
    callback(callback), callbackData(callbackData),
    script(initCx->runtime()), sourceObject(initCx->runtime()),
    errors(cx), overRecursed(false)
{
}

bool
ParseTask::init(JSContext* cx, const ReadOnlyCompileOptions& options)
{
    if (!this->options.copy(cx, options))
        return false;

    return true;
}

void
ParseTask::activate(JSRuntime* rt)
{
    rt->setUsedByExclusiveThread(exclusiveContextGlobal->zone());
    cx->enterCompartment(exclusiveContextGlobal->compartment());
}

bool
ParseTask::finish(JSContext* cx)
{
    if (sourceObject) {
        RootedScriptSource sso(cx, sourceObject);
        if (!ScriptSourceObject::initFromOptions(cx, sso, options))
            return false;
    }

    return true;
}

ParseTask::~ParseTask()
{
    // ParseTask takes over ownership of its input exclusive context.
    js_delete(cx);

    for (size_t i = 0; i < errors.length(); i++)
        js_delete(errors[i]);
}

void
js::CancelOffThreadParses(JSRuntime* rt)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().threads)
        return;

    // Instead of forcibly canceling pending parse tasks, just wait for all scheduled
    // and in progress ones to complete. Otherwise the final GC may not collect
    // everything due to zones being used off thread.
    while (true) {
        bool pending = false;
        GlobalHelperThreadState::ParseTaskVector& worklist = HelperThreadState().parseWorklist();
        for (size_t i = 0; i < worklist.length(); i++) {
            ParseTask* task = worklist[i];
            if (task->runtimeMatches(rt))
                pending = true;
        }
        if (!pending) {
            bool inProgress = false;
            for (size_t i = 0; i < HelperThreadState().threadCount; i++) {
                ParseTask* task = HelperThreadState().threads[i].parseTask();
                if (task && task->runtimeMatches(rt))
                    inProgress = true;
            }
            if (!inProgress)
                break;
        }
        HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
    }

    // Clean up any parse tasks which haven't been finished by the main thread.
    GlobalHelperThreadState::ParseTaskVector& finished = HelperThreadState().parseFinishedList();
    while (true) {
        bool found = false;
        for (size_t i = 0; i < finished.length(); i++) {
            ParseTask* task = finished[i];
            if (task->runtimeMatches(rt)) {
                found = true;
                AutoUnlockHelperThreadState unlock;
                HelperThreadState().finishParseTask(/* maybecx = */ nullptr, rt, task);
            }
        }
        if (!found)
            break;
    }
}

bool
js::OffThreadParsingMustWaitForGC(JSRuntime* rt)
{
    // Off thread parsing can't occur during incremental collections on the
    // atoms compartment, to avoid triggering barriers. (Outside the atoms
    // compartment, the compilation will use a new zone that is never
    // collected.) If an atoms-zone GC is in progress, hold off on executing the
    // parse task until the atoms-zone GC completes (see
    // EnqueuePendingParseTasksAfterGC).
    return rt->activeGCInAtomsZone();
}

static bool
EnsureConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key)
{
    if (!GlobalObject::ensureConstructor(cx, global, key))
        return false;

    MOZ_ASSERT(global->getPrototype(key).toObject().isDelegate(),
               "standard class prototype wasn't a delegate from birth");
    return true;
}

// Initialize all classes potentially created during parsing for use in parser
// data structures, template objects, &c.
static bool
EnsureParserCreatedClasses(JSContext* cx)
{
    Handle<GlobalObject*> global = cx->global();

    if (!EnsureConstructor(cx, global, JSProto_Function))
        return false; // needed by functions, also adds object literals' proto

    if (!EnsureConstructor(cx, global, JSProto_Array))
        return false; // needed by array literals

    if (!EnsureConstructor(cx, global, JSProto_RegExp))
        return false; // needed by regular expression literals

    if (!EnsureConstructor(cx, global, JSProto_Iterator))
        return false; // needed by ???

    if (!GlobalObject::initStarGenerators(cx, global))
        return false; // needed by function*() {} and generator comprehensions

    return true;
}

bool
js::StartOffThreadParseScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                              const char16_t* chars, size_t length,
                              JS::OffThreadCompileCallback callback, void* callbackData)
{
    // Suppress GC so that calls below do not trigger a new incremental GC
    // which could require barriers on the atoms compartment.
    gc::AutoSuppressGC suppress(cx);

    JS::CompartmentOptions compartmentOptions(cx->compartment()->options());
    compartmentOptions.setZone(JS::FreshZone);
    compartmentOptions.setInvisibleToDebugger(true);
    compartmentOptions.setMergeable(true);

    // Don't falsely inherit the host's global trace hook.
    compartmentOptions.setTrace(nullptr);

    JSObject* global = JS_NewGlobalObject(cx, &parseTaskGlobalClass, nullptr,
                                          JS::FireOnNewGlobalHook, compartmentOptions);
    if (!global)
        return false;

    JS_SetCompartmentPrincipals(global->compartment(), cx->compartment()->principals());

    // Initialize all classes required for parsing while still on the main
    // thread, for both the target and the new global so that prototype
    // pointers can be changed infallibly after parsing finishes.
    if (!EnsureParserCreatedClasses(cx))
        return false;
    {
        AutoCompartment ac(cx, global);
        if (!EnsureParserCreatedClasses(cx))
            return false;
    }

    ScopedJSDeletePtr<ExclusiveContext> helpercx(
        cx->new_<ExclusiveContext>(cx->runtime(), (PerThreadData*) nullptr,
                                   ExclusiveContext::Context_Exclusive));
    if (!helpercx)
        return false;

    ScopedJSDeletePtr<ParseTask> task(
        cx->new_<ParseTask>(helpercx.get(), global, cx, chars, length,
                            callback, callbackData));
    if (!task)
        return false;

    helpercx.forget();

    if (!task->init(cx, options))
        return false;

    if (OffThreadParsingMustWaitForGC(cx->runtime())) {
        AutoLockHelperThreadState lock;
        if (!HelperThreadState().parseWaitingOnGC().append(task.get())) {
            ReportOutOfMemory(cx);
            return false;
        }
    } else {
        AutoLockHelperThreadState lock;
        if (!HelperThreadState().parseWorklist().append(task.get())) {
            ReportOutOfMemory(cx);
            return false;
        }

        task->activate(cx->runtime());
        HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER);
    }

    task.forget();

    return true;
}

void
js::EnqueuePendingParseTasksAfterGC(JSRuntime* rt)
{
    MOZ_ASSERT(!OffThreadParsingMustWaitForGC(rt));

    GlobalHelperThreadState::ParseTaskVector newTasks;
    {
        AutoLockHelperThreadState lock;
        GlobalHelperThreadState::ParseTaskVector& waiting = HelperThreadState().parseWaitingOnGC();

        for (size_t i = 0; i < waiting.length(); i++) {
            ParseTask* task = waiting[i];
            if (task->runtimeMatches(rt)) {
                AutoEnterOOMUnsafeRegion oomUnsafe;
                if (!newTasks.append(task))
                    oomUnsafe.crash("EnqueuePendingParseTasksAfterGC");
                HelperThreadState().remove(waiting, &i);
            }
        }
    }

    if (newTasks.empty())
        return;

    // This logic should mirror the contents of the !activeGCInAtomsZone()
    // branch in StartOffThreadParseScript:

    for (size_t i = 0; i < newTasks.length(); i++)
        newTasks[i]->activate(rt);

    AutoLockHelperThreadState lock;

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!HelperThreadState().parseWorklist().appendAll(newTasks))
            oomUnsafe.crash("EnqueuePendingParseTasksAfterGC");
    }

    HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER);
}

static const uint32_t kDefaultHelperStackSize = 2048 * 1024;
static const uint32_t kDefaultHelperStackQuota = 1800 * 1024;

// TSan enforces a minimum stack size that's just slightly larger than our
// default helper stack size.  It does this to store blobs of TSan-specific
// data on each thread's stack.  Unfortunately, that means that even though
// we'll actually receive a larger stack than we requested, the effective
// usable space of that stack is significantly less than what we expect.
// To offset TSan stealing our stack space from underneath us, double the
// default.
//
// Note that we don't need this for ASan/MOZ_ASAN because ASan doesn't
// require all the thread-specific state that TSan does.
#if defined(MOZ_TSAN)
static const uint32_t HELPER_STACK_SIZE = 2 * kDefaultHelperStackSize;
static const uint32_t HELPER_STACK_QUOTA = 2 * kDefaultHelperStackQuota;
#else
static const uint32_t HELPER_STACK_SIZE = kDefaultHelperStackSize;
static const uint32_t HELPER_STACK_QUOTA = kDefaultHelperStackQuota;
#endif

bool
GlobalHelperThreadState::ensureInitialized()
{
    MOZ_ASSERT(CanUseExtraThreads());

    MOZ_ASSERT(this == &HelperThreadState());
    AutoLockHelperThreadState lock;

    if (threads)
        return true;

    threads = js_pod_calloc<HelperThread>(threadCount);
    if (!threads)
        return false;

    for (size_t i = 0; i < threadCount; i++) {
        HelperThread& helper = threads[i];
        helper.threadData.emplace(static_cast<JSRuntime*>(nullptr));
        helper.thread = PR_CreateThread(PR_USER_THREAD,
                                        HelperThread::ThreadMain, &helper,
                                        PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, HELPER_STACK_SIZE);
        if (!helper.thread || !helper.threadData->init()) {
            finishThreads();
            return false;
        }
    }

    return true;
}

GlobalHelperThreadState::GlobalHelperThreadState()
 : cpuCount(0),
   threadCount(0),
   threads(nullptr),
   wasmCompilationInProgress(false),
   numWasmFailedJobs(0),
   helperLock(nullptr),
   consumerWakeup(nullptr),
   producerWakeup(nullptr),
   pauseWakeup(nullptr)
{
    cpuCount = GetCPUCount();
    threadCount = ThreadCountForCPUCount(cpuCount);

    MOZ_ASSERT(cpuCount > 0, "GetCPUCount() seems broken");

    helperLock = PR_NewLock();
    consumerWakeup = PR_NewCondVar(helperLock);
    producerWakeup = PR_NewCondVar(helperLock);
    pauseWakeup = PR_NewCondVar(helperLock);
}

void
GlobalHelperThreadState::finish()
{
    finishThreads();

    PR_DestroyCondVar(consumerWakeup);
    PR_DestroyCondVar(producerWakeup);
    PR_DestroyCondVar(pauseWakeup);
    PR_DestroyLock(helperLock);

    ionLazyLinkList_.clear();
}

void
GlobalHelperThreadState::finishThreads()
{
    if (!threads)
        return;

    MOZ_ASSERT(CanUseExtraThreads());
    for (size_t i = 0; i < threadCount; i++)
        threads[i].destroy();
    js_free(threads);
    threads = nullptr;
}

void
GlobalHelperThreadState::lock()
{
    MOZ_ASSERT(!isLocked());
    AssertCurrentThreadCanLock(HelperThreadStateLock);
    PR_Lock(helperLock);
#ifdef DEBUG
    lockOwner.value = PR_GetCurrentThread();
#endif
}

void
GlobalHelperThreadState::unlock()
{
    MOZ_ASSERT(isLocked());
#ifdef DEBUG
    lockOwner.value = nullptr;
#endif
    PR_Unlock(helperLock);
}

#ifdef DEBUG
bool
GlobalHelperThreadState::isLocked()
{
    return lockOwner.value == PR_GetCurrentThread();
}
#endif

void
GlobalHelperThreadState::wait(CondVar which, uint32_t millis)
{
    MOZ_ASSERT(isLocked());
#ifdef DEBUG
    lockOwner.value = nullptr;
#endif
    DebugOnly<PRStatus> status =
        PR_WaitCondVar(whichWakeup(which),
                       millis ? PR_MillisecondsToInterval(millis) : PR_INTERVAL_NO_TIMEOUT);
    MOZ_ASSERT(status == PR_SUCCESS);
#ifdef DEBUG
    lockOwner.value = PR_GetCurrentThread();
#endif
}

void
GlobalHelperThreadState::notifyAll(CondVar which)
{
    MOZ_ASSERT(isLocked());
    PR_NotifyAllCondVar(whichWakeup(which));
}

void
GlobalHelperThreadState::notifyOne(CondVar which)
{
    MOZ_ASSERT(isLocked());
    PR_NotifyCondVar(whichWakeup(which));
}

bool
GlobalHelperThreadState::hasActiveThreads()
{
    MOZ_ASSERT(isLocked());
    if (!threads)
        return false;

    for (size_t i = 0; i < threadCount; i++) {
        if (!threads[i].idle())
            return true;
    }

    return false;
}

void
GlobalHelperThreadState::waitForAllThreads()
{
    CancelOffThreadIonCompile(nullptr, nullptr);

    AutoLockHelperThreadState lock;
    while (hasActiveThreads())
        wait(CONSUMER);
}

template <typename T>
bool
GlobalHelperThreadState::checkTaskThreadLimit(size_t maxThreads) const
{
    if (maxThreads >= threadCount)
        return true;

    size_t count = 0;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].currentTask.isSome() && threads[i].currentTask->is<T>())
            count++;
        if (count >= maxThreads)
            return false;
    }

    return true;
}

static inline bool
IsHelperThreadSimulatingOOM(js::oom::ThreadType threadType)
{
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    return js::oom::targetThread == threadType;
#else
    return false;
#endif
}

size_t
GlobalHelperThreadState::maxIonCompilationThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_ION))
        return 1;
    return threadCount;
}

size_t
GlobalHelperThreadState::maxUnpausedIonCompilationThreads() const
{
    return 1;
}

size_t
GlobalHelperThreadState::maxWasmCompilationThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_ASMJS))
        return 1;
    if (cpuCount < 2)
        return 2;
    return cpuCount;
}

size_t
GlobalHelperThreadState::maxParseThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_PARSE))
        return 1;

    // Don't allow simultaneous off thread parses, to reduce contention on the
    // atoms table. Note that asm.js compilation depends on this to avoid
    // stalling the helper thread, as off thread parse tasks can trigger and
    // block on other off thread asm.js compilation tasks.
    return 1;
}

size_t
GlobalHelperThreadState::maxCompressionThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_COMPRESS))
        return 1;
    return threadCount;
}

size_t
GlobalHelperThreadState::maxGCHelperThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_GCHELPER))
        return 1;
    return threadCount;
}

size_t
GlobalHelperThreadState::maxGCParallelThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::oom::THREAD_TYPE_GCPARALLEL))
        return 1;
    return threadCount;
}

bool
GlobalHelperThreadState::canStartWasmCompile()
{
    // Don't execute an wasm job if an earlier one failed.
    MOZ_ASSERT(isLocked());
    if (wasmWorklist().empty() || numWasmFailedJobs)
        return false;

    // Honor the maximum allowed threads to compile wasm jobs at once,
    // to avoid oversaturating the machine.
    if (!checkTaskThreadLimit<wasm::CompileTask*>(maxWasmCompilationThreads()))
        return false;

    return true;
}

static bool
IonBuilderHasHigherPriority(jit::IonBuilder* first, jit::IonBuilder* second)
{
    // This method can return whatever it wants, though it really ought to be a
    // total order. The ordering is allowed to race (change on the fly), however.

    // A lower optimization level indicates a higher priority.
    if (first->optimizationInfo().level() != second->optimizationInfo().level())
        return first->optimizationInfo().level() < second->optimizationInfo().level();

    // A script without an IonScript has precedence on one with.
    if (first->scriptHasIonScript() != second->scriptHasIonScript())
        return !first->scriptHasIonScript();

    // A higher warm-up counter indicates a higher priority.
    return first->script()->getWarmUpCount() / first->script()->length() >
           second->script()->getWarmUpCount() / second->script()->length();
}

bool
GlobalHelperThreadState::canStartIonCompile()
{
    return !ionWorklist().empty() &&
           checkTaskThreadLimit<jit::IonBuilder*>(maxIonCompilationThreads());
}

jit::IonBuilder*
GlobalHelperThreadState::highestPriorityPendingIonCompile(bool remove /* = false */)
{
    MOZ_ASSERT(isLocked());

    if (ionWorklist().empty()) {
        MOZ_ASSERT(!remove);
        return nullptr;
    }

    // Get the highest priority IonBuilder which has not started compilation yet.
    size_t index = 0;
    for (size_t i = 1; i < ionWorklist().length(); i++) {
        if (IonBuilderHasHigherPriority(ionWorklist()[i], ionWorklist()[index]))
            index = i;
    }
    jit::IonBuilder* builder = ionWorklist()[index];
    if (remove)
        ionWorklist().erase(&ionWorklist()[index]);
    return builder;
}

HelperThread*
GlobalHelperThreadState::lowestPriorityUnpausedIonCompileAtThreshold()
{
    MOZ_ASSERT(isLocked());

    // Get the lowest priority IonBuilder which has started compilation and
    // isn't paused, unless there are still fewer than the maximum number of
    // such builders permitted.
    size_t numBuilderThreads = 0;
    HelperThread* thread = nullptr;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].ionBuilder() && !threads[i].pause) {
            numBuilderThreads++;
            if (!thread || IonBuilderHasHigherPriority(thread->ionBuilder(), threads[i].ionBuilder()))
                thread = &threads[i];
        }
    }
    if (numBuilderThreads < maxUnpausedIonCompilationThreads())
        return nullptr;
    return thread;
}

HelperThread*
GlobalHelperThreadState::highestPriorityPausedIonCompile()
{
    MOZ_ASSERT(isLocked());

    // Get the highest priority IonBuilder which has started compilation but
    // which was subsequently paused.
    HelperThread* thread = nullptr;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].pause) {
            // Currently, only threads with IonBuilders can be paused.
            MOZ_ASSERT(threads[i].ionBuilder());
            if (!thread || IonBuilderHasHigherPriority(threads[i].ionBuilder(), thread->ionBuilder()))
                thread = &threads[i];
        }
    }
    return thread;
}

bool
GlobalHelperThreadState::pendingIonCompileHasSufficientPriority()
{
    MOZ_ASSERT(isLocked());

    // Can't compile anything if there are no scripts to compile.
    if (!canStartIonCompile())
        return false;

    // Count the number of threads currently compiling scripts, and look for
    // the thread with the lowest priority.
    HelperThread* lowestPriorityThread = lowestPriorityUnpausedIonCompileAtThreshold();

    // If the number of threads building scripts is less than the maximum, the
    // compilation can start immediately.
    if (!lowestPriorityThread)
        return true;

    // If there is a builder in the worklist with higher priority than some
    // builder currently being compiled, then that current compilation can be
    // paused, so allow the compilation.
    if (IonBuilderHasHigherPriority(highestPriorityPendingIonCompile(),
                                    lowestPriorityThread->ionBuilder()))
        return true;

    // Compilation will have to wait until one of the active compilations finishes.
    return false;
}

bool
GlobalHelperThreadState::canStartParseTask()
{
    MOZ_ASSERT(isLocked());
    return !parseWorklist().empty() && checkTaskThreadLimit<ParseTask*>(maxParseThreads());
}

bool
GlobalHelperThreadState::canStartCompressionTask()
{
    return !compressionWorklist().empty() &&
           checkTaskThreadLimit<SourceCompressionTask*>(maxCompressionThreads());
}

bool
GlobalHelperThreadState::canStartGCHelperTask()
{
    return !gcHelperWorklist().empty() &&
           checkTaskThreadLimit<GCHelperState*>(maxGCHelperThreads());
}

bool
GlobalHelperThreadState::canStartGCParallelTask()
{
    return !gcParallelWorklist().empty() &&
           checkTaskThreadLimit<GCParallelTask*>(maxGCParallelThreads());
}

js::GCParallelTask::~GCParallelTask()
{
    // Only most-derived classes' destructors may do the join: base class
    // destructors run after those for derived classes' members, so a join in a
    // base class can't ensure that the task is done using the members. All we
    // can do now is check that someone has previously stopped the task.
#ifdef DEBUG
    AutoLockHelperThreadState helperLock;
    MOZ_ASSERT(state == NotStarted);
#endif
}

bool
js::GCParallelTask::startWithLockHeld()
{
    MOZ_ASSERT(HelperThreadState().isLocked());

    // Tasks cannot be started twice.
    MOZ_ASSERT(state == NotStarted);

    // If we do the shutdown GC before running anything, we may never
    // have initialized the helper threads. Just use the serial path
    // since we cannot safely intialize them at this point.
    if (!HelperThreadState().threads)
        return false;

    if (!HelperThreadState().gcParallelWorklist().append(this))
        return false;
    state = Dispatched;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER);

    return true;
}

bool
js::GCParallelTask::start()
{
    AutoLockHelperThreadState helperLock;
    return startWithLockHeld();
}

void
js::GCParallelTask::joinWithLockHeld()
{
    MOZ_ASSERT(HelperThreadState().isLocked());

    if (state == NotStarted)
        return;

    while (state != Finished)
        HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
    state = NotStarted;
    cancel_ = false;
}

void
js::GCParallelTask::join()
{
    AutoLockHelperThreadState helperLock;
    joinWithLockHeld();
}

void
js::GCParallelTask::runFromMainThread(JSRuntime* rt)
{
    MOZ_ASSERT(state == NotStarted);
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(rt));
    uint64_t timeStart = PRMJ_Now();
    run();
    duration_ = PRMJ_Now() - timeStart;
}

void
js::GCParallelTask::runFromHelperThread()
{
    MOZ_ASSERT(HelperThreadState().isLocked());

    {
        AutoUnlockHelperThreadState parallelSection;
        uint64_t timeStart = PRMJ_Now();
        run();
        duration_ = PRMJ_Now() - timeStart;
    }

    state = Finished;
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

bool
js::GCParallelTask::isRunning() const
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    return state == Dispatched;
}

void
HelperThread::handleGCParallelWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartGCParallelTask());
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().gcParallelWorklist().popCopy());
    gcParallelTask()->runFromHelperThread();
    currentTask.reset();
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

static void
LeaveParseTaskZone(JSRuntime* rt, ParseTask* task)
{
    // Mark the zone as no longer in use by an ExclusiveContext, and available
    // to be collected by the GC.
    task->cx->leaveCompartment(task->cx->compartment());
    rt->clearUsedByExclusiveThread(task->cx->zone());
}

JSScript*
GlobalHelperThreadState::finishParseTask(JSContext* maybecx, JSRuntime* rt, void* token)
{
    ScopedJSDeletePtr<ParseTask> parseTask;

    // The token is a ParseTask* which should be in the finished list.
    // Find and remove its entry.
    {
        AutoLockHelperThreadState lock;
        ParseTaskVector& finished = parseFinishedList();
        for (size_t i = 0; i < finished.length(); i++) {
            if (finished[i] == token) {
                parseTask = finished[i];
                remove(finished, &i);
                break;
            }
        }
    }
    MOZ_ASSERT(parseTask);

    if (!maybecx) {
        LeaveParseTaskZone(rt, parseTask);
        return nullptr;
    }

    JSContext* cx = maybecx;
    MOZ_ASSERT(cx->compartment());

    // Make sure we have all the constructors we need for the prototype
    // remapping below, since we can't GC while that's happening.
    Rooted<GlobalObject*> global(cx, &cx->global()->as<GlobalObject>());
    if (!EnsureParserCreatedClasses(cx)) {
        LeaveParseTaskZone(rt, parseTask);
        return nullptr;
    }

    mergeParseTaskCompartment(rt, parseTask, global, cx->compartment());

    if (!parseTask->finish(cx))
        return nullptr;

    RootedScript script(rt, parseTask->script);
    assertSameCompartment(cx, script);

    // Report any error or warnings generated during the parse, and inform the
    // debugger about the compiled scripts.
    for (size_t i = 0; i < parseTask->errors.length(); i++)
        parseTask->errors[i]->throwError(cx);
    if (parseTask->overRecursed)
        ReportOverRecursed(cx);
    if (cx->isExceptionPending())
        return nullptr;

    if (!script) {
        // No error was reported, but no script produced. Assume we hit out of
        // memory.
        ReportOutOfMemory(cx);
        return nullptr;
    }

    // The Debugger only needs to be told about the topmost script that was compiled.
    Debugger::onNewScript(cx, script);

    // Update the compressed source table with the result. This is normally
    // called by setCompressedSource when compilation occurs on the main thread.
    if (script->scriptSource()->hasCompressedSource())
        script->scriptSource()->updateCompressedSourceSet(rt);

    return script;
}

JSObject*
GlobalObject::getStarGeneratorFunctionPrototype()
{
    const Value& v = getReservedSlot(STAR_GENERATOR_FUNCTION_PROTO);
    return v.isObject() ? &v.toObject() : nullptr;
}

void
GlobalHelperThreadState::mergeParseTaskCompartment(JSRuntime* rt, ParseTask* parseTask,
                                                   Handle<GlobalObject*> global,
                                                   JSCompartment* dest)
{
    // After we call LeaveParseTaskZone() it's not safe to GC until we have
    // finished merging the contents of the parse task's compartment into the
    // destination compartment.  Finish any ongoing incremental GC first and
    // assert that no allocation can occur.
    gc::AutoFinishGC finishGC(rt);
    JS::AutoAssertNoAlloc noAlloc(rt);

    LeaveParseTaskZone(rt, parseTask);

    {
        gc::ZoneCellIter iter(parseTask->cx->zone(), gc::AllocKind::OBJECT_GROUP);

        // Generator functions don't have Function.prototype as prototype but a
        // different function object, so the IdentifyStandardPrototype trick
        // below won't work.  Just special-case it.
        JSObject* parseTaskStarGenFunctionProto =
            parseTask->exclusiveContextGlobal->as<GlobalObject>().getStarGeneratorFunctionPrototype();

        // Point the prototypes of any objects in the script's compartment to refer
        // to the corresponding prototype in the new compartment. This will briefly
        // create cross compartment pointers, which will be fixed by the
        // MergeCompartments call below.
        for (; !iter.done(); iter.next()) {
            ObjectGroup* group = iter.get<ObjectGroup>();
            TaggedProto proto(group->proto());
            if (!proto.isObject())
                continue;

            JSObject* protoObj = proto.toObject();

            JSObject* newProto;
            if (protoObj == parseTaskStarGenFunctionProto) {
                newProto = global->getStarGeneratorFunctionPrototype();
            } else {
                JSProtoKey key = JS::IdentifyStandardPrototype(protoObj);
                if (key == JSProto_Null)
                    continue;

                MOZ_ASSERT(key == JSProto_Object || key == JSProto_Array ||
                           key == JSProto_Function || key == JSProto_RegExp ||
                           key == JSProto_Iterator);

                newProto = GetBuiltinPrototypePure(global, key);
            }

            MOZ_ASSERT(newProto);
            group->setProtoUnchecked(TaggedProto(newProto));
        }
    }

    // Move the parsed script and all its contents into the desired compartment.
    gc::MergeCompartments(parseTask->cx->compartment(), dest);
}

void
HelperThread::destroy()
{
    if (thread) {
        {
            AutoLockHelperThreadState lock;
            terminate = true;

            /* Notify all helpers, to ensure that this thread wakes up. */
            HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER);
        }

        PR_JoinThread(thread);
    }

    threadData.reset();
}

#ifdef MOZ_NUWA_PROCESS
extern "C" {
MFBT_API bool IsNuwaProcess();
MFBT_API void NuwaMarkCurrentThread(void (*recreate)(void*), void* arg);
}
#endif

/* static */
void
HelperThread::ThreadMain(void* arg)
{
    PR_SetCurrentThreadName("JS Helper");

#ifdef MOZ_NUWA_PROCESS
    if (IsNuwaProcess()) {
        MOZ_ASSERT(NuwaMarkCurrentThread != nullptr);
        NuwaMarkCurrentThread(nullptr, nullptr);
    }
#endif

    //See bug 1104658.
    //Set the FPU control word to be the same as the main thread's, or math
    //computations on this thread may use incorrect precision rules during
    //Ion compilation.
    FIX_FPU();

    static_cast<HelperThread*>(arg)->threadLoop();
}

void
HelperThread::handleWasmWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartWasmCompile());
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().wasmWorklist().popCopy());
    bool success = false;

    wasm::CompileTask* task = wasmTask();
    {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(), task->args().runtime);
        success = wasm::CompileFunction(task);
    }

    // On success, try to move work to the finished list.
    if (success)
        success = HelperThreadState().wasmFinishedList().append(task);

    // On failure, note the failure for harvesting by the parent.
    if (!success)
        HelperThreadState().noteWasmFailure();

    // Notify the main thread in case it's waiting.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
    currentTask.reset();
}

void
HelperThread::handleIonWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartIonCompile());
    MOZ_ASSERT(idle());

    // Find the IonBuilder in the worklist with the highest priority, and
    // remove it from the worklist.
    jit::IonBuilder* builder =
        HelperThreadState().highestPriorityPendingIonCompile(/* remove = */ true);

    // If there are now too many threads with active IonBuilders, indicate to
    // the one with the lowest priority that it should pause. Note that due to
    // builder priorities changing since pendingIonCompileHasSufficientPriority
    // was called, the builder we are pausing may actually be higher priority
    // than the one we are about to start. Oh well.
    if (HelperThread* other = HelperThreadState().lowestPriorityUnpausedIonCompileAtThreshold()) {
        MOZ_ASSERT(other->ionBuilder() && !other->pause);
        other->pause = true;
    }

    currentTask.emplace(builder);
    builder->setPauseFlag(&pause);

    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, builder->script());
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonCompilation);

    JSRuntime* rt = builder->script()->compartment()->runtimeFromAnyThread();

    {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(),
                                              builder->script()->runtimeFromAnyThread());
        jit::JitContext jctx(jit::CompileRuntime::get(rt),
                             jit::CompileCompartment::get(builder->script()->compartment()),
                             &builder->alloc());
        builder->setBackgroundCodegen(jit::CompileBackEnd(builder));
    }

    FinishOffThreadIonCompile(builder);
    currentTask.reset();
    pause = false;

    // Ping the main thread so that the compiled code can be incorporated
    // at the next interrupt callback. Don't interrupt Ion code for this, as
    // this incorporation can be delayed indefinitely without affecting
    // performance as long as the main thread is actually executing Ion code.
    rt->requestInterrupt(JSRuntime::RequestInterruptCanWait);

    // Notify the main thread in case it is waiting for the compilation to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);

    // When finishing Ion compilation jobs, we can start unpausing compilation
    // threads that were paused to restrict the number of active compilations.
    // Only unpause one at a time, to make sure we don't exceed the restriction.
    // Since threads are currently only paused for Ion compilations, this
    // strategy will eventually unpause all paused threads, regardless of how
    // many there are, since each thread we unpause will eventually finish and
    // end up back here.
    if (HelperThread* other = HelperThreadState().highestPriorityPausedIonCompile()) {
        MOZ_ASSERT(other->ionBuilder() && other->pause);

        // Only unpause the other thread if there isn't a higher priority
        // builder which this thread or another can start on.
        jit::IonBuilder* builder = HelperThreadState().highestPriorityPendingIonCompile();
        if (!builder || IonBuilderHasHigherPriority(other->ionBuilder(), builder)) {
            other->pause = false;

            // Notify all paused threads, to make sure the one we just
            // unpaused wakes up.
            HelperThreadState().notifyAll(GlobalHelperThreadState::PAUSE);
        }
    }
}

static HelperThread*
CurrentHelperThread()
{
    PRThread* prThread = PR_GetCurrentThread();
    HelperThread* thread = nullptr;
    for (size_t i = 0; i < HelperThreadState().threadCount; i++) {
        if (prThread == HelperThreadState().threads[i].thread) {
            thread = &HelperThreadState().threads[i];
            break;
        }
    }
    MOZ_ASSERT(thread);
    return thread;
}

void
js::PauseCurrentHelperThread()
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog logPaused(logger, TraceLogger_IonCompilationPaused);

    HelperThread* thread = CurrentHelperThread();

    AutoLockHelperThreadState lock;
    while (thread->pause)
        HelperThreadState().wait(GlobalHelperThreadState::PAUSE);
}

void
ExclusiveContext::setHelperThread(HelperThread* thread)
{
    helperThread_ = thread;
    perThreadData = thread->threadData.ptr();
}

frontend::CompileError&
ExclusiveContext::addPendingCompileError()
{
    frontend::CompileError* error = js_new<frontend::CompileError>();
    if (!error)
        MOZ_CRASH();
    if (!helperThread()->parseTask()->errors.append(error))
        MOZ_CRASH();
    return *error;
}

void
ExclusiveContext::addPendingOverRecursed()
{
    if (helperThread()->parseTask())
        helperThread()->parseTask()->overRecursed = true;
}

void
HelperThread::handleParseWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartParseTask());
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().parseWorklist().popCopy());
    ParseTask* task = parseTask();
    task->cx->setHelperThread(this);

    {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(),
                                              task->exclusiveContextGlobal->runtimeFromAnyThread());
        SourceBufferHolder srcBuf(task->chars, task->length,
                                  SourceBufferHolder::NoOwnership);

        // ! WARNING WARNING WARNING !
        //
        // See comment in Parser::bindLexical about optimizing global lexical
        // bindings. If we start optimizing them, passing in task->cx's
        // global lexical scope would be incorrect!
        //
        // ! WARNING WARNING WARNING !
        ExclusiveContext* parseCx = task->cx;
        Rooted<ClonedBlockObject*> globalLexical(parseCx, &parseCx->global()->lexicalScope());
        Rooted<ScopeObject*> staticScope(parseCx, &globalLexical->staticBlock());
        task->script = frontend::CompileScript(parseCx, &task->alloc,
                                               globalLexical, staticScope, nullptr,
                                               task->options, srcBuf,
                                               /* source_ = */ nullptr,
                                               /* extraSct = */ nullptr,
                                               /* sourceObjectOut = */ task->sourceObject.address());
    }

    // The callback is invoked while we are still off the main thread.
    task->callback(task, task->callbackData);

    // FinishOffThreadScript will need to be called on the script to
    // migrate it into the correct compartment.
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!HelperThreadState().parseFinishedList().append(task))
            oomUnsafe.crash("handleParseWorkload");
    }

    currentTask.reset();

    // Notify the main thread in case it is waiting for the parse/emit to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

void
HelperThread::handleCompressionWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartCompressionTask());
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().compressionWorklist().popCopy());
    SourceCompressionTask* task = compressionTask();
    task->helperThread = this;

    {
        AutoUnlockHelperThreadState unlock;
        task->result = task->work();
    }

    task->helperThread = nullptr;
    currentTask.reset();

    // Notify the main thread in case it is waiting for the compression to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

bool
js::StartOffThreadCompression(ExclusiveContext* cx, SourceCompressionTask* task)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().compressionWorklist().append(task)) {
        if (JSContext* maybecx = cx->maybeJSContext())
            ReportOutOfMemory(maybecx);
        return false;
    }

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER);
    return true;
}

bool
GlobalHelperThreadState::compressionInProgress(SourceCompressionTask* task)
{
    MOZ_ASSERT(isLocked());
    for (size_t i = 0; i < compressionWorklist().length(); i++) {
        if (compressionWorklist()[i] == task)
            return true;
    }
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].compressionTask() == task)
            return true;
    }
    return false;
}

bool
SourceCompressionTask::complete()
{
    if (!active()) {
        MOZ_ASSERT(!compressed);
        return true;
    }

    {
        AutoLockHelperThreadState lock;
        while (HelperThreadState().compressionInProgress(this))
            HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
    }

    if (result == Success) {
        ss->setCompressedSource(cx->isJSContext() ? cx->asJSContext()->runtime() : nullptr,
                                compressed, compressedBytes, compressedHash);

        // Update memory accounting.
        cx->updateMallocCounter(ss->computedSizeOfData());
    } else {
        js_free(compressed);

        if (result == OOM)
            ReportOutOfMemory(cx);
        else if (result == Aborted && !ss->ensureOwnsSource(cx))
            result = OOM;
    }

    ss = nullptr;
    compressed = nullptr;
    MOZ_ASSERT(!active());

    return result != OOM;
}

SourceCompressionTask*
GlobalHelperThreadState::compressionTaskForSource(ScriptSource* ss)
{
    MOZ_ASSERT(isLocked());
    for (size_t i = 0; i < compressionWorklist().length(); i++) {
        SourceCompressionTask* task = compressionWorklist()[i];
        if (task->source() == ss)
            return task;
    }
    for (size_t i = 0; i < threadCount; i++) {
        SourceCompressionTask* task = threads[i].compressionTask();
        if (task && task->source() == ss)
            return task;
    }
    return nullptr;
}

void
HelperThread::handleGCHelperWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartGCHelperTask());
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().gcHelperWorklist().popCopy());
    GCHelperState* task = gcHelperTask();

    {
        AutoUnlockHelperThreadState unlock;
        task->work();
    }

    currentTask.reset();
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

void
HelperThread::threadLoop()
{
    MOZ_ASSERT(CanUseExtraThreads());

    JS::AutoSuppressGCAnalysis nogc;
    AutoLockHelperThreadState lock;

    js::TlsPerThreadData.set(threadData.ptr());

    // Compute the thread's stack limit, for over-recursed checks.
    uintptr_t stackLimit = GetNativeStackBase();
#if JS_STACK_GROWTH_DIRECTION > 0
    stackLimit += HELPER_STACK_QUOTA;
#else
    stackLimit -= HELPER_STACK_QUOTA;
#endif
    for (size_t i = 0; i < ArrayLength(threadData->nativeStackLimit); i++)
        threadData->nativeStackLimit[i] = stackLimit;

    while (true) {
        MOZ_ASSERT(idle());

        // Block until a task is available. Save the value of whether we are
        // going to do an Ion compile, in case the value returned by the method
        // changes.
        bool ionCompile = false;
        while (true) {
            if (terminate)
                return;
            if (HelperThreadState().canStartWasmCompile() ||
                (ionCompile = HelperThreadState().pendingIonCompileHasSufficientPriority()) ||
                HelperThreadState().canStartParseTask() ||
                HelperThreadState().canStartCompressionTask() ||
                HelperThreadState().canStartGCHelperTask() ||
                HelperThreadState().canStartGCParallelTask())
            {
                break;
            }
            HelperThreadState().wait(GlobalHelperThreadState::PRODUCER);
        }

        // Dispatch tasks, prioritizing wasm work.
        if (HelperThreadState().canStartWasmCompile()) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_ASMJS);
            handleWasmWorkload();
        } else if (ionCompile) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_ION);
            handleIonWorkload();
        } else if (HelperThreadState().canStartParseTask()) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_PARSE);
            handleParseWorkload();
        } else if (HelperThreadState().canStartCompressionTask()) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_COMPRESS);
            handleCompressionWorkload();
        } else if (HelperThreadState().canStartGCHelperTask()) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_GCHELPER);
            handleGCHelperWorkload();
        } else if (HelperThreadState().canStartGCParallelTask()) {
            js::oom::SetThreadType(js::oom::THREAD_TYPE_GCPARALLEL);
            handleGCParallelWorkload();
        } else {
            MOZ_CRASH("No task to perform");
        }
    }
}
