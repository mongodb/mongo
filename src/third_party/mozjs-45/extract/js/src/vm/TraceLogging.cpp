/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TraceLogging.h"

#include "mozilla/DebugOnly.h"

#include <string.h>

#include "jsapi.h"
#include "jsprf.h"
#include "jsscript.h"

#include "jit/BaselineJIT.h"
#include "jit/CompileWrappers.h"
#include "vm/Runtime.h"
#include "vm/Time.h"
#include "vm/TraceLoggingGraph.h"

#include "jit/JitFrames-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::NativeEndian;

TraceLoggerThreadState* traceLoggerState = nullptr;

#if defined(MOZ_HAVE_RDTSC)

uint64_t inline rdtsc() {
    return ReadTimestampCounter();
}

#elif defined(__powerpc__)
static __inline__ uint64_t
rdtsc(void)
{
    uint64_t result=0;
    uint32_t upper, lower,tmp;
    __asm__ volatile(
            "0:                  \n"
            "\tmftbu   %0           \n"
            "\tmftb    %1           \n"
            "\tmftbu   %2           \n"
            "\tcmpw    %2,%0        \n"
            "\tbne     0b         \n"
            : "=r"(upper),"=r"(lower),"=r"(tmp)
            );
    result = upper;
    result = result<<32;
    result = result|lower;

    return result;

}
#elif defined(__arm__)

#include <sys/time.h>

static __inline__ uint64_t
rdtsc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ret = tv.tv_sec;
    ret *= 1000000;
    ret += tv.tv_usec;
    return ret;
}

#else

static __inline__ uint64_t
rdtsc(void)
{
    return 0;
}

#endif // defined(MOZ_HAVE_RDTSC)

class AutoTraceLoggerThreadStateLock
{
  TraceLoggerThreadState* logging;

  public:
    explicit AutoTraceLoggerThreadStateLock(TraceLoggerThreadState* logging MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : logging(logging)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        PR_Lock(logging->lock);
    }
    ~AutoTraceLoggerThreadStateLock() {
        PR_Unlock(logging->lock);
    }
  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

static bool
EnsureTraceLoggerState()
{
    if (MOZ_LIKELY(traceLoggerState))
        return true;

    traceLoggerState = js_new<TraceLoggerThreadState>();
    if (!traceLoggerState)
        return false;

    if (!traceLoggerState->init()) {
        DestroyTraceLoggerThreadState();
        return false;
    }

    return true;
}

void
js::DestroyTraceLoggerThreadState()
{
    if (traceLoggerState) {
        js_delete(traceLoggerState);
        traceLoggerState = nullptr;
    }
}

bool
TraceLoggerThread::init()
{
    if (!pointerMap.init())
        return false;
    if (!textIdPayloads.init())
        return false;
    if (!events.init())
        return false;

    // Minimum amount of capacity needed for operation to allow flushing.
    // Flushing requires space for the actual event and two spaces to log the
    // start and stop of flushing.
    if (!events.ensureSpaceBeforeAdd(3))
        return false;

    return true;
}

void
TraceLoggerThread::initGraph()
{
    // Create a graph. I don't like this is called reset, but it locks the
    // graph into the UniquePtr. So it gets deleted when TraceLoggerThread
    // is destructed.
    graph.reset(js_new<TraceLoggerGraph>());
    if (!graph.get())
        return;

    MOZ_ASSERT(traceLoggerState);
    uint64_t start = rdtsc() - traceLoggerState->startupTime;
    if (!graph->init(start)) {
        graph = nullptr;
        return;
    }

    // Report the textIds to the graph.
    for (uint32_t i = 0; i < TraceLogger_LastTreeItem; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        graph->addTextId(i, TLTextIdString(id));
    }
    graph->addTextId(TraceLogger_LastTreeItem, "TraceLogger internal");
    for (uint32_t i = TraceLogger_LastTreeItem + 1; i < TraceLogger_Last; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        graph->addTextId(i, TLTextIdString(id));
    }
}

TraceLoggerThread::~TraceLoggerThread()
{
    if (graph.get()) {
        if (!failed)
            graph->log(events);
        graph = nullptr;
    }

    if (textIdPayloads.initialized()) {
        for (TextIdHashMap::Range r = textIdPayloads.all(); !r.empty(); r.popFront())
            js_delete(r.front().value());
    }
}

bool
TraceLoggerThread::enable()
{
    if (enabled > 0) {
        enabled++;
        return true;
    }

    if (failed)
        return false;

    enabled = 1;
    logTimestamp(TraceLogger_Enable);

    return true;
}

bool
TraceLoggerThread::fail(JSContext* cx, const char* error)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TRACELOGGER_ENABLE_FAIL, error);
    failed = true;
    enabled = 0;

    return false;
}

bool
TraceLoggerThread::enable(JSContext* cx)
{
    if (!enable())
        return fail(cx, "internal error");

    if (enabled == 1) {
        // Get the top Activation to log the top script/pc (No inlined frames).
        ActivationIterator iter(cx->runtime());
        Activation* act = iter.activation();

        if (!act)
            return fail(cx, "internal error");

        JSScript* script = nullptr;
        int32_t engine = 0;

        if (act->isJit()) {
            JitFrameIterator it(iter);

            while (!it.isScripted() && !it.done())
                ++it;

            MOZ_ASSERT(!it.done());
            MOZ_ASSERT(it.isIonJS() || it.isBaselineJS());

            script = it.script();
            engine = it.isIonJS() ? TraceLogger_IonMonkey : TraceLogger_Baseline;
        } else if (act->isAsmJS()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TRACELOGGER_ENABLE_FAIL,
                                 "not yet supported in asmjs code");
            return false;
        } else {
            MOZ_ASSERT(act->isInterpreter());
            InterpreterFrame* fp = act->asInterpreter()->current();
            MOZ_ASSERT(!fp->runningInJit());

            script = fp->script();
            engine = TraceLogger_Interpreter;
            if (script->compartment() != cx->compartment())
                return fail(cx, "compartment mismatch");
        }

        TraceLoggerEvent event(this, TraceLogger_Scripts, script);
        startEvent(event);
        startEvent(engine);
    }

    return true;
}

bool
TraceLoggerThread::disable()
{
    if (failed)
        return false;

    if (enabled == 0)
        return true;

    if (enabled > 1) {
        enabled--;
        return true;
    }

    logTimestamp(TraceLogger_Disable);
    enabled = 0;

    return true;
}

const char*
TraceLoggerThread::eventText(uint32_t id)
{
    if (id < TraceLogger_Last)
        return TLTextIdString(static_cast<TraceLoggerTextId>(id));

    TextIdHashMap::Ptr p = textIdPayloads.lookup(id);
    MOZ_ASSERT(p);

    return p->value()->string();
}

bool
TraceLoggerThread::textIdIsScriptEvent(uint32_t id)
{
    if (id < TraceLogger_Last)
        return false;

    // Currently this works by checking if text begins with "script".
    const char* str = eventText(id);
    return EqualChars(str, "script", 6);
}

void
TraceLoggerThread::extractScriptDetails(uint32_t textId, const char** filename, size_t* filename_len,
                                        const char** lineno, size_t* lineno_len, const char** colno,
                                        size_t* colno_len)
{
    MOZ_ASSERT(textIdIsScriptEvent(textId));

    const char* script = eventText(textId);

    // Get the start of filename (remove 'script ' at the start).
    MOZ_ASSERT(EqualChars(script, "script ", 7));
    *filename = script + 7;

    // Get the start of lineno and colno.
    *lineno = script;
    *colno = script;
    const char* next = script - 1;
    while ((next = strchr(next + 1, ':'))) {
        *lineno = *colno;
        *colno = next;
    }

    MOZ_ASSERT(*lineno && *lineno != script);
    MOZ_ASSERT(*colno && *colno != script);

    // Remove the ':' at the front.
    *lineno = *lineno + 1;
    *colno = *colno + 1;

    *filename_len = *lineno - *filename - 1;
    *lineno_len = *colno - *lineno - 1;
    *colno_len = strlen(*colno);
}

TraceLoggerEventPayload*
TraceLoggerThread::getOrCreateEventPayload(TraceLoggerTextId textId)
{
    TextIdHashMap::AddPtr p = textIdPayloads.lookupForAdd(textId);
    if (p) {
        MOZ_ASSERT(p->value()->textId() == textId); // Sanity check.
        return p->value();
    }

    TraceLoggerEventPayload* payload = js_new<TraceLoggerEventPayload>(textId, (char*)nullptr);

    if (!textIdPayloads.add(p, textId, payload))
        return nullptr;

    return payload;
}

TraceLoggerEventPayload*
TraceLoggerThread::getOrCreateEventPayload(const char* text)
{
    PointerHashMap::AddPtr p = pointerMap.lookupForAdd((const void*)text);
    if (p) {
        MOZ_ASSERT(p->value()->textId() < nextTextId); // Sanity check.
        return p->value();
    }

    size_t len = strlen(text);
    char* str = js_pod_malloc<char>(len + 1);
    if (!str)
        return nullptr;

    DebugOnly<size_t> ret = JS_snprintf(str, len + 1, "%s", text);
    MOZ_ASSERT(ret == len);
    MOZ_ASSERT(strlen(str) == len);

    uint32_t textId = nextTextId;

    TraceLoggerEventPayload* payload = js_new<TraceLoggerEventPayload>(textId, str);
    if (!payload) {
        js_free(str);
        return nullptr;
    }

    if (!textIdPayloads.putNew(textId, payload)) {
        js_delete(payload);
        return nullptr;
    }

    if (!pointerMap.add(p, text, payload))
        return nullptr;

    if (graph.get())
        graph->addTextId(textId, str);

    nextTextId++;

    return payload;
}

TraceLoggerEventPayload*
TraceLoggerThread::getOrCreateEventPayload(TraceLoggerTextId type, const char* filename,
                                           size_t lineno, size_t colno, const void* ptr)
{
    MOZ_ASSERT(type == TraceLogger_Scripts || type == TraceLogger_AnnotateScripts ||
               type == TraceLogger_InlinedScripts);

    if (!filename)
        filename = "<unknown>";

    // Only log scripts when enabled otherwise return the global Scripts textId,
    // which will get filtered out.
    MOZ_ASSERT(traceLoggerState);
    if (!traceLoggerState->isTextIdEnabled(type))
        return getOrCreateEventPayload(type);

    PointerHashMap::AddPtr p = pointerMap.lookupForAdd(ptr);
    if (p) {
        MOZ_ASSERT(p->value()->textId() < nextTextId); // Sanity check.
        return p->value();
    }

    // Compute the length of the string to create.
    size_t lenFilename = strlen(filename);
    size_t lenLineno = 1;
    for (size_t i = lineno; i /= 10; lenLineno++);
    size_t lenColno = 1;
    for (size_t i = colno; i /= 10; lenColno++);

    size_t len = 7 + lenFilename + 1 + lenLineno + 1 + lenColno;
    char* str = js_pod_malloc<char>(len + 1);
    if (!str)
        return nullptr;

    DebugOnly<size_t> ret =
        JS_snprintf(str, len + 1, "script %s:%u:%u", filename, lineno, colno);
    MOZ_ASSERT(ret == len);
    MOZ_ASSERT(strlen(str) == len);

    uint32_t textId = nextTextId;
    TraceLoggerEventPayload* payload = js_new<TraceLoggerEventPayload>(textId, str);
    if (!payload) {
        js_free(str);
        return nullptr;
    }

    if (!textIdPayloads.putNew(textId, payload)) {
        js_delete(payload);
        return nullptr;
    }

    if (!pointerMap.add(p, ptr, payload))
        return nullptr;

    if (graph.get())
        graph->addTextId(textId, str);

    nextTextId++;

    return payload;
}

TraceLoggerEventPayload*
TraceLoggerThread::getOrCreateEventPayload(TraceLoggerTextId type, JSScript* script)
{
    return getOrCreateEventPayload(type, script->filename(), script->lineno(), script->column(),
                                   script);
}

TraceLoggerEventPayload*
TraceLoggerThread::getOrCreateEventPayload(TraceLoggerTextId type,
                                           const JS::ReadOnlyCompileOptions& script)
{
    return getOrCreateEventPayload(type, script.filename(), script.lineno, script.column, &script);
}

void
TraceLoggerThread::startEvent(TraceLoggerTextId id) {
    startEvent(uint32_t(id));
}

void
TraceLoggerThread::startEvent(const TraceLoggerEvent& event) {
    if (!event.hasPayload()) {
        startEvent(TraceLogger_Error);
        return;
    }
    startEvent(event.payload()->textId());
}

void
TraceLoggerThread::startEvent(uint32_t id)
{
    MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
    MOZ_ASSERT(traceLoggerState);
    if (!traceLoggerState->isTextIdEnabled(id))
       return;

    log(id);
}

void
TraceLoggerThread::stopEvent(TraceLoggerTextId id) {
    stopEvent(uint32_t(id));
}

void
TraceLoggerThread::stopEvent(const TraceLoggerEvent& event) {
    if (!event.hasPayload()) {
        stopEvent(TraceLogger_Error);
        return;
    }
    stopEvent(event.payload()->textId());
}

void
TraceLoggerThread::stopEvent(uint32_t id)
{
    MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
    MOZ_ASSERT(traceLoggerState);
    if (!traceLoggerState->isTextIdEnabled(id))
        return;

    log(TraceLogger_Stop);
}

void
TraceLoggerThread::logTimestamp(TraceLoggerTextId id)
{
    logTimestamp(uint32_t(id));
}

void
TraceLoggerThread::logTimestamp(uint32_t id)
{
    MOZ_ASSERT(id > TraceLogger_LastTreeItem && id < TraceLogger_Last);
    log(id);
}

void
TraceLoggerThread::log(uint32_t id)
{
    if (enabled == 0)
        return;

    MOZ_ASSERT(traceLoggerState);
    if (!events.ensureSpaceBeforeAdd()) {
        uint64_t start = rdtsc() - traceLoggerState->startupTime;

        if (graph.get())
            graph->log(events);

        iteration_++;
        events.clear();

        // Log the time it took to flush the events as being from the
        // Tracelogger.
        if (graph.get()) {
            MOZ_ASSERT(events.capacity() > 2);
            EventEntry& entryStart = events.pushUninitialized();
            entryStart.time = start;
            entryStart.textId = TraceLogger_Internal;

            EventEntry& entryStop = events.pushUninitialized();
            entryStop.time = rdtsc() - traceLoggerState->startupTime;
            entryStop.textId = TraceLogger_Stop;
        }

        // Remove the item in the pointerMap for which the payloads
        // have no uses anymore
        for (PointerHashMap::Enum e(pointerMap); !e.empty(); e.popFront()) {
            if (e.front().value()->uses() != 0)
                continue;

            TextIdHashMap::Ptr p = textIdPayloads.lookup(e.front().value()->textId());
            MOZ_ASSERT(p);
            textIdPayloads.remove(p);

            e.removeFront();
        }

        // Free all payloads that have no uses anymore.
        for (TextIdHashMap::Enum e(textIdPayloads); !e.empty(); e.popFront()) {
            if (e.front().value()->uses() == 0) {
                js_delete(e.front().value());
                e.removeFront();
            }
        }
    }

    uint64_t time = rdtsc() - traceLoggerState->startupTime;

    EventEntry& entry = events.pushUninitialized();
    entry.time = time;
    entry.textId = id;
}

TraceLoggerThreadState::~TraceLoggerThreadState()
{
    for (size_t i = 0; i < mainThreadLoggers.length(); i++)
        js_delete(mainThreadLoggers[i]);

    mainThreadLoggers.clear();

    if (threadLoggers.initialized()) {
        for (ThreadLoggerHashMap::Range r = threadLoggers.all(); !r.empty(); r.popFront())
            js_delete(r.front().value());

        threadLoggers.finish();
    }

    if (lock) {
        PR_DestroyLock(lock);
        lock = nullptr;
    }

#ifdef DEBUG
    initialized = false;
#endif
}

static bool
ContainsFlag(const char* str, const char* flag)
{
    size_t flaglen = strlen(flag);
    const char* index = strstr(str, flag);
    while (index) {
        if ((index == str || index[-1] == ',') && (index[flaglen] == 0 || index[flaglen] == ','))
            return true;
        index = strstr(index + flaglen, flag);
    }
    return false;
}

bool
TraceLoggerThreadState::init()
{
    lock = PR_NewLock();
    if (!lock)
        return false;

    if (!threadLoggers.init())
        return false;

    const char* env = getenv("TLLOG");
    if (!env)
        env = "";

    if (strstr(env, "help")) {
        fflush(nullptr);
        printf(
            "\n"
            "usage: TLLOG=option,option,option,... where options can be:\n"
            "\n"
            "Collections:\n"
            "  Default        Output all default. It includes:\n"
            "                 AnnotateScripts, Bailout, Baseline, BaselineCompilation, GC,\n"
            "                 GCAllocation, GCSweeping, Interpreter, IonCompilation, IonLinking,\n"
            "                 IonMonkey, MinorGC, ParserCompileFunction, ParserCompileScript,\n"
            "                 ParserCompileLazy, ParserCompileModule, IrregexpCompile,\n"
            "                 IrregexpExecute, Scripts, Engine\n"
            "\n"
            "  IonCompiler    Output all information about compilation. It includes:\n"
            "                 IonCompilation, IonLinking, PruneUnusedBranches, FoldTests,\n"
            "                 SplitCriticalEdges, RenumberBlocks, ScalarReplacement, \n"
            "                 DominatorTree, PhiAnalysis, MakeLoopsContiguous, ApplyTypes, \n"
            "                 EagerSimdUnbox, AliasAnalysis, GVN, LICM, Sincos, RangeAnalysis, \n"
            "                 LoopUnrolling, EffectiveAddressAnalysis, AlignmentMaskAnalysis, \n"
            "                 EliminateDeadCode, ReorderInstructions, EdgeCaseAnalysis, \n"
            "                 EliminateRedundantChecks, AddKeepAliveInstructions, GenerateLIR, \n"
            "                 RegisterAllocation, GenerateCode, Scripts\n"
            "\n"
            "Specific log items:\n"
        );
        for (uint32_t i = 1; i < TraceLogger_Last; i++) {
            TraceLoggerTextId id = TraceLoggerTextId(i);
            if (!TLTextIdIsToggable(id))
                continue;
            printf("  %s\n", TLTextIdString(id));
        }
        printf("\n");
        exit(0);
        /*NOTREACHED*/
    }

    for (uint32_t i = 1; i < TraceLogger_Last; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        if (TLTextIdIsToggable(id))
            enabledTextIds[i] = ContainsFlag(env, TLTextIdString(id));
        else
            enabledTextIds[i] = true;
    }

    if (ContainsFlag(env, "Default")) {
        enabledTextIds[TraceLogger_AnnotateScripts] = true;
        enabledTextIds[TraceLogger_Bailout] = true;
        enabledTextIds[TraceLogger_Baseline] = true;
        enabledTextIds[TraceLogger_BaselineCompilation] = true;
        enabledTextIds[TraceLogger_GC] = true;
        enabledTextIds[TraceLogger_GCAllocation] = true;
        enabledTextIds[TraceLogger_GCSweeping] = true;
        enabledTextIds[TraceLogger_Interpreter] = true;
        enabledTextIds[TraceLogger_IonCompilation] = true;
        enabledTextIds[TraceLogger_IonLinking] = true;
        enabledTextIds[TraceLogger_IonMonkey] = true;
        enabledTextIds[TraceLogger_MinorGC] = true;
        enabledTextIds[TraceLogger_ParserCompileFunction] = true;
        enabledTextIds[TraceLogger_ParserCompileLazy] = true;
        enabledTextIds[TraceLogger_ParserCompileScript] = true;
        enabledTextIds[TraceLogger_ParserCompileModule] = true;
        enabledTextIds[TraceLogger_IrregexpCompile] = true;
        enabledTextIds[TraceLogger_IrregexpExecute] = true;
        enabledTextIds[TraceLogger_Scripts] = true;
        enabledTextIds[TraceLogger_Engine] = true;
    }

    if (ContainsFlag(env, "IonCompiler")) {
        enabledTextIds[TraceLogger_IonCompilation] = true;
        enabledTextIds[TraceLogger_IonLinking] = true;
        enabledTextIds[TraceLogger_PruneUnusedBranches] = true;
        enabledTextIds[TraceLogger_FoldTests] = true;
        enabledTextIds[TraceLogger_SplitCriticalEdges] = true;
        enabledTextIds[TraceLogger_RenumberBlocks] = true;
        enabledTextIds[TraceLogger_ScalarReplacement] = true;
        enabledTextIds[TraceLogger_DominatorTree] = true;
        enabledTextIds[TraceLogger_PhiAnalysis] = true;
        enabledTextIds[TraceLogger_MakeLoopsContiguous] = true;
        enabledTextIds[TraceLogger_ApplyTypes] = true;
        enabledTextIds[TraceLogger_EagerSimdUnbox] = true;
        enabledTextIds[TraceLogger_AliasAnalysis] = true;
        enabledTextIds[TraceLogger_GVN] = true;
        enabledTextIds[TraceLogger_LICM] = true;
        enabledTextIds[TraceLogger_Sincos] = true;
        enabledTextIds[TraceLogger_RangeAnalysis] = true;
        enabledTextIds[TraceLogger_LoopUnrolling] = true;
        enabledTextIds[TraceLogger_EffectiveAddressAnalysis] = true;
        enabledTextIds[TraceLogger_AlignmentMaskAnalysis] = true;
        enabledTextIds[TraceLogger_EliminateDeadCode] = true;
        enabledTextIds[TraceLogger_ReorderInstructions] = true;
        enabledTextIds[TraceLogger_EdgeCaseAnalysis] = true;
        enabledTextIds[TraceLogger_EliminateRedundantChecks] = true;
        enabledTextIds[TraceLogger_AddKeepAliveInstructions] = true;
        enabledTextIds[TraceLogger_GenerateLIR] = true;
        enabledTextIds[TraceLogger_RegisterAllocation] = true;
        enabledTextIds[TraceLogger_GenerateCode] = true;
        enabledTextIds[TraceLogger_Scripts] = true;
    }

    enabledTextIds[TraceLogger_Interpreter] = enabledTextIds[TraceLogger_Engine];
    enabledTextIds[TraceLogger_Baseline] = enabledTextIds[TraceLogger_Engine];
    enabledTextIds[TraceLogger_IonMonkey] = enabledTextIds[TraceLogger_Engine];

    const char* options = getenv("TLOPTIONS");
    if (options) {
        if (strstr(options, "help")) {
            fflush(nullptr);
            printf(
                "\n"
                "usage: TLOPTIONS=option,option,option,... where options can be:\n"
                "\n"
                "  EnableMainThread        Start logging the main thread immediately.\n"
                "  EnableOffThread         Start logging helper threads immediately.\n"
                "  EnableGraph             Enable spewing the tracelogging graph to a file.\n"
            );
            printf("\n");
            exit(0);
            /*NOTREACHED*/
        }

        if (strstr(options, "EnableMainThread"))
           mainThreadEnabled = true;
        if (strstr(options, "EnableOffThread"))
           offThreadEnabled = true;
        if (strstr(options, "EnableGraph"))
           graphSpewingEnabled = true;
    }

    startupTime = rdtsc();

#ifdef DEBUG
    initialized = true;
#endif

    return true;
}

void
TraceLoggerThreadState::enableTextId(JSContext* cx, uint32_t textId)
{
    MOZ_ASSERT(TLTextIdIsToggable(textId));

    if (enabledTextIds[textId])
        return;

    enabledTextIds[textId] = true;
    if (textId == TraceLogger_Engine) {
        enabledTextIds[TraceLogger_IonMonkey] = true;
        enabledTextIds[TraceLogger_Baseline] = true;
        enabledTextIds[TraceLogger_Interpreter] = true;
    }

    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

    if (textId == TraceLogger_Scripts)
        jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), true);
    if (textId == TraceLogger_Engine)
        jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), true);

}
void
TraceLoggerThreadState::disableTextId(JSContext* cx, uint32_t textId)
{
    MOZ_ASSERT(TLTextIdIsToggable(textId));

    if (!enabledTextIds[textId])
        return;

    enabledTextIds[textId] = false;
    if (textId == TraceLogger_Engine) {
        enabledTextIds[TraceLogger_IonMonkey] = false;
        enabledTextIds[TraceLogger_Baseline] = false;
        enabledTextIds[TraceLogger_Interpreter] = false;
    }

    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

    if (textId == TraceLogger_Scripts)
        jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), false);
    if (textId == TraceLogger_Engine)
        jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), false);
}


TraceLoggerThread*
js::TraceLoggerForMainThread(CompileRuntime* runtime)
{
    if (!EnsureTraceLoggerState())
        return nullptr;
    return traceLoggerState->forMainThread(runtime);
}

TraceLoggerThread*
TraceLoggerThreadState::forMainThread(CompileRuntime* runtime)
{
    return forMainThread(runtime->mainThread());
}

TraceLoggerThread*
js::TraceLoggerForMainThread(JSRuntime* runtime)
{
    if (!EnsureTraceLoggerState())
        return nullptr;
    return traceLoggerState->forMainThread(runtime);
}

TraceLoggerThread*
TraceLoggerThreadState::forMainThread(JSRuntime* runtime)
{
    return forMainThread(&runtime->mainThread);
}

TraceLoggerThread*
TraceLoggerThreadState::forMainThread(PerThreadData* mainThread)
{
    MOZ_ASSERT(initialized);
    if (!mainThread->traceLogger) {
        AutoTraceLoggerThreadStateLock lock(this);

        TraceLoggerThread* logger = create();
        if (!logger)
            return nullptr;

        if (!mainThreadLoggers.append(logger)) {
            js_delete(logger);
            return nullptr;
        }

        mainThread->traceLogger = logger;

        if (graphSpewingEnabled)
            logger->initGraph();

        if (mainThreadEnabled)
            logger->enable();
    }

    return mainThread->traceLogger;
}

TraceLoggerThread*
js::TraceLoggerForCurrentThread()
{
    PRThread* thread = PR_GetCurrentThread();
    if (!EnsureTraceLoggerState())
        return nullptr;
    return traceLoggerState->forThread(thread);
}

TraceLoggerThread*
TraceLoggerThreadState::forThread(PRThread* thread)
{
    MOZ_ASSERT(initialized);

    AutoTraceLoggerThreadStateLock lock(this);

    ThreadLoggerHashMap::AddPtr p = threadLoggers.lookupForAdd(thread);
    if (p)
        return p->value();

    TraceLoggerThread* logger = create();
    if (!logger)
        return nullptr;

    if (!threadLoggers.add(p, thread, logger)) {
        js_delete(logger);
        return nullptr;
    }

    if (graphSpewingEnabled)
        logger->initGraph();

    if (offThreadEnabled)
        logger->enable();

    return logger;
}

TraceLoggerThread*
TraceLoggerThreadState::create()
{
    TraceLoggerThread* logger = js_new<TraceLoggerThread>();
    if (!logger)
        return nullptr;

    if (!logger->init()) {
        js_delete(logger);
        return nullptr;
    }

    return logger;
}

bool
js::TraceLogTextIdEnabled(uint32_t textId)
{
    if (!EnsureTraceLoggerState())
        return false;
    return traceLoggerState->isTextIdEnabled(textId);
}

void
js::TraceLogEnableTextId(JSContext* cx, uint32_t textId)
{
    if (!EnsureTraceLoggerState())
        return;
    traceLoggerState->enableTextId(cx, textId);
}
void
js::TraceLogDisableTextId(JSContext* cx, uint32_t textId)
{
    if (!EnsureTraceLoggerState())
        return;
    traceLoggerState->disableTextId(cx, textId);
}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerThread* logger, TraceLoggerTextId textId)
{
    payload_ = nullptr;
    if (logger) {
        payload_ = logger->getOrCreateEventPayload(textId);
        if (payload_)
            payload_->use();
    }
}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerThread* logger, TraceLoggerTextId type,
                                   JSScript* script)
{
    payload_ = nullptr;
    if (logger) {
        payload_ = logger->getOrCreateEventPayload(type, script);
        if (payload_)
            payload_->use();
    }
}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerThread* logger, TraceLoggerTextId type,
                                   const JS::ReadOnlyCompileOptions& compileOptions)
{
    payload_ = nullptr;
    if (logger) {
        payload_ = logger->getOrCreateEventPayload(type, compileOptions);
        if (payload_)
            payload_->use();
    }
}

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerThread* logger, const char* text)
{
    payload_ = nullptr;
    if (logger) {
        payload_ = logger->getOrCreateEventPayload(text);
        if (payload_)
            payload_->use();
    }
}

TraceLoggerEvent::~TraceLoggerEvent()
{
    if (payload_)
        payload_->release();
}

TraceLoggerEvent&
TraceLoggerEvent::operator=(const TraceLoggerEvent& other)
{
    if (hasPayload())
        payload()->release();
    if (other.hasPayload())
        other.payload()->use();

    payload_ = other.payload_;

    return *this;
}
