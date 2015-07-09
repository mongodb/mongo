/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/HelperThreads.h"

#include "mozilla/DebugOnly.h"

#include "jsnativestack.h"
#include "jsnum.h" // For FIX_FPU()
#include "prmjtime.h"

#include "frontend/BytecodeCompiler.h"
#include "gc/GCInternals.h"
#include "jit/IonBuilder.h"
#include "vm/Debugger.h"
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

void
js::EnsureHelperThreadsInitialized()
{
    MOZ_ASSERT(gHelperThreadState);
    gHelperThreadState->ensureInitialized();
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
js::StartOffThreadAsmJSCompile(ExclusiveContext* cx, AsmJSParallelTask* asmData)
{
    // Threads already initialized by the AsmJS compiler.
    MOZ_ASSERT(asmData->mir);
    MOZ_ASSERT(asmData->lir == nullptr);

    AutoLockHelperThreadState lock;

    // Don't append this task if another failed.
    if (HelperThreadState().asmJSFailed())
        return false;

    if (!HelperThreadState().asmJSWorklist().append(asmData))
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
    if (!HelperThreadState().ionFinishedList().append(builder))
        CrashAtUnhandlableOOM("FinishOffThreadIonCompile");
}

static inline bool
CompiledScriptMatches(JSCompartment* compartment, JSScript* script, JSScript* target)
{
    if (script)
        return target == script;
    return target->compartment() == compartment;
}

void
js::CancelOffThreadIonCompile(JSCompartment* compartment, JSScript* script)
{
    jit::JitCompartment* jitComp = compartment->jitCompartment();
    if (!jitComp)
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
        while (helper.ionBuilder &&
               CompiledScriptMatches(compartment, script, helper.ionBuilder->script()))
        {
            helper.ionBuilder->cancel();
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
            builder->script()->setPendingIonBuilder(nullptr, nullptr);
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
    exclusiveContextGlobal(initCx, exclusiveContextGlobal),
    callback(callback), callbackData(callbackData),
    script(nullptr), errors(cx), overRecursed(false)
{
}

bool
ParseTask::init(JSContext* cx, const ReadOnlyCompileOptions& options)
{
    if (!this->options.copy(cx, options))
        return false;

    // If the main-thread global is a debuggee that observes asm.js, disable
    // asm.js compilation. This is preferred to marking the task compartment
    // as a debuggee, as the task compartment is (1) invisible to Debugger and
    // (2) cannot have any Debuggers.
    if (cx->compartment()->debuggerObservesAsmJS())
        this->options.asmJSOption = false;

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
    if (script) {
        // Finish off the ScriptSourceObject initialization that we put off in
        // js::frontend::CreateScriptSourceObject.
        RootedScriptSource sso(cx, &script->sourceObject()->as<ScriptSourceObject>());
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
                ParseTask* task = HelperThreadState().threads[i].parseTask;
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

    JS_SetCompartmentPrincipals(global->compartment(), cx->compartment()->principals);

    RootedObject obj(cx);

    // Initialize all classes needed for parsing while we are still on the main
    // thread. Do this for both the target and the new global so that prototype
    // pointers can be changed infallibly after parsing finishes.
    if (!GetBuiltinConstructor(cx, JSProto_Function, &obj) ||
        !GetBuiltinConstructor(cx, JSProto_Array, &obj) ||
        !GetBuiltinConstructor(cx, JSProto_RegExp, &obj) ||
        !GetBuiltinConstructor(cx, JSProto_Iterator, &obj))
    {
        return false;
    }
    {
        AutoCompartment ac(cx, global);
        if (!GetBuiltinConstructor(cx, JSProto_Function, &obj) ||
            !GetBuiltinConstructor(cx, JSProto_Array, &obj) ||
            !GetBuiltinConstructor(cx, JSProto_RegExp, &obj) ||
            !GetBuiltinConstructor(cx, JSProto_Iterator, &obj))
        {
            return false;
        }
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
        if (!HelperThreadState().parseWaitingOnGC().append(task.get()))
            return false;
    } else {
        task->activate(cx->runtime());

        AutoLockHelperThreadState lock;

        if (!HelperThreadState().parseWorklist().append(task.get()))
            return false;

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
                newTasks.append(task);
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

    for (size_t i = 0; i < newTasks.length(); i++)
        HelperThreadState().parseWorklist().append(newTasks[i]);

    HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER);
}

static const uint32_t HELPER_STACK_SIZE = 512 * 1024;
static const uint32_t HELPER_STACK_QUOTA = 450 * 1024;

void
GlobalHelperThreadState::ensureInitialized()
{
    MOZ_ASSERT(CanUseExtraThreads());

    MOZ_ASSERT(this == &HelperThreadState());
    AutoLockHelperThreadState lock;

    if (threads)
        return;

    threads = js_pod_calloc<HelperThread>(threadCount);
    if (!threads)
        CrashAtUnhandlableOOM("GlobalHelperThreadState::ensureInitialized");

    for (size_t i = 0; i < threadCount; i++) {
        HelperThread& helper = threads[i];
        helper.threadData.emplace(static_cast<JSRuntime*>(nullptr));
        helper.thread = PR_CreateThread(PR_USER_THREAD,
                                        HelperThread::ThreadMain, &helper,
                                        PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, HELPER_STACK_SIZE);
        if (!helper.thread || !helper.threadData->init())
            CrashAtUnhandlableOOM("GlobalHelperThreadState::ensureInitialized");
    }

    resetAsmJSFailureState();
}

GlobalHelperThreadState::GlobalHelperThreadState()
 : cpuCount(0),
   threadCount(0),
   threads(nullptr),
   asmJSCompilationInProgress(false),
   helperLock(nullptr),
#ifdef DEBUG
   lockOwner(nullptr),
#endif
   consumerWakeup(nullptr),
   producerWakeup(nullptr),
   pauseWakeup(nullptr),
   numAsmJSFailedJobs(0),
   asmJSFailedFunction(nullptr)
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
    if (threads) {
        MOZ_ASSERT(CanUseExtraThreads());
        for (size_t i = 0; i < threadCount; i++)
            threads[i].destroy();
        js_free(threads);
    }

    PR_DestroyCondVar(consumerWakeup);
    PR_DestroyCondVar(producerWakeup);
    PR_DestroyCondVar(pauseWakeup);
    PR_DestroyLock(helperLock);

    ionLazyLinkList_.clear();
}

void
GlobalHelperThreadState::lock()
{
    MOZ_ASSERT(!isLocked());
    AssertCurrentThreadCanLock(HelperThreadStateLock);
    PR_Lock(helperLock);
#ifdef DEBUG
    lockOwner = PR_GetCurrentThread();
#endif
}

void
GlobalHelperThreadState::unlock()
{
    MOZ_ASSERT(isLocked());
#ifdef DEBUG
    lockOwner = nullptr;
#endif
    PR_Unlock(helperLock);
}

#ifdef DEBUG
bool
GlobalHelperThreadState::isLocked()
{
    return lockOwner == PR_GetCurrentThread();
}
#endif

void
GlobalHelperThreadState::wait(CondVar which, uint32_t millis)
{
    MOZ_ASSERT(isLocked());
#ifdef DEBUG
    lockOwner = nullptr;
#endif
    DebugOnly<PRStatus> status =
        PR_WaitCondVar(whichWakeup(which),
                       millis ? PR_MillisecondsToInterval(millis) : PR_INTERVAL_NO_TIMEOUT);
    MOZ_ASSERT(status == PR_SUCCESS);
#ifdef DEBUG
    lockOwner = PR_GetCurrentThread();
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
GlobalHelperThreadState::canStartAsmJSCompile()
{
    // Don't execute an AsmJS job if an earlier one failed.
    MOZ_ASSERT(isLocked());
    if (asmJSWorklist().empty() || numAsmJSFailedJobs)
        return false;

    // Honor the maximum allowed threads to compile AsmJS jobs at once,
    // to avoid oversaturating the machine.
    size_t numAsmJSThreads = 0;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].asmData)
            numAsmJSThreads++;
    }
    if (numAsmJSThreads >= maxAsmJSCompilationThreads())
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
    if (first->script()->hasIonScript() != second->script()->hasIonScript())
        return !first->script()->hasIonScript();

    // A higher warm-up counter indicates a higher priority.
    return first->script()->getWarmUpCount() / first->script()->length() >
           second->script()->getWarmUpCount() / second->script()->length();
}

bool
GlobalHelperThreadState::canStartIonCompile()
{
    return !ionWorklist().empty();
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
    // isn't paused, unless there are still fewer than the aximum number of
    // such builders permitted.
    size_t numBuilderThreads = 0;
    HelperThread* thread = nullptr;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].ionBuilder && !threads[i].pause) {
            numBuilderThreads++;
            if (!thread || IonBuilderHasHigherPriority(thread->ionBuilder, threads[i].ionBuilder))
                thread = &threads[i];
        }
    }
    if (numBuilderThreads < maxIonCompilationThreads())
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
            MOZ_ASSERT(threads[i].ionBuilder);
            if (!thread || IonBuilderHasHigherPriority(threads[i].ionBuilder, thread->ionBuilder))
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
    if (IonBuilderHasHigherPriority(highestPriorityPendingIonCompile(), lowestPriorityThread->ionBuilder))
        return true;

    // Compilation will have to wait until one of the active compilations finishes.
    return false;
}

bool
GlobalHelperThreadState::canStartParseTask()
{
    // Don't allow simultaneous off thread parses, to reduce contention on the
    // atoms table. Note that asm.js compilation depends on this to avoid
    // stalling the helper thread, as off thread parse tasks can trigger and
    // block on other off thread asm.js compilation tasks.
    MOZ_ASSERT(isLocked());
    if (parseWorklist().empty())
        return false;
    for (size_t i = 0; i < threadCount; i++) {
        if (threads[i].parseTask)
            return false;
    }
    return true;
}

bool
GlobalHelperThreadState::canStartCompressionTask()
{
    return !compressionWorklist().empty();
}

bool
GlobalHelperThreadState::canStartGCHelperTask()
{
    return !gcHelperWorklist().empty();
}

bool
GlobalHelperThreadState::canStartGCParallelTask()
{
    return !gcParallelWorklist().empty();
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

    MOZ_ASSERT(!gcParallelTask);
    gcParallelTask = HelperThreadState().gcParallelWorklist().popCopy();
    gcParallelTask->runFromHelperThread();
    gcParallelTask = nullptr;
}

static void
LeaveParseTaskZone(JSRuntime* rt, ParseTask* task)
{
    // Mark the zone as no longer in use by an ExclusiveContext, and available
    // to be collected by the GC.
    task->cx->leaveCompartment(task->cx->compartment());
    rt->clearUsedByExclusiveThread(task->cx->zone());
}

static bool
EnsureConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key)
{
    if (!GlobalObject::ensureConstructor(cx, global, key))
        return false;

    return global->getPrototype(key).toObject().setDelegate(cx);
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
    if (!EnsureConstructor(cx, global, JSProto_Object) ||
        !EnsureConstructor(cx, global, JSProto_Array) ||
        !EnsureConstructor(cx, global, JSProto_Function) ||
        !EnsureConstructor(cx, global, JSProto_RegExp) ||
        !EnsureConstructor(cx, global, JSProto_Iterator))
    {
        LeaveParseTaskZone(rt, parseTask);
        return nullptr;
    }

    LeaveParseTaskZone(rt, parseTask);

    // Point the prototypes of any objects in the script's compartment to refer
    // to the corresponding prototype in the new compartment. This will briefly
    // create cross compartment pointers, which will be fixed by the
    // MergeCompartments call below.  It's not safe for a GC to observe this
    // state, so finish any ongoing GC first and assert that we can't trigger
    // another one.
    gc::AutoFinishGC finishGC(rt);
    for (gc::ZoneCellIter iter(parseTask->cx->zone(), gc::FINALIZE_OBJECT_GROUP);
         !iter.done();
         iter.next())
    {
        JS::AutoAssertNoAlloc noAlloc(rt);
        ObjectGroup* group = iter.get<ObjectGroup>();
        TaggedProto proto(group->proto());
        if (!proto.isObject())
            continue;

        JSProtoKey key = JS::IdentifyStandardPrototype(proto.toObject());
        if (key == JSProto_Null)
            continue;
        MOZ_ASSERT(key == JSProto_Object || key == JSProto_Array ||
                   key == JSProto_Function || key == JSProto_RegExp ||
                   key == JSProto_Iterator);

        JSObject* newProto = GetBuiltinPrototypePure(global, key);
        MOZ_ASSERT(newProto);

        group->setProtoUnchecked(TaggedProto(newProto));
    }

    // Move the parsed script and all its contents into the desired compartment.
    gc::MergeCompartments(parseTask->cx->compartment(), cx->compartment());
    if (!parseTask->finish(cx))
        return nullptr;

    RootedScript script(rt, parseTask->script);
    assertSameCompartment(cx, script);

    // Report any error or warnings generated during the parse, and inform the
    // debugger about the compiled scripts.
    for (size_t i = 0; i < parseTask->errors.length(); i++)
        parseTask->errors[i]->throwError(cx);
    if (parseTask->overRecursed)
        js_ReportOverRecursed(cx);
    if (cx->isExceptionPending())
        return nullptr;

    if (script) {
        // The Debugger only needs to be told about the topmost script that was compiled.
        Debugger::onNewScript(cx, script);

        // Update the compressed source table with the result. This is normally
        // called by setCompressedSource when compilation occurs on the main thread.
        if (script->scriptSource()->hasCompressedSource())
            script->scriptSource()->updateCompressedSourceSet(rt);
    }

    return script;
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
    PR_SetCurrentThreadName("Analysis Helper");

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
HelperThread::handleAsmJSWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartAsmJSCompile());
    MOZ_ASSERT(idle());

    asmData = HelperThreadState().asmJSWorklist().popCopy();
    bool success = false;

    do {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(), asmData->runtime);

        jit::JitContext jcx(asmData->mir->compartment->runtime(),
                            asmData->mir->compartment,
                            &asmData->mir->alloc());

        int64_t before = PRMJ_Now();

        if (!OptimizeMIR(asmData->mir))
            break;

        asmData->lir = GenerateLIR(asmData->mir);
        if (!asmData->lir)
            break;

        int64_t after = PRMJ_Now();
        asmData->compileTime = (after - before) / PRMJ_USEC_PER_MSEC;

        success = true;
    } while(0);

    // On failure, signal parent for harvesting in CancelOutstandingJobs().
    if (!success) {
        HelperThreadState().noteAsmJSFailure(asmData->func);
        HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
        asmData = nullptr;
        return;
    }

    // On success, move work to the finished list.
    HelperThreadState().asmJSFinishedList().append(asmData);
    asmData = nullptr;

    // Notify the main thread in case it's blocked waiting for a LifoAlloc.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
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
        MOZ_ASSERT(other->ionBuilder && !other->pause);
        other->pause = true;
    }

    ionBuilder = builder;
    ionBuilder->setPauseFlag(&pause);

    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, ionBuilder->script());
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonCompilation);

    JSRuntime* rt = ionBuilder->script()->compartment()->runtimeFromAnyThread();

    {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(),
                                              ionBuilder->script()->runtimeFromAnyThread());
        jit::JitContext jctx(jit::CompileRuntime::get(rt),
                             jit::CompileCompartment::get(ionBuilder->script()->compartment()),
                             &ionBuilder->alloc());
        ionBuilder->setBackgroundCodegen(jit::CompileBackEnd(ionBuilder));
    }

    FinishOffThreadIonCompile(ionBuilder);
    ionBuilder = nullptr;
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
        MOZ_ASSERT(other->ionBuilder && other->pause);

        // Only unpause the other thread if there isn't a higher priority
        // builder which this thread or another can start on.
        jit::IonBuilder* builder = HelperThreadState().highestPriorityPendingIonCompile();
        if (!builder || IonBuilderHasHigherPriority(other->ionBuilder, builder)) {
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
    if (!helperThread()->parseTask->errors.append(error))
        MOZ_CRASH();
    return *error;
}

void
ExclusiveContext::addPendingOverRecursed()
{
    if (helperThread()->parseTask)
        helperThread()->parseTask->overRecursed = true;
}

void
HelperThread::handleParseWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartParseTask());
    MOZ_ASSERT(idle());

    parseTask = HelperThreadState().parseWorklist().popCopy();
    parseTask->cx->setHelperThread(this);

    {
        AutoUnlockHelperThreadState unlock;
        PerThreadData::AutoEnterRuntime enter(threadData.ptr(),
                                              parseTask->exclusiveContextGlobal->runtimeFromAnyThread());
        SourceBufferHolder srcBuf(parseTask->chars, parseTask->length,
                                  SourceBufferHolder::NoOwnership);
        parseTask->script = frontend::CompileScript(parseTask->cx, &parseTask->alloc,
                                                    NullPtr(), NullPtr(), NullPtr(),
                                                    parseTask->options,
                                                    srcBuf);
    }

    // The callback is invoked while we are still off the main thread.
    parseTask->callback(parseTask, parseTask->callbackData);

    // FinishOffThreadScript will need to be called on the script to
    // migrate it into the correct compartment.
    HelperThreadState().parseFinishedList().append(parseTask);

    parseTask = nullptr;

    // Notify the main thread in case it is waiting for the parse/emit to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

void
HelperThread::handleCompressionWorkload()
{
    MOZ_ASSERT(HelperThreadState().isLocked());
    MOZ_ASSERT(HelperThreadState().canStartCompressionTask());
    MOZ_ASSERT(idle());

    compressionTask = HelperThreadState().compressionWorklist().popCopy();
    compressionTask->helperThread = this;

    {
        AutoUnlockHelperThreadState unlock;
        compressionTask->result = compressionTask->work();
    }

    compressionTask->helperThread = nullptr;
    compressionTask = nullptr;

    // Notify the main thread in case it is waiting for the compression to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER);
}

bool
js::StartOffThreadCompression(ExclusiveContext* cx, SourceCompressionTask* task)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().compressionWorklist().append(task)) {
        if (JSContext* maybecx = cx->maybeJSContext())
            js_ReportOutOfMemory(maybecx);
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
        if (threads[i].compressionTask == task)
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
            js_ReportOutOfMemory(cx);
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
        SourceCompressionTask* task = threads[i].compressionTask;
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

    MOZ_ASSERT(!gcHelperState);
    gcHelperState = HelperThreadState().gcHelperWorklist().popCopy();

    {
        AutoUnlockHelperThreadState unlock;
        gcHelperState->work();
    }

    gcHelperState = nullptr;
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
            if (HelperThreadState().canStartAsmJSCompile() ||
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

        // Dispatch tasks, prioritizing AsmJS work.
        if (HelperThreadState().canStartAsmJSCompile())
            handleAsmJSWorkload();
        else if (ionCompile)
            handleIonWorkload();
        else if (HelperThreadState().canStartParseTask())
            handleParseWorkload();
        else if (HelperThreadState().canStartCompressionTask())
            handleCompressionWorkload();
        else if (HelperThreadState().canStartGCHelperTask())
            handleGCHelperWorkload();
        else if (HelperThreadState().canStartGCParallelTask())
            handleGCParallelWorkload();
        else
            MOZ_CRASH("No task to perform");
    }
}
