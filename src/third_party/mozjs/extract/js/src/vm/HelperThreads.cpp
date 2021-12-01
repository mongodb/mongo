/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/HelperThreads.h"

#include "mozilla/Maybe.h"
#include "mozilla/Unused.h"

#include "builtin/Promise.h"
#include "frontend/BytecodeCompiler.h"
#include "gc/GCInternals.h"
#include "jit/IonBuilder.h"
#include "js/Utility.h"
#include "threading/CpuCount.h"
#include "util/NativeStack.h"
#include "vm/Debugger.h"
#include "vm/ErrorReporting.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/Time.h"
#include "vm/TraceLogging.h"
#include "vm/Xdr.h"

#include "gc/PrivateIterators-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::Unused;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

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
ClampDefaultCPUCount(size_t cpuCount)
{
    // It's extremely rare for SpiderMonkey to have more than a few cores worth
    // of work. At higher core counts, performance can even decrease due to NUMA
    // (and SpiderMonkey's lack of NUMA-awareness), contention, and general lack
    // of optimization for high core counts. So to avoid wasting thread stack
    // resources (and cluttering gdb and core dumps), clamp to 8 cores for now.
    return Min<size_t>(cpuCount, 8);
}

static size_t
ThreadCountForCPUCount(size_t cpuCount)
{
    // We need at least two threads for tier-2 wasm compilations, because
    // there's a master task that holds a thread while other threads do the
    // compilation.
    return Max<size_t>(cpuCount, 2);
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
js::StartOffThreadWasmCompile(wasm::CompileTask* task, wasm::CompileMode mode)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().wasmWorklist(lock, mode).pushBack(task))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    return true;
}

void
js::StartOffThreadWasmTier2Generator(wasm::UniqueTier2GeneratorTask task)
{
    MOZ_ASSERT(CanUseExtraThreads());

    AutoLockHelperThreadState lock;

    if (!HelperThreadState().wasmTier2GeneratorWorklist(lock).append(task.get()))
        return;

    Unused << task.release();

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
}

static void
CancelOffThreadWasmTier2GeneratorLocked(AutoLockHelperThreadState& lock)
{
    if (!HelperThreadState().threads)
        return;

    // Remove pending tasks from the tier2 generator worklist and cancel and
    // delete them.
    {
        wasm::Tier2GeneratorTaskPtrVector& worklist =
            HelperThreadState().wasmTier2GeneratorWorklist(lock);
        for (size_t i = 0; i < worklist.length(); i++) {
            wasm::Tier2GeneratorTask* task = worklist[i];
            HelperThreadState().remove(worklist, &i);
            js_delete(task);
        }
    }

    // There is at most one running Tier2Generator task and we assume that
    // below.
    static_assert(GlobalHelperThreadState::MaxTier2GeneratorTasks == 1,
                  "code must be generalized");

    // If there is a running Tier2 generator task, shut it down in a predictable
    // way.  The task will be deleted by the normal deletion logic.
    for (auto& helper : *HelperThreadState().threads) {
        if (helper.wasmTier2GeneratorTask()) {
            // Set a flag that causes compilation to shortcut itself.
            helper.wasmTier2GeneratorTask()->cancel();

            // Wait for the generator task to finish.  This avoids a shutdown race where
            // the shutdown code is trying to shut down helper threads and the ongoing
            // tier2 compilation is trying to finish, which requires it to have access
            // to helper threads.
            uint32_t oldFinishedCount = HelperThreadState().wasmTier2GeneratorsFinished(lock);
            while (HelperThreadState().wasmTier2GeneratorsFinished(lock) == oldFinishedCount)
                HelperThreadState().wait(lock, GlobalHelperThreadState::CONSUMER);

            // At most one of these tasks.
            break;
        }
    }
}

void
js::CancelOffThreadWasmTier2Generator()
{
    AutoLockHelperThreadState lock;
    CancelOffThreadWasmTier2GeneratorLocked(lock);
}

bool
js::StartOffThreadIonCompile(jit::IonBuilder* builder, const AutoLockHelperThreadState& lock)
{
    if (!HelperThreadState().ionWorklist(lock).append(builder))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    return true;
}

bool
js::StartOffThreadIonFree(jit::IonBuilder* builder, const AutoLockHelperThreadState& lock)
{
    MOZ_ASSERT(CanUseExtraThreads());

    if (!HelperThreadState().ionFreeList(lock).append(builder))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    return true;
}

/*
 * Move an IonBuilder for which compilation has either finished, failed, or
 * been cancelled into the global finished compilation list. All off thread
 * compilations which are started must eventually be finished.
 */
static void
FinishOffThreadIonCompile(jit::IonBuilder* builder, const AutoLockHelperThreadState& lock)
{
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!HelperThreadState().ionFinishedList(lock).append(builder))
        oomUnsafe.crash("FinishOffThreadIonCompile");
    builder->script()->zoneFromAnyThread()->group()->numFinishedBuilders++;
}

static JSRuntime*
GetSelectorRuntime(const CompilationSelector& selector)
{
    struct Matcher
    {
        JSRuntime* match(JSScript* script)    { return script->runtimeFromActiveCooperatingThread(); }
        JSRuntime* match(JSCompartment* comp) { return comp->runtimeFromActiveCooperatingThread(); }
        JSRuntime* match(Zone* zone)          { return zone->runtimeFromActiveCooperatingThread(); }
        JSRuntime* match(ZonesInState zbs)    { return zbs.runtime; }
        JSRuntime* match(JSRuntime* runtime)  { return runtime; }
        JSRuntime* match(AllCompilations all) { return nullptr; }
        JSRuntime* match(CompilationsUsingNursery cun) { return cun.runtime; }
    };

    return selector.match(Matcher());
}

static bool
JitDataStructuresExist(const CompilationSelector& selector)
{
    struct Matcher
    {
        bool match(JSScript* script)    { return !!script->compartment()->jitCompartment(); }
        bool match(JSCompartment* comp) { return !!comp->jitCompartment(); }
        bool match(Zone* zone)          { return !!zone->jitZone(); }
        bool match(ZonesInState zbs)    { return zbs.runtime->hasJitRuntime(); }
        bool match(JSRuntime* runtime)  { return runtime->hasJitRuntime(); }
        bool match(AllCompilations all) { return true; }
        bool match(CompilationsUsingNursery cun) { return cun.runtime->hasJitRuntime(); }
    };

    return selector.match(Matcher());
}

static bool
IonBuilderMatches(const CompilationSelector& selector, jit::IonBuilder* builder)
{
    struct BuilderMatches
    {
        jit::IonBuilder* builder_;

        bool match(JSScript* script)    { return script == builder_->script(); }
        bool match(JSCompartment* comp) { return comp == builder_->script()->compartment(); }
        bool match(Zone* zone)          { return zone == builder_->script()->zone(); }
        bool match(JSRuntime* runtime)  { return runtime == builder_->script()->runtimeFromAnyThread(); }
        bool match(AllCompilations all) { return true; }
        bool match(ZonesInState zbs)    {
            return zbs.runtime == builder_->script()->runtimeFromAnyThread() &&
                   zbs.state == builder_->script()->zoneFromAnyThread()->gcState();
        }
        bool match(CompilationsUsingNursery cun) {
            return cun.runtime == builder_->script()->runtimeFromAnyThread() &&
                   !builder_->safeForMinorGC();
        }
    };

    return selector.match(BuilderMatches{builder});
}

static void
CancelOffThreadIonCompileLocked(const CompilationSelector& selector, bool discardLazyLinkList,
                                AutoLockHelperThreadState& lock)
{
    if (!HelperThreadState().threads)
        return;

    /* Cancel any pending entries for which processing hasn't started. */
    GlobalHelperThreadState::IonBuilderVector& worklist = HelperThreadState().ionWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
        jit::IonBuilder* builder = worklist[i];
        if (IonBuilderMatches(selector, builder)) {
            FinishOffThreadIonCompile(builder, lock);
            HelperThreadState().remove(worklist, &i);
        }
    }

    /* Wait for in progress entries to finish up. */
    bool cancelled;
    do {
        cancelled = false;
        for (auto& helper : *HelperThreadState().threads) {
            if (helper.ionBuilder() &&
                IonBuilderMatches(selector, helper.ionBuilder()))
            {
                helper.ionBuilder()->cancel();
                cancelled = true;
            }
        }
        if (cancelled)
            HelperThreadState().wait(lock, GlobalHelperThreadState::CONSUMER);
    } while (cancelled);

    /* Cancel code generation for any completed entries. */
    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList(lock);
    for (size_t i = 0; i < finished.length(); i++) {
        jit::IonBuilder* builder = finished[i];
        if (IonBuilderMatches(selector, builder)) {
            builder->script()->zoneFromAnyThread()->group()->numFinishedBuilders--;
            jit::FinishOffThreadBuilder(builder->script()->runtimeFromAnyThread(), builder, lock);
            HelperThreadState().remove(finished, &i);
        }
    }

    /* Cancel lazy linking for pending builders (attached to the ionScript). */
    if (discardLazyLinkList) {
        MOZ_ASSERT(!selector.is<AllCompilations>());
        JSRuntime* runtime = GetSelectorRuntime(selector);
        for (ZoneGroupsIter group(runtime); !group.done(); group.next()) {
            jit::IonBuilder* builder = group->ionLazyLinkList().getFirst();
            while (builder) {
                jit::IonBuilder* next = builder->getNext();
                if (IonBuilderMatches(selector, builder))
                    jit::FinishOffThreadBuilder(runtime, builder, lock);
                builder = next;
            }
        }
    }
}

void
js::CancelOffThreadIonCompile(const CompilationSelector& selector, bool discardLazyLinkList)
{
    if (!JitDataStructuresExist(selector))
        return;

    AutoLockHelperThreadState lock;
    CancelOffThreadIonCompileLocked(selector, discardLazyLinkList, lock);
}

#ifdef DEBUG
bool
js::HasOffThreadIonCompile(JSCompartment* comp)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().threads || comp->isAtomsCompartment())
        return false;

    GlobalHelperThreadState::IonBuilderVector& worklist = HelperThreadState().ionWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
        jit::IonBuilder* builder = worklist[i];
        if (builder->script()->compartment() == comp)
            return true;
    }

    for (auto& helper : *HelperThreadState().threads) {
        if (helper.ionBuilder() && helper.ionBuilder()->script()->compartment() == comp)
            return true;
    }

    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList(lock);
    for (size_t i = 0; i < finished.length(); i++) {
        jit::IonBuilder* builder = finished[i];
        if (builder->script()->compartment() == comp)
            return true;
    }

    jit::IonBuilder* builder = comp->zone()->group()->ionLazyLinkList().getFirst();
    while (builder) {
        if (builder->script()->compartment() == comp)
            return true;
        builder = builder->getNext();
    }

    return false;
}
#endif

static const JSClassOps parseTaskGlobalClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr,
    JS_GlobalObjectTraceHook
};

static const JSClass parseTaskGlobalClass = {
    "internal-parse-task-global", JSCLASS_GLOBAL_FLAGS,
    &parseTaskGlobalClassOps
};

ParseTask::ParseTask(ParseTaskKind kind, JSContext* cx,
                     JS::OffThreadCompileCallback callback, void* callbackData)
  : kind(kind),
    options(cx),
    alloc(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    parseGlobal(nullptr),
    callback(callback), callbackData(callbackData),
    scripts(cx), sourceObjects(cx),
    overRecursed(false), outOfMemory(false)
{
    MOZ_ALWAYS_TRUE(scripts.reserve(scripts.capacity()));
    MOZ_ALWAYS_TRUE(sourceObjects.reserve(sourceObjects.capacity()));
}

bool
ParseTask::init(JSContext* cx, const ReadOnlyCompileOptions& options, JSObject* global)
{
    if (!this->options.copy(cx, options))
        return false;

    parseGlobal = global;
    return true;
}

void
ParseTask::activate(JSRuntime* rt)
{
    rt->setUsedByHelperThread(parseGlobal->zone());
}

bool
ParseTask::finish(JSContext* cx)
{
    for (auto& sourceObject : sourceObjects) {
        RootedScriptSource sso(cx, sourceObject);
        if (!ScriptSourceObject::initFromOptions(cx, sso, options))
            return false;
        if (!sso->source()->tryCompressOffThread(cx))
            return false;
    }

    return true;
}

ParseTask::~ParseTask()
{
    for (size_t i = 0; i < errors.length(); i++)
        js_delete(errors[i]);
}

void
ParseTask::trace(JSTracer* trc)
{
    if (parseGlobal->runtimeFromAnyThread() != trc->runtime())
        return;

    Zone* zone = MaybeForwarded(parseGlobal)->zoneFromAnyThread();
    if (zone->usedByHelperThread()) {
        MOZ_ASSERT(!zone->isCollecting());
        return;
    }

    TraceManuallyBarrieredEdge(trc, &parseGlobal, "ParseTask::parseGlobal");
    scripts.trace(trc);
    sourceObjects.trace(trc);
}

ScriptParseTask::ScriptParseTask(JSContext* cx, const char16_t* chars, size_t length,
                                 JS::OffThreadCompileCallback callback, void* callbackData)
  : ParseTask(ParseTaskKind::Script, cx, callback, callbackData),
    data(TwoByteChars(chars, length))
{}

void
ScriptParseTask::parse(JSContext* cx)
{
    SourceBufferHolder srcBuf(data.begin().get(), data.length(), SourceBufferHolder::NoOwnership);
    Rooted<ScriptSourceObject*> sourceObject(cx);

    ScopeKind scopeKind = options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

    JSScript* script = frontend::CompileGlobalScript(cx, alloc, scopeKind,
                                                     options, srcBuf,
                                                     /* sourceObjectOut = */ &sourceObject.get());
    if (script)
        scripts.infallibleAppend(script);
    if (sourceObject)
        sourceObjects.infallibleAppend(sourceObject);
}

ModuleParseTask::ModuleParseTask(JSContext* cx, const char16_t* chars, size_t length,
                                 JS::OffThreadCompileCallback callback, void* callbackData)
  : ParseTask(ParseTaskKind::Module, cx, callback, callbackData),
    data(TwoByteChars(chars, length))
{}

void
ModuleParseTask::parse(JSContext* cx)
{
    SourceBufferHolder srcBuf(data.begin().get(), data.length(), SourceBufferHolder::NoOwnership);
    Rooted<ScriptSourceObject*> sourceObject(cx);

    ModuleObject* module = frontend::CompileModule(cx, options, srcBuf, alloc, &sourceObject.get());
    if (module) {
        scripts.infallibleAppend(module->script());
        if (sourceObject)
            sourceObjects.infallibleAppend(sourceObject);
    }
}

ScriptDecodeTask::ScriptDecodeTask(JSContext* cx, const JS::TranscodeRange& range,
                                   JS::OffThreadCompileCallback callback, void* callbackData)
  : ParseTask(ParseTaskKind::ScriptDecode, cx, callback, callbackData),
    range(range)
{}

void
ScriptDecodeTask::parse(JSContext* cx)
{
    RootedScript resultScript(cx);
    Rooted<ScriptSourceObject*> sourceObject(cx);

    XDROffThreadDecoder decoder(cx, alloc, &options, /* sourceObjectOut = */ &sourceObject.get(),
                                range);
    decoder.codeScript(&resultScript);
    MOZ_ASSERT(bool(resultScript) == (decoder.resultCode() == JS::TranscodeResult_Ok));
    if (decoder.resultCode() == JS::TranscodeResult_Ok) {
        scripts.infallibleAppend(resultScript);
        if (sourceObject)
            sourceObjects.infallibleAppend(sourceObject);
    }
}

MultiScriptsDecodeTask::MultiScriptsDecodeTask(JSContext* cx, JS::TranscodeSources& sources,
                                               JS::OffThreadCompileCallback callback,
                                               void* callbackData)
  : ParseTask(ParseTaskKind::MultiScriptsDecode, cx, callback, callbackData),
    sources(&sources)
{}

void
MultiScriptsDecodeTask::parse(JSContext* cx)
{
    if (!scripts.reserve(sources->length()) ||
        !sourceObjects.reserve(sources->length()))
    {
        return;
    }

    for (auto& source : *sources) {
        CompileOptions opts(cx, options);
        opts.setFileAndLine(source.filename, source.lineno);

        RootedScript resultScript(cx);
        Rooted<ScriptSourceObject*> sourceObject(cx);

        XDROffThreadDecoder decoder(cx, alloc, &opts, &sourceObject.get(), source.range);
        decoder.codeScript(&resultScript);
        MOZ_ASSERT(bool(resultScript) == (decoder.resultCode() == JS::TranscodeResult_Ok));

        if (decoder.resultCode() != JS::TranscodeResult_Ok)
            break;
        MOZ_ASSERT(resultScript);
        scripts.infallibleAppend(resultScript);
        sourceObjects.infallibleAppend(sourceObject);
    }
}

void
js::CancelOffThreadParses(JSRuntime* rt)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().threads)
        return;

#ifdef DEBUG
    GlobalHelperThreadState::ParseTaskVector& waitingOnGC =
        HelperThreadState().parseWaitingOnGC(lock);
    for (size_t i = 0; i < waitingOnGC.length(); i++)
        MOZ_ASSERT(!waitingOnGC[i]->runtimeMatches(rt));
#endif

    // Instead of forcibly canceling pending parse tasks, just wait for all scheduled
    // and in progress ones to complete. Otherwise the final GC may not collect
    // everything due to zones being used off thread.
    while (true) {
        bool pending = false;
        GlobalHelperThreadState::ParseTaskVector& worklist = HelperThreadState().parseWorklist(lock);
        for (size_t i = 0; i < worklist.length(); i++) {
            ParseTask* task = worklist[i];
            if (task->runtimeMatches(rt))
                pending = true;
        }
        if (!pending) {
            bool inProgress = false;
            for (auto& thread : *HelperThreadState().threads) {
                ParseTask* task = thread.parseTask();
                if (task && task->runtimeMatches(rt))
                    inProgress = true;
            }
            if (!inProgress)
                break;
        }
        HelperThreadState().wait(lock, GlobalHelperThreadState::CONSUMER);
    }

    // Clean up any parse tasks which haven't been finished by the active thread.
    GlobalHelperThreadState::ParseTaskVector& finished = HelperThreadState().parseFinishedList(lock);
    while (true) {
        bool found = false;
        for (size_t i = 0; i < finished.length(); i++) {
            ParseTask* task = finished[i];
            if (task->runtimeMatches(rt)) {
                found = true;
                AutoUnlockHelperThreadState unlock(lock);
                HelperThreadState().cancelParseTask(rt, task->kind, task);
            }
        }
        if (!found)
            break;
    }

#ifdef DEBUG
    GlobalHelperThreadState::ParseTaskVector& worklist = HelperThreadState().parseWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
        ParseTask* task = worklist[i];
        MOZ_ASSERT(!task->runtimeMatches(rt));
    }
#endif
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
EnsureParserCreatedClasses(JSContext* cx, ParseTaskKind kind)
{
    Handle<GlobalObject*> global = cx->global();

    if (!EnsureConstructor(cx, global, JSProto_Function))
        return false; // needed by functions, also adds object literals' proto

    if (!EnsureConstructor(cx, global, JSProto_Array))
        return false; // needed by array literals

    if (!EnsureConstructor(cx, global, JSProto_RegExp))
        return false; // needed by regular expression literals

    if (!GlobalObject::initGenerators(cx, global))
        return false; // needed by function*() {}

    if (kind == ParseTaskKind::Module && !GlobalObject::ensureModulePrototypesCreated(cx, global))
        return false;

    return true;
}

class MOZ_RAII AutoSetCreatedForHelperThread
{
    ZoneGroup* group;

  public:
    explicit AutoSetCreatedForHelperThread(JSObject* global)
      : group(global->zone()->group())
    {
        group->setCreatedForHelperThread();
    }

    void forget() {
        group = nullptr;
    }

    ~AutoSetCreatedForHelperThread() {
        if (group)
            group->clearUsedByHelperThread();
    }
};

static JSObject*
CreateGlobalForOffThreadParse(JSContext* cx, const gc::AutoSuppressGC& nogc)
{
    JSCompartment* currentCompartment = cx->compartment();

    JS::CompartmentOptions compartmentOptions(currentCompartment->creationOptions(),
                                              currentCompartment->behaviors());

    auto& creationOptions = compartmentOptions.creationOptions();

    creationOptions.setInvisibleToDebugger(true)
                   .setMergeable(true)
                   .setNewZoneInNewZoneGroup();

    // Don't falsely inherit the host's global trace hook.
    creationOptions.setTrace(nullptr);

    JSObject* obj = JS_NewGlobalObject(cx, &parseTaskGlobalClass, nullptr,
                                       JS::DontFireOnNewGlobalHook, compartmentOptions);
    if (!obj)
        return nullptr;

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    JS_SetCompartmentPrincipals(global->compartment(), currentCompartment->principals());

    return global;
}

static bool
QueueOffThreadParseTask(JSContext* cx, ParseTask* task)
{
    AutoLockHelperThreadState lock;

    bool mustWait = OffThreadParsingMustWaitForGC(cx->runtime());

    auto& queue = mustWait ? HelperThreadState().parseWaitingOnGC(lock)
                           : HelperThreadState().parseWorklist(lock);
    if (!queue.append(task)) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (!mustWait) {
        task->activate(cx->runtime());
        HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    }

    return true;
}

bool
StartOffThreadParseTask(JSContext* cx, ParseTask* task, const ReadOnlyCompileOptions& options)
{
    // Suppress GC so that calls below do not trigger a new incremental GC
    // which could require barriers on the atoms compartment.
    gc::AutoSuppressGC nogc(cx);
    gc::AutoSuppressNurseryCellAlloc noNurseryAlloc(cx);
    AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

    JSObject* global = CreateGlobalForOffThreadParse(cx, nogc);
    if (!global)
        return false;

    // Mark the global's zone group as created for a helper thread. This
    // prevents it from being collected until clearUsedByHelperThread() is
    // called after parsing is complete. If this function exits due to error
    // this state is cleared automatically.
    AutoSetCreatedForHelperThread createdForHelper(global);

    if (!task->init(cx, options, global))
        return false;

    if (!QueueOffThreadParseTask(cx, task))
        return false;

    createdForHelper.forget();
    return true;
}

bool
js::StartOffThreadParseScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                              const char16_t* chars, size_t length,
                              JS::OffThreadCompileCallback callback, void* callbackData)
{
    ScopedJSDeletePtr<ParseTask> task;
    task = cx->new_<ScriptParseTask>(cx, chars, length, callback, callbackData);
    if (!task || !StartOffThreadParseTask(cx, task, options))
        return false;

    task.forget();
    return true;
}

bool
js::StartOffThreadParseModule(JSContext* cx, const ReadOnlyCompileOptions& options,
                              const char16_t* chars, size_t length,
                              JS::OffThreadCompileCallback callback, void* callbackData)
{
    ScopedJSDeletePtr<ParseTask> task;
    task = cx->new_<ModuleParseTask>(cx, chars, length, callback, callbackData);
    if (!task || !StartOffThreadParseTask(cx, task, options))
        return false;

    task.forget();
    return true;
}

bool
js::StartOffThreadDecodeScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                               const JS::TranscodeRange& range,
                               JS::OffThreadCompileCallback callback, void* callbackData)
{
    ScopedJSDeletePtr<ParseTask> task;
    task = cx->new_<ScriptDecodeTask>(cx, range, callback, callbackData);
    if (!task || !StartOffThreadParseTask(cx, task, options))
        return false;

    task.forget();
    return true;
}

bool
js::StartOffThreadDecodeMultiScripts(JSContext* cx, const ReadOnlyCompileOptions& options,
                                     JS::TranscodeSources& sources,
                                     JS::OffThreadCompileCallback callback, void* callbackData)
{
    ScopedJSDeletePtr<ParseTask> task;
    task = cx->new_<MultiScriptsDecodeTask>(cx, sources, callback, callbackData);
    if (!task || !StartOffThreadParseTask(cx, task, options))
        return false;

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
        GlobalHelperThreadState::ParseTaskVector& waiting =
            HelperThreadState().parseWaitingOnGC(lock);

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

    // This logic should mirror the contents of the
    // !OffThreadParsingMustWaitForGC() branch in QueueOffThreadParseTask:

    for (size_t i = 0; i < newTasks.length(); i++)
        newTasks[i]->activate(rt);

    AutoLockHelperThreadState lock;

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!HelperThreadState().parseWorklist(lock).appendAll(newTasks))
            oomUnsafe.crash("EnqueuePendingParseTasksAfterGC");
    }

    HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER, lock);
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

    threads = js::MakeUnique<HelperThreadVector>();
    if (!threads || !threads->initCapacity(threadCount))
        return false;

    for (size_t i = 0; i < threadCount; i++) {
        threads->infallibleEmplaceBack();
        HelperThread& helper = (*threads)[i];

        helper.thread = mozilla::Some(Thread(Thread::Options().setStackSize(HELPER_STACK_SIZE)));
        if (!helper.thread->init(HelperThread::ThreadMain, &helper))
            goto error;

        continue;

    error:
        // Ensure that we do not leave uninitialized threads in the `threads`
        // vector.
        threads->popBack();
        finishThreads();
        return false;
    }

    return true;
}

GlobalHelperThreadState::GlobalHelperThreadState()
 : cpuCount(0),
   threadCount(0),
   threads(nullptr),
   wasmTier2GeneratorsFinished_(0),
   helperLock(mutexid::GlobalHelperThreadState)
{
    cpuCount = ClampDefaultCPUCount(GetCPUCount());
    threadCount = ThreadCountForCPUCount(cpuCount);

    MOZ_ASSERT(cpuCount > 0, "GetCPUCount() seems broken");
}

void
GlobalHelperThreadState::finish()
{
    CancelOffThreadWasmTier2Generator();
    finishThreads();

    // Make sure there are no Ion free tasks left. We check this here because,
    // unlike the other tasks, we don't explicitly block on this when
    // destroying a runtime.
    AutoLockHelperThreadState lock;
    auto& freeList = ionFreeList(lock);
    while (!freeList.empty())
        jit::FreeIonBuilder(freeList.popCopy());
}

void
GlobalHelperThreadState::finishThreads()
{
    if (!threads)
        return;

    MOZ_ASSERT(CanUseExtraThreads());
    for (auto& thread : *threads)
        thread.destroy();
    threads.reset(nullptr);
}

void
GlobalHelperThreadState::lock()
{
    helperLock.lock();
}

void
GlobalHelperThreadState::unlock()
{
    helperLock.unlock();
}

#ifdef DEBUG
bool
GlobalHelperThreadState::isLockedByCurrentThread()
{
    return helperLock.ownedByCurrentThread();
}
#endif // DEBUG

void
GlobalHelperThreadState::wait(AutoLockHelperThreadState& locked, CondVar which,
                              TimeDuration timeout /* = TimeDuration::Forever() */)
{
    whichWakeup(which).wait_for(locked, timeout);
}

void
GlobalHelperThreadState::notifyAll(CondVar which, const AutoLockHelperThreadState&)
{
    whichWakeup(which).notify_all();
}

void
GlobalHelperThreadState::notifyOne(CondVar which, const AutoLockHelperThreadState&)
{
    whichWakeup(which).notify_one();
}

bool
GlobalHelperThreadState::hasActiveThreads(const AutoLockHelperThreadState&)
{
    if (!threads)
        return false;

    for (auto& thread : *threads) {
        if (!thread.idle())
            return true;
    }

    return false;
}

void
GlobalHelperThreadState::waitForAllThreads()
{
    AutoLockHelperThreadState lock;
    waitForAllThreadsLocked(lock);
}

void
GlobalHelperThreadState::waitForAllThreadsLocked(AutoLockHelperThreadState& lock)
{
    CancelOffThreadIonCompileLocked(CompilationSelector(AllCompilations()), false, lock);
    CancelOffThreadWasmTier2GeneratorLocked(lock);

    while (hasActiveThreads(lock))
        wait(lock, CONSUMER);
}

// A task can be a "master" task, ie, it will block waiting for other worker
// threads that perform work on its behalf.  If so it must not take the last
// available thread; there must always be at least one worker thread able to do
// the actual work.  (Or the system may deadlock.)
//
// If a task is a master task it *must* pass isMaster=true here, or perform a
// similar calculation to avoid deadlock from starvation.
//
// isMaster should only be true if the thread calling checkTaskThreadLimit() is
// a helper thread.
//
// NOTE: Calling checkTaskThreadLimit() from a helper thread in the dynamic
// region after currentTask.emplace() and before currentTask.reset() may cause
// it to return a different result than if it is called outside that dynamic
// region, as the predicate inspects the values of the threads' currentTask
// members.

template <typename T>
bool
GlobalHelperThreadState::checkTaskThreadLimit(size_t maxThreads, bool isMaster) const
{
    MOZ_ASSERT(maxThreads > 0);

    if (!isMaster && maxThreads >= threadCount)
        return true;

    size_t count = 0;
    size_t idle = 0;
    for (auto& thread : *threads) {
        if (thread.currentTask.isSome()) {
            if (thread.currentTask->is<T>())
                count++;
        } else {
            idle++;
        }
        if (count >= maxThreads)
            return false;
    }

    // It is possible for the number of idle threads to be zero here, because
    // checkTaskThreadLimit() can be called from non-helper threads.  Notably,
    // the compression task scheduler invokes it, and runs off a helper thread.
    if (idle == 0)
        return false;

    // A master thread that's the last available thread must not be allowed to
    // run.
    if (isMaster && idle == 1)
        return false;

    return true;
}

struct MOZ_RAII AutoSetContextRuntime
{
    explicit AutoSetContextRuntime(JSRuntime* rt) {
        TlsContext.get()->setRuntime(rt);
    }
    ~AutoSetContextRuntime() {
        TlsContext.get()->setRuntime(nullptr);
    }
};

static inline bool
IsHelperThreadSimulatingOOM(js::ThreadType threadType)
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
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_ION))
        return 1;
    return threadCount;
}

size_t
GlobalHelperThreadState::maxWasmCompilationThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM))
        return 1;
    return cpuCount;
}

size_t
GlobalHelperThreadState::maxWasmTier2GeneratorThreads() const
{
    return MaxTier2GeneratorTasks;
}

size_t
GlobalHelperThreadState::maxPromiseHelperThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM))
        return 1;
    return cpuCount;
}

size_t
GlobalHelperThreadState::maxParseThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_PARSE))
        return 1;
    return cpuCount;
}

size_t
GlobalHelperThreadState::maxCompressionThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_COMPRESS))
        return 1;

    // Compression is triggered on major GCs to compress ScriptSources. It is
    // considered low priority work.
    return 1;
}

size_t
GlobalHelperThreadState::maxGCHelperThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_GCHELPER))
        return 1;
    return threadCount;
}

size_t
GlobalHelperThreadState::maxGCParallelThreads() const
{
    if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_GCPARALLEL))
        return 1;
    return threadCount;
}

bool
GlobalHelperThreadState::canStartWasmTier1Compile(const AutoLockHelperThreadState& lock)
{
    return canStartWasmCompile(lock, wasm::CompileMode::Tier1);
}

bool
GlobalHelperThreadState::canStartWasmTier2Compile(const AutoLockHelperThreadState& lock)
{
    return canStartWasmCompile(lock, wasm::CompileMode::Tier2);
}

bool
GlobalHelperThreadState::canStartWasmCompile(const AutoLockHelperThreadState& lock,
                                             wasm::CompileMode mode)
{
    if (wasmWorklist(lock, mode).empty())
        return false;

    // Parallel compilation and background compilation should be disabled on
    // unicore systems.

    MOZ_RELEASE_ASSERT(cpuCount > 1);

    // If Tier2 is very backlogged we must give priority to it, since the Tier2
    // queue holds onto Tier1 tasks.  Indeed if Tier2 is backlogged we will
    // devote more resources to Tier2 and not start any Tier1 work at all.

    bool tier2oversubscribed = wasmTier2GeneratorWorklist(lock).length() > 20;

    // For Tier1 and Once compilation, honor the maximum allowed threads to
    // compile wasm jobs at once, to avoid oversaturating the machine.
    //
    // For Tier2 compilation we need to allow other things to happen too, so we
    // do not allow all logical cores to be used for background work; instead we
    // wish to use a fraction of the physical cores.  We can't directly compute
    // the physical cores from the logical cores, but 1/3 of the logical cores
    // is a safe estimate for the number of physical cores available for
    // background work.

    size_t physCoresAvailable = size_t(ceil(cpuCount / 3.0));

    size_t threads;
    if (mode == wasm::CompileMode::Tier2) {
        if (tier2oversubscribed)
            threads = maxWasmCompilationThreads();
        else
            threads = physCoresAvailable;
    } else {
        if (tier2oversubscribed)
            threads = 0;
        else
            threads = maxWasmCompilationThreads();
    }

    if (!threads || !checkTaskThreadLimit<wasm::CompileTask*>(threads))
        return false;

    return true;
}

bool
GlobalHelperThreadState::canStartWasmTier2Generator(const AutoLockHelperThreadState& lock)
{
    return !wasmTier2GeneratorWorklist(lock).empty() &&
           checkTaskThreadLimit<wasm::Tier2GeneratorTask*>(maxWasmTier2GeneratorThreads(),
                                                           /*isMaster=*/true);
}

bool
GlobalHelperThreadState::canStartPromiseHelperTask(const AutoLockHelperThreadState& lock)
{
    // PromiseHelperTasks can be wasm compilation tasks that in turn block on
    // wasm compilation so set isMaster = true.
    return !promiseHelperTasks(lock).empty() &&
           checkTaskThreadLimit<PromiseHelperTask*>(maxPromiseHelperThreads(),
                                                    /*isMaster=*/true);
}

static bool
IonBuilderHasHigherPriority(jit::IonBuilder* first, jit::IonBuilder* second)
{
    // Return true if priority(first) > priority(second).
    //
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
GlobalHelperThreadState::canStartIonCompile(const AutoLockHelperThreadState& lock)
{
    return !ionWorklist(lock).empty() &&
           checkTaskThreadLimit<jit::IonBuilder*>(maxIonCompilationThreads());
}

bool
GlobalHelperThreadState::canStartIonFreeTask(const AutoLockHelperThreadState& lock)
{
    return !ionFreeList(lock).empty();
}

jit::IonBuilder*
GlobalHelperThreadState::highestPriorityPendingIonCompile(const AutoLockHelperThreadState& lock)
{
    auto& worklist = ionWorklist(lock);
    MOZ_ASSERT(!worklist.empty());

    // Get the highest priority IonBuilder which has not started compilation yet.
    size_t index = 0;
    for (size_t i = 1; i < worklist.length(); i++) {
        if (IonBuilderHasHigherPriority(worklist[i], worklist[index]))
            index = i;
    }

    jit::IonBuilder* builder = worklist[index];
    worklist.erase(&worklist[index]);
    return builder;
}

bool
GlobalHelperThreadState::canStartParseTask(const AutoLockHelperThreadState& lock)
{
    // Parse tasks that end up compiling asm.js in turn may use Wasm compilation
    // threads to generate machine code.  We have no way (at present) to know
    // ahead of time whether a parse task is going to parse asm.js content or
    // not, so we just assume that all parse tasks are master tasks.
    return !parseWorklist(lock).empty() &&
           checkTaskThreadLimit<ParseTask*>(maxParseThreads(), /*isMaster=*/true);
}

bool
GlobalHelperThreadState::canStartCompressionTask(const AutoLockHelperThreadState& lock)
{
    return !compressionWorklist(lock).empty() &&
           checkTaskThreadLimit<SourceCompressionTask*>(maxCompressionThreads());
}

void
GlobalHelperThreadState::startHandlingCompressionTasks(const AutoLockHelperThreadState& lock)
{
    scheduleCompressionTasks(lock);
    if (canStartCompressionTask(lock))
        notifyOne(PRODUCER, lock);
}

void
GlobalHelperThreadState::scheduleCompressionTasks(const AutoLockHelperThreadState& lock)
{
    auto& pending = compressionPendingList(lock);
    auto& worklist = compressionWorklist(lock);

    for (size_t i = 0; i < pending.length(); i++) {
        if (pending[i]->shouldStart()) {
            // OOMing during appending results in the task not being scheduled
            // and deleted.
            Unused << worklist.append(Move(pending[i]));
            remove(pending, &i);
        }
    }
}

bool
GlobalHelperThreadState::canStartGCHelperTask(const AutoLockHelperThreadState& lock)
{
    return !gcHelperWorklist(lock).empty() &&
           checkTaskThreadLimit<GCHelperState*>(maxGCHelperThreads());
}

bool
GlobalHelperThreadState::canStartGCParallelTask(const AutoLockHelperThreadState& lock)
{
    return !gcParallelWorklist(lock).empty() &&
           checkTaskThreadLimit<GCParallelTask*>(maxGCParallelThreads());
}

js::GCParallelTask::~GCParallelTask()
{
    // Only most-derived classes' destructors may do the join: base class
    // destructors run after those for derived classes' members, so a join in a
    // base class can't ensure that the task is done using the members. All we
    // can do now is check that someone has previously stopped the task.
#ifdef DEBUG
    Maybe<AutoLockHelperThreadState> helperLock;
    if (!HelperThreadState().isLockedByCurrentThread())
        helperLock.emplace();
    MOZ_ASSERT(state == NotStarted);
#endif
}

bool
js::GCParallelTask::startWithLockHeld(AutoLockHelperThreadState& lock)
{
    // Tasks cannot be started twice.
    MOZ_ASSERT(state == NotStarted);

    // If we do the shutdown GC before running anything, we may never
    // have initialized the helper threads. Just use the serial path
    // since we cannot safely intialize them at this point.
    if (!HelperThreadState().threads)
        return false;

    if (!HelperThreadState().gcParallelWorklist(lock).append(this))
        return false;
    state = Dispatched;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);

    return true;
}

bool
js::GCParallelTask::start()
{
    AutoLockHelperThreadState helperLock;
    return startWithLockHeld(helperLock);
}

void
js::GCParallelTask::joinWithLockHeld(AutoLockHelperThreadState& locked)
{
    if (state == NotStarted)
        return;

    while (state != Finished)
        HelperThreadState().wait(locked, GlobalHelperThreadState::CONSUMER);
    state = NotStarted;
    cancel_ = false;
}

void
js::GCParallelTask::join()
{
    AutoLockHelperThreadState helperLock;
    joinWithLockHeld(helperLock);
}

static inline
TimeDuration
TimeSince(TimeStamp prev)
{
    TimeStamp now = TimeStamp::Now();
    // Sadly this happens sometimes.
    MOZ_ASSERT(now >= prev);
    if (now < prev)
        now = prev;
    return now - prev;
}

void
js::GCParallelTask::runFromActiveCooperatingThread(JSRuntime* rt)
{
    MOZ_ASSERT(state == NotStarted);
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(rt));
    TimeStamp timeStart = TimeStamp::Now();
    runTask();
    duration_ = TimeSince(timeStart);
}

void
js::GCParallelTask::runFromHelperThread(AutoLockHelperThreadState& locked)
{
    AutoSetContextRuntime ascr(runtime());
    gc::AutoSetThreadIsPerformingGC performingGC;

    {
        AutoUnlockHelperThreadState parallelSection(locked);
        TimeStamp timeStart = TimeStamp::Now();
        TlsContext.get()->heapState = JS::HeapState::MajorCollecting;
        runTask();
        TlsContext.get()->heapState = JS::HeapState::Idle;
        duration_ = TimeSince(timeStart);
    }

    state = Finished;
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

bool
js::GCParallelTask::isRunningWithLockHeld(const AutoLockHelperThreadState& locked) const
{
    return state == Dispatched;
}

bool
js::GCParallelTask::isRunning() const
{
    AutoLockHelperThreadState helperLock;
    return isRunningWithLockHeld(helperLock);
}

void
HelperThread::handleGCParallelWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartGCParallelTask(locked));
    MOZ_ASSERT(idle());

    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog logCompile(logger, TraceLogger_GC);

    currentTask.emplace(HelperThreadState().gcParallelWorklist(locked).popCopy());
    gcParallelTask()->runFromHelperThread(locked);
    currentTask.reset();
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

static void
LeaveParseTaskZone(JSRuntime* rt, ParseTask* task)
{
    // Mark the zone as no longer in use by a helper thread, and available
    // to be collected by the GC.
    rt->clearUsedByHelperThread(task->parseGlobal->zone());
}

ParseTask*
GlobalHelperThreadState::removeFinishedParseTask(ParseTaskKind kind, void* token)
{
    // The token is a ParseTask* which should be in the finished list.
    // Find and remove its entry.

    AutoLockHelperThreadState lock;
    ParseTaskVector& finished = parseFinishedList(lock);

    for (size_t i = 0; i < finished.length(); i++) {
        if (finished[i] == token) {
            ParseTask* parseTask = finished[i];
            remove(finished, &i);
            MOZ_ASSERT(parseTask);
            MOZ_ASSERT(parseTask->kind == kind);
            return parseTask;
        }
    }

    MOZ_CRASH("Invalid ParseTask token");
}

template <typename F, typename>
bool
GlobalHelperThreadState::finishParseTask(JSContext* cx, ParseTaskKind kind, void* token, F&& finishCallback)
{
    MOZ_ASSERT(cx->compartment());

    ScopedJSDeletePtr<ParseTask> parseTask(removeFinishedParseTask(kind, token));

    // Make sure we have all the constructors we need for the prototype
    // remapping below, since we can't GC while that's happening.
    if (!EnsureParserCreatedClasses(cx, kind)) {
        LeaveParseTaskZone(cx->runtime(), parseTask);
        return false;
    }

    mergeParseTaskCompartment(cx, parseTask, cx->compartment());

    bool ok = finishCallback(parseTask);

    for (auto& script : parseTask->scripts)
        releaseAssertSameCompartment(cx, script);

    if (!parseTask->finish(cx) || !ok)
        return false;

    // Report out of memory errors eagerly, or errors could be malformed.
    if (parseTask->outOfMemory) {
        ReportOutOfMemory(cx);
        return false;
    }

    // Report any error or warnings generated during the parse, and inform the
    // debugger about the compiled scripts.
    for (size_t i = 0; i < parseTask->errors.length(); i++)
        parseTask->errors[i]->throwError(cx);
    if (parseTask->overRecursed)
        ReportOverRecursed(cx);
    if (cx->isExceptionPending())
        return false;

    return true;
}

JSScript*
GlobalHelperThreadState::finishParseTask(JSContext* cx, ParseTaskKind kind, void* token)
{
    JS::RootedScript script(cx);

    bool ok = finishParseTask(cx, kind, token, [&script] (ParseTask* parseTask) {
        MOZ_RELEASE_ASSERT(parseTask->scripts.length() <= 1);

        if (parseTask->scripts.length() > 0)
            script = parseTask->scripts[0];

        return true;
    });

    if (!ok)
        return nullptr;

    if (!script) {
        // No error was reported, but no script produced. Assume we hit out of
        // memory.
        ReportOutOfMemory(cx);
        return nullptr;
    }

    // The Debugger only needs to be told about the topmost script that was compiled.
    Debugger::onNewScript(cx, script);

    return script;
}

bool
GlobalHelperThreadState::finishParseTask(JSContext* cx, ParseTaskKind kind, void* token,
                                         MutableHandle<ScriptVector> scripts)
{
    size_t expectedLength = 0;

    bool ok = finishParseTask(cx, kind, token, [&scripts, &expectedLength] (ParseTask* parseTask) {
        MOZ_ASSERT(parseTask->kind == ParseTaskKind::MultiScriptsDecode);
        auto task = static_cast<MultiScriptsDecodeTask*>(parseTask);

        expectedLength = task->sources->length();

        if (!scripts.reserve(task->scripts.length()))
            return false;

        for (auto& script : task->scripts)
            scripts.infallibleAppend(script);
        return true;
    });

    if (!ok)
        return false;

    if (scripts.length() != expectedLength) {
        // No error was reported, but fewer scripts produced than expected.
        // Assume we hit out of memory.
        ReportOutOfMemory(cx);
        return false;
    }

    // The Debugger only needs to be told about the topmost script that was compiled.
    JS::RootedScript rooted(cx);
    for (auto& script : scripts) {
        MOZ_ASSERT(script->isGlobalCode());

        rooted = script;
        Debugger::onNewScript(cx, rooted);
    }

    return true;
}

JSScript*
GlobalHelperThreadState::finishScriptParseTask(JSContext* cx, void* token)
{
    JSScript* script = finishParseTask(cx, ParseTaskKind::Script, token);
    MOZ_ASSERT_IF(script, script->isGlobalCode());
    return script;
}

JSScript*
GlobalHelperThreadState::finishScriptDecodeTask(JSContext* cx, void* token)
{
    JSScript* script = finishParseTask(cx, ParseTaskKind::ScriptDecode, token);
    MOZ_ASSERT_IF(script, script->isGlobalCode());
    return script;
}

bool
GlobalHelperThreadState::finishMultiScriptsDecodeTask(JSContext* cx, void* token, MutableHandle<ScriptVector> scripts)
{
    return finishParseTask(cx, ParseTaskKind::MultiScriptsDecode, token, scripts);
}

JSObject*
GlobalHelperThreadState::finishModuleParseTask(JSContext* cx, void* token)
{
    JSScript* script = finishParseTask(cx, ParseTaskKind::Module, token);
    if (!script)
        return nullptr;

    MOZ_ASSERT(script->module());

    RootedModuleObject module(cx, script->module());
    module->fixEnvironmentsAfterCompartmentMerge();
    if (!ModuleObject::Freeze(cx, module))
        return nullptr;

    return module;
}

void
GlobalHelperThreadState::cancelParseTask(JSRuntime* rt, ParseTaskKind kind, void* token)
{
    ScopedJSDeletePtr<ParseTask> parseTask(removeFinishedParseTask(kind, token));
    LeaveParseTaskZone(rt, parseTask);
}

void
GlobalHelperThreadState::mergeParseTaskCompartment(JSContext* cx, ParseTask* parseTask,
                                                   JSCompartment* dest)
{
    // After we call LeaveParseTaskZone() it's not safe to GC until we have
    // finished merging the contents of the parse task's compartment into the
    // destination compartment.
    JS::AutoAssertNoGC nogc(cx);

    LeaveParseTaskZone(cx->runtime(), parseTask);

    // Move the parsed script and all its contents into the desired compartment.
    gc::MergeCompartments(parseTask->parseGlobal->compartment(), dest);
}

void
HelperThread::destroy()
{
    if (thread.isSome()) {
        {
            AutoLockHelperThreadState lock;
            terminate = true;

            /* Notify all helpers, to ensure that this thread wakes up. */
            HelperThreadState().notifyAll(GlobalHelperThreadState::PRODUCER, lock);
        }

        thread->join();
        thread.reset();
    }
}

/* static */
void
HelperThread::ThreadMain(void* arg)
{
    ThisThread::SetName("JS Helper");

    static_cast<HelperThread*>(arg)->threadLoop();
    Mutex::ShutDown();
}

void
HelperThread::handleWasmTier1Workload(AutoLockHelperThreadState& locked)
{
    handleWasmWorkload(locked, wasm::CompileMode::Tier1);
}

void
HelperThread::handleWasmTier2Workload(AutoLockHelperThreadState& locked)
{
    handleWasmWorkload(locked, wasm::CompileMode::Tier2);
}

void
HelperThread::handleWasmWorkload(AutoLockHelperThreadState& locked, wasm::CompileMode mode)
{
    MOZ_ASSERT(HelperThreadState().canStartWasmCompile(locked, mode));
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().wasmWorklist(locked, mode).popCopyFront());

    wasm::CompileTask* task = wasmTask();
    {
        AutoUnlockHelperThreadState unlock(locked);
        wasm::ExecuteCompileTaskFromHelperThread(task);
    }

    // No active thread should be waiting on the CONSUMER mutex.
    currentTask.reset();
}

void
HelperThread::handleWasmTier2GeneratorWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartWasmTier2Generator(locked));
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().wasmTier2GeneratorWorklist(locked).popCopy());

    wasm::Tier2GeneratorTask* task = wasmTier2GeneratorTask();
    {
        AutoUnlockHelperThreadState unlock(locked);
        task->execute();
    }

    // During shutdown the main thread will wait for any ongoing (cancelled)
    // tier-2 generation to shut down normally.  To do so, it waits on the
    // CONSUMER condition for the count of finished generators to rise.
    HelperThreadState().incWasmTier2GeneratorsFinished(locked);
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);

    js_delete(task);
    currentTask.reset();
}

void
HelperThread::handlePromiseHelperTaskWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartPromiseHelperTask(locked));
    MOZ_ASSERT(idle());

    PromiseHelperTask* task = HelperThreadState().promiseHelperTasks(locked).popCopy();
    currentTask.emplace(task);

    {
        AutoUnlockHelperThreadState unlock(locked);
        task->execute();
        task->dispatchResolveAndDestroy();
    }

    // No active thread should be waiting on the CONSUMER mutex.
    currentTask.reset();
}

void
HelperThread::handleIonWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartIonCompile(locked));
    MOZ_ASSERT(idle());

    // Find the IonBuilder in the worklist with the highest priority, and
    // remove it from the worklist.
    jit::IonBuilder* builder = HelperThreadState().highestPriorityPendingIonCompile(locked);

    currentTask.emplace(builder);

    JSRuntime* rt = builder->script()->compartment()->runtimeFromAnyThread();

    {
        AutoUnlockHelperThreadState unlock(locked);

        TraceLoggerThread* logger = TraceLoggerForCurrentThread();
        TraceLoggerEvent event(TraceLogger_AnnotateScripts, builder->script());
        AutoTraceLog logScript(logger, event);
        AutoTraceLog logCompile(logger, TraceLogger_IonCompilation);

        AutoSetContextRuntime ascr(rt);
        jit::JitContext jctx(jit::CompileRuntime::get(rt),
                             jit::CompileCompartment::get(builder->script()->compartment()),
                             &builder->alloc());
        builder->setBackgroundCodegen(jit::CompileBackEnd(builder));
    }

    FinishOffThreadIonCompile(builder, locked);

    // Ping any thread currently operating on the compiled script's zone group
    // so that the compiled code can be incorporated at the next interrupt
    // callback. Don't interrupt Ion code for this, as this incorporation can
    // be delayed indefinitely without affecting performance as long as the
    // active thread is actually executing Ion code.
    //
    // This must happen before the current task is reset. DestroyContext
    // cancels in progress Ion compilations before destroying its target
    // context, and after we reset the current task we are no longer considered
    // to be Ion compiling.
    JSContext* target = builder->script()->zoneFromAnyThread()->group()->ownerContext().context();
    if (target)
        target->requestInterrupt(JSContext::RequestInterruptCanWait);

    currentTask.reset();

    // Notify the active thread in case it is waiting for the compilation to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

void
HelperThread::handleIonFreeWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(idle());
    MOZ_ASSERT(HelperThreadState().canStartIonFreeTask(locked));

    auto& freeList = HelperThreadState().ionFreeList(locked);

    jit::IonBuilder* builder = freeList.popCopy();
    {
        AutoUnlockHelperThreadState unlock(locked);
        FreeIonBuilder(builder);
    }
}

HelperThread*
js::CurrentHelperThread()
{
    if (!HelperThreadState().threads)
        return nullptr;
    auto threadId = ThisThread::GetId();
    for (auto& thisThread : *HelperThreadState().threads) {
        if (thisThread.thread.isSome() && threadId == thisThread.thread->get_id())
            return &thisThread;
    }
    return nullptr;
}

bool
JSContext::addPendingCompileError(js::CompileError** error)
{
    auto errorPtr = make_unique<js::CompileError>();
    if (!errorPtr)
        return false;
    if (!helperThread()->parseTask()->errors.append(errorPtr.get())) {
        ReportOutOfMemory(this);
        return false;
    }
    *error = errorPtr.release();
    return true;
}

void
JSContext::addPendingOverRecursed()
{
    if (helperThread()->parseTask())
        helperThread()->parseTask()->overRecursed = true;
}

void
JSContext::addPendingOutOfMemory()
{
    // Keep in sync with recoverFromOutOfMemory.
    if (helperThread()->parseTask())
        helperThread()->parseTask()->outOfMemory = true;
}

void
HelperThread::handleParseWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartParseTask(locked));
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().parseWorklist(locked).popCopy());
    ParseTask* task = parseTask();

    {
        AutoUnlockHelperThreadState unlock(locked);
        AutoSetContextRuntime ascr(task->parseGlobal->runtimeFromAnyThread());

        JSContext* cx = TlsContext.get();
        AutoCompartment ac(cx, task->parseGlobal);

        task->parse(cx);

        cx->frontendCollectionPool().purge();
    }

    // The callback is invoked while we are still off thread.
    task->callback(task, task->callbackData);

    // FinishOffThreadScript will need to be called on the script to
    // migrate it into the correct compartment.
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!HelperThreadState().parseFinishedList(locked).append(task))
            oomUnsafe.crash("handleParseWorkload");
    }

    currentTask.reset();

    // Notify the active thread in case it is waiting for the parse/emit to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

void
HelperThread::handleCompressionWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartCompressionTask(locked));
    MOZ_ASSERT(idle());

    UniquePtr<SourceCompressionTask> task;
    {
        auto& worklist = HelperThreadState().compressionWorklist(locked);
        task = Move(worklist.back());
        worklist.popBack();
        currentTask.emplace(task.get());
    }

    {
        AutoUnlockHelperThreadState unlock(locked);

        TraceLoggerThread* logger = TraceLoggerForCurrentThread();
        AutoTraceLog logCompile(logger, TraceLogger_CompressSource);

        task->work();
    }

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!HelperThreadState().compressionFinishedList(locked).append(Move(task)))
            oomUnsafe.crash("handleCompressionWorkload");
    }

    currentTask.reset();

    // Notify the active thread in case it is waiting for the compression to finish.
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

bool
js::EnqueueOffThreadCompression(JSContext* cx, UniquePtr<SourceCompressionTask> task)
{
    AutoLockHelperThreadState lock;

    auto& pending = HelperThreadState().compressionPendingList(lock);
    if (!pending.append(Move(task))) {
        if (!cx->helperThread())
            ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

template <typename T>
static void
ClearCompressionTaskList(T& list, JSRuntime* runtime)
{
    for (size_t i = 0; i < list.length(); i++) {
        if (list[i]->runtimeMatches(runtime))
            HelperThreadState().remove(list, &i);
    }
}

void
js::CancelOffThreadCompressions(JSRuntime* runtime)
{
    AutoLockHelperThreadState lock;

    if (!HelperThreadState().threads)
        return;

    // Cancel all pending compression tasks.
    ClearCompressionTaskList(HelperThreadState().compressionPendingList(lock), runtime);
    ClearCompressionTaskList(HelperThreadState().compressionWorklist(lock), runtime);

    // Cancel all in-process compression tasks and wait for them to join so we
    // clean up the finished tasks.
    while (true) {
        bool inProgress = false;
        for (auto& thread : *HelperThreadState().threads) {
            SourceCompressionTask* task = thread.compressionTask();
            if (task && task->runtimeMatches(runtime))
                inProgress = true;
        }

        if (!inProgress)
            break;

        HelperThreadState().wait(lock, GlobalHelperThreadState::CONSUMER);
    }

    // Clean up finished tasks.
    ClearCompressionTaskList(HelperThreadState().compressionFinishedList(lock), runtime);
}

void
PromiseHelperTask::executeAndResolveAndDestroy(JSContext* cx)
{
    execute();
    run(cx, JS::Dispatchable::NotShuttingDown);
}

bool
js::StartOffThreadPromiseHelperTask(JSContext* cx, UniquePtr<PromiseHelperTask> task)
{
    // Execute synchronously if there are no helper threads.
    if (!CanUseExtraThreads()) {
        task.release()->executeAndResolveAndDestroy(cx);
        return true;
    }

    AutoLockHelperThreadState lock;

    if (!HelperThreadState().promiseHelperTasks(lock).append(task.get())) {
        ReportOutOfMemory(cx);
        return false;
    }

    Unused << task.release();

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    return true;
}

bool
js::StartOffThreadPromiseHelperTask(PromiseHelperTask* task)
{
    MOZ_ASSERT(CanUseExtraThreads());

    AutoLockHelperThreadState lock;

    if (!HelperThreadState().promiseHelperTasks(lock).append(task))
        return false;

    HelperThreadState().notifyOne(GlobalHelperThreadState::PRODUCER, lock);
    return true;
}

void
GlobalHelperThreadState::trace(JSTracer* trc, gc::AutoTraceSession& session)
{
    // There's an assertion that requires the exclusive access lock when tracing
    // atoms (see AtomIsPinnedInRuntime). Due to mutex ordering requirements we
    // need to take that lock before the helper thread lock, if we don't have it
    // already.
    Maybe<AutoLockForExclusiveAccess> exclusiveLock;
    if (!session.maybeLock.isSome())
        exclusiveLock.emplace(trc->runtime());

    AutoLockHelperThreadState lock;
    for (auto builder : ionWorklist(lock))
        builder->trace(trc);
    for (auto builder : ionFinishedList(lock))
        builder->trace(trc);

    if (HelperThreadState().threads) {
        for (auto& helper : *HelperThreadState().threads) {
            if (auto builder = helper.ionBuilder())
                builder->trace(trc);
        }
    }

    for (ZoneGroupsIter group(trc->runtime()); !group.done(); group.next()) {
        jit::IonBuilder* builder = group->ionLazyLinkList().getFirst();
        while (builder) {
            builder->trace(trc);
            builder = builder->getNext();
        }
    }

    for (auto parseTask : parseWorklist_)
        parseTask->trace(trc);
    for (auto parseTask : parseFinishedList_)
        parseTask->trace(trc);
    for (auto parseTask : parseWaitingOnGC_)
        parseTask->trace(trc);
}

void
HelperThread::handleGCHelperWorkload(AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(HelperThreadState().canStartGCHelperTask(locked));
    MOZ_ASSERT(idle());

    currentTask.emplace(HelperThreadState().gcHelperWorklist(locked).popCopy());
    GCHelperState* task = gcHelperTask();

    AutoSetContextRuntime ascr(task->runtime());

    {
        AutoUnlockHelperThreadState unlock(locked);
        task->work();
    }

    currentTask.reset();
    HelperThreadState().notifyAll(GlobalHelperThreadState::CONSUMER, locked);
}

void
JSContext::setHelperThread(HelperThread* thread)
{
    if (helperThread_)
        nurserySuppressions_--;

    helperThread_ = thread;

    if (helperThread_)
        nurserySuppressions_++;
}

// Definition of helper thread tasks.
//
// Priority is determined by the order they're listed here.
const HelperThread::TaskSpec HelperThread::taskSpecs[] = {
    {
        THREAD_TYPE_GCPARALLEL,
        &GlobalHelperThreadState::canStartGCParallelTask,
        &HelperThread::handleGCParallelWorkload
    },
    {
        THREAD_TYPE_GCHELPER,
        &GlobalHelperThreadState::canStartGCHelperTask,
        &HelperThread::handleGCHelperWorkload
    },
    {
        THREAD_TYPE_ION,
        &GlobalHelperThreadState::canStartIonCompile,
        &HelperThread::handleIonWorkload
    },
    {
        THREAD_TYPE_WASM,
        &GlobalHelperThreadState::canStartWasmTier1Compile,
        &HelperThread::handleWasmTier1Workload
    },
    {
        THREAD_TYPE_PROMISE_TASK,
        &GlobalHelperThreadState::canStartPromiseHelperTask,
        &HelperThread::handlePromiseHelperTaskWorkload
    },
    {
        THREAD_TYPE_PARSE,
        &GlobalHelperThreadState::canStartParseTask,
        &HelperThread::handleParseWorkload
    },
    {
        THREAD_TYPE_COMPRESS,
        &GlobalHelperThreadState::canStartCompressionTask,
        &HelperThread::handleCompressionWorkload
    },
    {
        THREAD_TYPE_ION_FREE,
        &GlobalHelperThreadState::canStartIonFreeTask,
        &HelperThread::handleIonFreeWorkload
    },
    {
        THREAD_TYPE_WASM,
        &GlobalHelperThreadState::canStartWasmTier2Compile,
        &HelperThread::handleWasmTier2Workload
    },
    {
        THREAD_TYPE_WASM_TIER2,
        &GlobalHelperThreadState::canStartWasmTier2Generator,
        &HelperThread::handleWasmTier2GeneratorWorkload
    }
};

void
HelperThread::threadLoop()
{
    MOZ_ASSERT(CanUseExtraThreads());

    JS::AutoSuppressGCAnalysis nogc;
    AutoLockHelperThreadState lock;

    JSContext cx(nullptr, JS::ContextOptions());
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!cx.init(ContextKind::Background))
            oomUnsafe.crash("HelperThread cx.init()");
    }
    cx.setHelperThread(this);
    JS_SetNativeStackQuota(&cx, HELPER_STACK_QUOTA);

    while (!terminate) {
        MOZ_ASSERT(idle());

        // The selectors may depend on the HelperThreadState not changing
        // between task selection and task execution, in particular, on new
        // tasks not being added (because of the lifo structure of the work
        // lists). Unlocking the HelperThreadState between task selection and
        // execution is not well-defined.

        const TaskSpec* task = findHighestPriorityTask(lock);
        if (!task) {
            HelperThreadState().wait(lock, GlobalHelperThreadState::PRODUCER);
            continue;
        }

        js::oom::SetThreadType(task->type);
        (this->*(task->handleWorkload))(lock);
        js::oom::SetThreadType(js::THREAD_TYPE_NONE);
    }
}

const HelperThread::TaskSpec*
HelperThread::findHighestPriorityTask(const AutoLockHelperThreadState& locked)
{
    // Return the highest priority task that is ready to start, or nullptr.

    for (const auto& task : taskSpecs) {
        if ((HelperThreadState().*(task.canStart))(locked))
            return &task;
    }

    return nullptr;
}
