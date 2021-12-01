/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TraceLogging.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ScopeExit.h"

#include <string.h>

#include "jsapi.h"

#include "jit/BaselineJIT.h"
#include "jit/CompileWrappers.h"
#include "threading/LockGuard.h"
#include "vm/JSScript.h"
#include "vm/Runtime.h"
#include "vm/Time.h"
#include "vm/TraceLoggingGraph.h"

#include "jit/JitFrames-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

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
#elif defined(__arm__) || defined(__aarch64__)

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

size_t
js::SizeOfTraceLogState(mozilla::MallocSizeOf mallocSizeOf)
{
    return traceLoggerState ? traceLoggerState->sizeOfIncludingThis(mallocSizeOf) : 0;
}

void
js::DestroyTraceLoggerThreadState()
{
    if (traceLoggerState) {
        js_delete(traceLoggerState);
        traceLoggerState = nullptr;
    }
}

#ifdef DEBUG
bool
js::CurrentThreadOwnsTraceLoggerThreadStateLock()
{
    return traceLoggerState && traceLoggerState->lock.ownedByCurrentThread();
}
#endif

void
js::DestroyTraceLogger(TraceLoggerThread* logger)
{
    if (!EnsureTraceLoggerState())
        return;
    traceLoggerState->destroyLogger(logger);
}

bool
TraceLoggerThread::init()
{
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
}

bool
TraceLoggerThread::enable()
{
    if (enabled_ > 0) {
        enabled_++;
        return true;
    }

    if (failed)
        return false;

    enabled_ = 1;
    logTimestamp(TraceLogger_Enable);

    return true;
}

bool
TraceLoggerThread::fail(JSContext* cx, const char* error)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TRACELOGGER_ENABLE_FAIL, error);
    failed = true;
    enabled_ = 0;

    return false;
}

void
TraceLoggerThread::silentFail(const char* error)
{
    traceLoggerState->maybeSpewError(error);
    failed = true;
    enabled_ = 0;
}

size_t
TraceLoggerThread::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    size_t size = 0;
#ifdef DEBUG
    size += graphStack.sizeOfExcludingThis(mallocSizeOf);
#endif
    size += events.sizeOfExcludingThis(mallocSizeOf);
    if (graph.get())
        size += graph->sizeOfIncludingThis(mallocSizeOf);
    return size;
}

size_t
TraceLoggerThread::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
}

bool
TraceLoggerThread::enable(JSContext* cx)
{
    if (!enable())
        return fail(cx, "internal error");

    if (enabled_ == 1) {
        // Get the top Activation to log the top script/pc (No inlined frames).
        ActivationIterator iter(cx);
        Activation* act = iter.activation();

        if (!act)
            return fail(cx, "internal error");

        JSScript* script = nullptr;
        int32_t engine = 0;

        if (act->isJit()) {
            JitFrameIter frame(iter->asJit());

            while (!frame.done()) {
                if (frame.isWasm()) {
                    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                              JSMSG_TRACELOGGER_ENABLE_FAIL,
                                              "not yet supported in wasm code");
                    return false;
                }
                if (frame.asJSJit().isScripted())
                    break;
                ++frame;
            }

            MOZ_ASSERT(!frame.done());

            const JSJitFrameIter& jitFrame = frame.asJSJit();
            MOZ_ASSERT(jitFrame.isIonJS() || jitFrame.isBaselineJS());

            script = jitFrame.script();
            engine = jitFrame.isIonJS() ? TraceLogger_IonMonkey : TraceLogger_Baseline;
        } else {
            MOZ_ASSERT(act->isInterpreter());
            InterpreterFrame* fp = act->asInterpreter()->current();
            MOZ_ASSERT(!fp->runningInJit());

            script = fp->script();
            engine = TraceLogger_Interpreter;
        }
        if (script->compartment() != cx->compartment())
            return fail(cx, "compartment mismatch");

        TraceLoggerEvent event(TraceLogger_Scripts, script);
        startEvent(event);
        startEvent(engine);
    }

    return true;
}

bool
TraceLoggerThread::disable(bool force, const char* error)
{
    if (failed) {
        MOZ_ASSERT(enabled_ == 0);
        return false;
    }

    if (enabled_ == 0)
        return true;

    if (enabled_ > 1 && !force) {
        enabled_--;
        return true;
    }

    if (force)
        traceLoggerState->maybeSpewError(error);

    logTimestamp(TraceLogger_Disable);
    enabled_ = 0;

    return true;
}

const char*
TraceLoggerThread::maybeEventText(uint32_t id)
{
    if (id < TraceLogger_Last)
        return TLTextIdString(static_cast<TraceLoggerTextId>(id));
    return traceLoggerState->maybeEventText(id);
}

const char*
TraceLoggerThreadState::maybeEventText(uint32_t id)
{
    LockGuard<Mutex> guard(lock);

    TextIdHashMap::Ptr p = textIdPayloads.lookup(id);
    if (!p)
        return nullptr;

    return p->value()->string();
}

size_t
TraceLoggerThreadState::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    LockGuard<Mutex> guard(lock);

    // Do not count threadLoggers since they are counted by JSContext::traceLogger.

    size_t size = 0;
    size += pointerMap.sizeOfExcludingThis(mallocSizeOf);
    if (textIdPayloads.initialized()) {
        size += textIdPayloads.sizeOfExcludingThis(mallocSizeOf);
        for (TextIdHashMap::Range r = textIdPayloads.all(); !r.empty(); r.popFront())
            r.front().value()->sizeOfIncludingThis(mallocSizeOf);
    }
    return size;
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
TraceLoggerThreadState::getOrCreateEventPayload(const char* text)
{
    LockGuard<Mutex> guard(lock);

    PointerHashMap::AddPtr p = pointerMap.lookupForAdd((const void*)text);
    if (p) {
        MOZ_ASSERT(p->value()->textId() < nextTextId); // Sanity check.
        p->value()->use();
        return p->value();
    }

    char* str = js_strdup(text);
    if (!str)
        return nullptr;

    uint32_t textId = nextTextId;

    TraceLoggerEventPayload* payload = js_new<TraceLoggerEventPayload>(textId, str);
    if (!payload) {
        js_free(str);
        return nullptr;
    }

    if (!textIdPayloads.putNew(textId, payload)) {
        js_delete(payload);
        payload = nullptr;
        return nullptr;
    }

    payload->use();

    nextTextId++;

    if (!pointerMap.add(p, text, payload))
        return nullptr;

    payload->incPointerCount();

    return payload;
}

TraceLoggerEventPayload*
TraceLoggerThreadState::getOrCreateEventPayload(const char* filename,
                                                size_t lineno, size_t colno, const void* ptr)
{
    if (!filename)
        filename = "<unknown>";

    LockGuard<Mutex> guard(lock);

    PointerHashMap::AddPtr p;
    if (ptr) {
        p = pointerMap.lookupForAdd(ptr);
        if (p) {
            MOZ_ASSERT(p->value()->textId() < nextTextId); // Sanity check.
            p->value()->use();
            return p->value();
        }
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
        snprintf(str, len + 1, "script %s:%zu:%zu", filename, lineno, colno);
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
        payload = nullptr;
        return nullptr;
    }

    payload->use();

    nextTextId++;

    if (ptr) {
        if (!pointerMap.add(p, ptr, payload))
            return nullptr;

        payload->incPointerCount();
    }

    return payload;
}

TraceLoggerEventPayload*
TraceLoggerThreadState::getOrCreateEventPayload(JSScript* script)
{
    return getOrCreateEventPayload(script->filename(), script->lineno(), script->column(), nullptr);
}

void
TraceLoggerThreadState::purgeUnusedPayloads()
{
    // Care needs to be taken to maintain a coherent state in this function,
    // as payloads can have their use count change at any time from non-zero to
    // zero (but not the other way around; see TraceLoggerEventPayload::use()).
    LockGuard<Mutex> guard(lock);

    // Remove all the pointers to payloads that have no uses anymore
    // and decrease the pointer count of that payload.
    for (PointerHashMap::Enum e(pointerMap); !e.empty(); e.popFront()) {
        if (e.front().value()->uses() == 0) {
            e.front().value()->decPointerCount();
            e.removeFront();
        }
    }

    // Free all other payloads that have no uses anymore.
    for (TextIdHashMap::Enum e(textIdPayloads); !e.empty(); e.popFront()) {
        if (e.front().value()->uses() == 0 && e.front().value()->pointerCount() == 0) {
            js_delete(e.front().value());
            e.removeFront();
        }
    }
}

void
TraceLoggerThread::startEvent(TraceLoggerTextId id) {
    startEvent(uint32_t(id));
}

void
TraceLoggerThread::startEvent(const TraceLoggerEvent& event) {
    if (!event.hasTextId()) {
        if (!enabled())
            return;
        startEvent(TraceLogger_Error);
        disable(/* force = */ true, "TraceLogger encountered an empty event. "
                                    "Potentially due to OOM during creation of "
                                    "this event. Disabling TraceLogger.");
        return;
    }
    startEvent(event.textId());
}

void
TraceLoggerThread::startEvent(uint32_t id)
{
    MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
    MOZ_ASSERT(traceLoggerState);
    if (!traceLoggerState->isTextIdEnabled(id))
       return;

#ifdef DEBUG
    if (enabled_ > 0) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!graphStack.append(id))
            oomUnsafe.crash("Could not add item to debug stack.");
    }
#endif

    if (graph.get()) {
        for (uint32_t otherId = graph->nextTextId(); otherId <= id; otherId++)
            graph->addTextId(otherId, maybeEventText(id));
    }

    log(id);
}

void
TraceLoggerThread::stopEvent(TraceLoggerTextId id) {
    stopEvent(uint32_t(id));
}

void
TraceLoggerThread::stopEvent(const TraceLoggerEvent& event) {
    if (!event.hasTextId()) {
        stopEvent(TraceLogger_Error);
        return;
    }
    stopEvent(event.textId());
}

void
TraceLoggerThread::stopEvent(uint32_t id)
{
    MOZ_ASSERT(TLTextIdIsTreeEvent(id) || id == TraceLogger_Error);
    MOZ_ASSERT(traceLoggerState);
    if (!traceLoggerState->isTextIdEnabled(id))
        return;

#ifdef DEBUG
    if (enabled_ > 0 && !graphStack.empty()) {
        uint32_t prev = graphStack.popCopy();
        if (id == TraceLogger_Error || prev == TraceLogger_Error) {
            // When encountering an Error id the stack will most likely not be correct anymore.
            // Ignore this.
        } else if (id == TraceLogger_Engine) {
            MOZ_ASSERT(prev == TraceLogger_IonMonkey || prev == TraceLogger_Baseline ||
                       prev == TraceLogger_Interpreter);
        } else if (id == TraceLogger_Scripts) {
            MOZ_ASSERT(prev >= TraceLogger_Last);
        } else if (id >= TraceLogger_Last) {
            MOZ_ASSERT(prev >= TraceLogger_Last);
            if (prev != id) {
                // Ignore if the text has been flushed already.
                MOZ_ASSERT_IF(maybeEventText(prev), strcmp(eventText(id), eventText(prev)) == 0);
            }
        } else {
            MOZ_ASSERT(id == prev);
        }
    }
#endif

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
    if (enabled_ == 0)
        return;

#ifdef DEBUG
    if (id == TraceLogger_Disable)
        graphStack.clear();
#endif

    MOZ_ASSERT(traceLoggerState);

    // We request for 3 items to add, since if we don't have enough room
    // we record the time it took to make more space. To log this information
    // we need 2 extra free entries.
    if (!events.hasSpaceForAdd(3)) {
        uint64_t start = rdtsc() - traceLoggerState->startupTime;

        if (!events.ensureSpaceBeforeAdd(3)) {
            if (graph.get())
                graph->log(events);

            iteration_++;
            events.clear();

            // Periodically remove unused payloads from the global logger state.
            traceLoggerState->purgeUnusedPayloads();
        }

        // Log the time it took to flush the events as being from the
        // Tracelogger.
        if (graph.get()) {
            MOZ_ASSERT(events.hasSpaceForAdd(2));
            EventEntry& entryStart = events.pushUninitialized();
            entryStart.time = start;
            entryStart.textId = TraceLogger_Internal;

            EventEntry& entryStop = events.pushUninitialized();
            entryStop.time = rdtsc() - traceLoggerState->startupTime;
            entryStop.textId = TraceLogger_Stop;
        }

    }

    uint64_t time = rdtsc() - traceLoggerState->startupTime;

    EventEntry& entry = events.pushUninitialized();
    entry.time = time;
    entry.textId = id;
}

TraceLoggerThreadState::~TraceLoggerThreadState()
{
    while (TraceLoggerThread* logger = threadLoggers.popFirst())
        js_delete(logger);

    threadLoggers.clear();

    if (textIdPayloads.initialized()) {
        for (TextIdHashMap::Range r = textIdPayloads.all(); !r.empty(); r.popFront())
            js_delete(r.front().value());
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
            "                 GCAllocation, GCSweeping, Interpreter, IonAnalysis, IonCompilation,\n"
            "                 IonLinking, IonMonkey, MinorGC, Frontend, ParsingFull,\n"
            "                 ParsingSyntax, BytecodeEmission, IrregexpCompile, IrregexpExecute,\n"
            "                 Scripts, Engine, WasmCompilation\n"
            "\n"
            "  IonCompiler    Output all information about compilation. It includes:\n"
            "                 IonCompilation, IonLinking, PruneUnusedBranches, FoldTests,\n"
            "                 SplitCriticalEdges, RenumberBlocks, ScalarReplacement, \n"
            "                 DominatorTree, PhiAnalysis, MakeLoopsContiguous, ApplyTypes, \n"
            "                 EagerSimdUnbox, AliasAnalysis, GVN, LICM, Sincos, RangeAnalysis, \n"
            "                 LoopUnrolling, FoldLinearArithConstants, EffectiveAddressAnalysis, \n"
            "                 AlignmentMaskAnalysis, EliminateDeadCode, ReorderInstructions, \n"
            "                 EdgeCaseAnalysis, EliminateRedundantChecks, \n"
            "                 AddKeepAliveInstructions, GenerateLIR, RegisterAllocation, \n"
            "                 GenerateCode, Scripts, IonBuilderRestartLoop\n"
            "\n"
            "  VMSpecific     Output the specific name of the VM call\n"
            "\n"
            "  Frontend       Output all information about frontend compilation. It includes:\n"
            "                 Frontend, ParsingFull, ParsingSyntax, Tokenizing,\n"
            "                 BytecodeEmission, BytecodeFoldConstants, BytecodeNameFunctions\n"
            "Specific log items:\n"
        );
        for (uint32_t i = 1; i < TraceLogger_Last; i++) {
            TraceLoggerTextId id = TraceLoggerTextId(i);
            if (!TLTextIdIsTogglable(id))
                continue;
            printf("  %s\n", TLTextIdString(id));
        }
        printf("\n");
        exit(0);
        /*NOTREACHED*/
    }

    for (uint32_t i = 1; i < TraceLogger_Last; i++) {
        TraceLoggerTextId id = TraceLoggerTextId(i);
        if (TLTextIdIsTogglable(id))
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
        enabledTextIds[TraceLogger_IonAnalysis] = true;
        enabledTextIds[TraceLogger_IonCompilation] = true;
        enabledTextIds[TraceLogger_IonLinking] = true;
        enabledTextIds[TraceLogger_IonMonkey] = true;
        enabledTextIds[TraceLogger_MinorGC] = true;
        enabledTextIds[TraceLogger_Frontend] = true;
        enabledTextIds[TraceLogger_ParsingFull] = true;
        enabledTextIds[TraceLogger_ParsingSyntax] = true;
        enabledTextIds[TraceLogger_BytecodeEmission] = true;
        enabledTextIds[TraceLogger_IrregexpCompile] = true;
        enabledTextIds[TraceLogger_IrregexpExecute] = true;
        enabledTextIds[TraceLogger_Scripts] = true;
        enabledTextIds[TraceLogger_Engine] = true;
        enabledTextIds[TraceLogger_WasmCompilation] = true;
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
        enabledTextIds[TraceLogger_FoldLinearArithConstants] = true;
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
        enabledTextIds[TraceLogger_IonBuilderRestartLoop] = true;
    }

    if (ContainsFlag(env, "Frontend")) {
        enabledTextIds[TraceLogger_Frontend] = true;
        enabledTextIds[TraceLogger_ParsingFull] = true;
        enabledTextIds[TraceLogger_ParsingSyntax] = true;
        enabledTextIds[TraceLogger_BytecodeEmission] = true;
        enabledTextIds[TraceLogger_BytecodeFoldConstants] = true;
        enabledTextIds[TraceLogger_BytecodeNameFunctions] = true;
    }

    enabledTextIds[TraceLogger_Interpreter] = enabledTextIds[TraceLogger_Engine];
    enabledTextIds[TraceLogger_Baseline] = enabledTextIds[TraceLogger_Engine];
    enabledTextIds[TraceLogger_IonMonkey] = enabledTextIds[TraceLogger_Engine];

    enabledTextIds[TraceLogger_Error] = true;

    const char* options = getenv("TLOPTIONS");
    if (options) {
        if (strstr(options, "help")) {
            fflush(nullptr);
            printf(
                "\n"
                "usage: TLOPTIONS=option,option,option,... where options can be:\n"
                "\n"
                "  EnableActiveThread      Start logging cooperating threads immediately.\n"
                "  EnableOffThread         Start logging helper threads immediately.\n"
                "  EnableGraph             Enable spewing the tracelogging graph to a file.\n"
                "  Errors                  Report errors during tracing to stderr.\n"
            );
            printf("\n");
            exit(0);
            /*NOTREACHED*/
        }

        if (strstr(options, "EnableActiveThread"))
            cooperatingThreadEnabled = true;
        if (strstr(options, "EnableOffThread"))
            helperThreadEnabled = true;
        if (strstr(options, "EnableGraph"))
            graphSpewingEnabled = true;
        if (strstr(options, "Errors"))
            spewErrors = true;
    }

    if (!pointerMap.init())
        return false;
    if (!textIdPayloads.init())
        return false;

    startupTime = rdtsc();

#ifdef DEBUG
    initialized = true;
#endif

    return true;
}

void
TraceLoggerThreadState::enableTextId(JSContext* cx, uint32_t textId)
{
    MOZ_ASSERT(TLTextIdIsTogglable(textId));

    if (enabledTextIds[textId])
        return;

    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

    enabledTextIds[textId] = true;
    if (textId == TraceLogger_Engine) {
        enabledTextIds[TraceLogger_IonMonkey] = true;
        enabledTextIds[TraceLogger_Baseline] = true;
        enabledTextIds[TraceLogger_Interpreter] = true;
    }

    if (textId == TraceLogger_Scripts)
        jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), true);
    if (textId == TraceLogger_Engine)
        jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), true);

}
void
TraceLoggerThreadState::disableTextId(JSContext* cx, uint32_t textId)
{
    MOZ_ASSERT(TLTextIdIsTogglable(textId));

    if (!enabledTextIds[textId])
        return;

    ReleaseAllJITCode(cx->runtime()->defaultFreeOp());

    enabledTextIds[textId] = false;
    if (textId == TraceLogger_Engine) {
        enabledTextIds[TraceLogger_IonMonkey] = false;
        enabledTextIds[TraceLogger_Baseline] = false;
        enabledTextIds[TraceLogger_Interpreter] = false;
    }

    if (textId == TraceLogger_Scripts)
        jit::ToggleBaselineTraceLoggerScripts(cx->runtime(), false);
    if (textId == TraceLogger_Engine)
        jit::ToggleBaselineTraceLoggerEngine(cx->runtime(), false);
}

TraceLoggerThread*
js::TraceLoggerForCurrentThread(JSContext* maybecx)
{
    if (!EnsureTraceLoggerState())
        return nullptr;
    return traceLoggerState->forCurrentThread(maybecx);
}

TraceLoggerThread*
TraceLoggerThreadState::forCurrentThread(JSContext* maybecx)
{
    MOZ_ASSERT(initialized);
    MOZ_ASSERT_IF(maybecx, maybecx == TlsContext.get());

    JSContext* cx = maybecx ? maybecx : TlsContext.get();
    if (!cx)
        return nullptr;

    if (!cx->traceLogger) {
        LockGuard<Mutex> guard(lock);

        TraceLoggerThread* logger = js_new<TraceLoggerThread>();
        if (!logger)
            return nullptr;

        if (!logger->init()) {
            js_delete(logger);
            return nullptr;
        }

        threadLoggers.insertFront(logger);
        cx->traceLogger = logger;

        if (graphSpewingEnabled)
            logger->initGraph();

        if (CurrentHelperThread() ? helperThreadEnabled : cooperatingThreadEnabled)
            logger->enable();
    }

    return cx->traceLogger;
}

void
TraceLoggerThreadState::destroyLogger(TraceLoggerThread* logger)
{
    MOZ_ASSERT(initialized);
    MOZ_ASSERT(logger);
    LockGuard<Mutex> guard(lock);

    logger->remove();
    js_delete(logger);
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

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerTextId type, JSScript* script)
  : TraceLoggerEvent(type, script->filename(), script->lineno(), script->column())
{ }

TraceLoggerEvent::TraceLoggerEvent(TraceLoggerTextId type, const char* filename, size_t line,
                                   size_t column)
  : payload_()
{
    MOZ_ASSERT(type == TraceLogger_Scripts || type == TraceLogger_AnnotateScripts ||
               type == TraceLogger_InlinedScripts || type == TraceLogger_Frontend);

    if (!traceLoggerState)
        return;

    // Only log scripts when enabled, otherwise use the more generic type
    // (which will get filtered out).
    if (!traceLoggerState->isTextIdEnabled(type)) {
        payload_.setTextId(type);
        return;
    }

    payload_.setEventPayload(
        traceLoggerState->getOrCreateEventPayload(filename, line, column, nullptr));
}

TraceLoggerEvent::TraceLoggerEvent(const char* text)
  : payload_()
{
    if (traceLoggerState)
        payload_.setEventPayload(traceLoggerState->getOrCreateEventPayload(text));
}

TraceLoggerEvent::~TraceLoggerEvent()
{
    if (hasExtPayload())
        extPayload()->release();
}

uint32_t
TraceLoggerEvent::textId() const
{
    MOZ_ASSERT(hasTextId());
    if (hasExtPayload())
        return extPayload()->textId();
    return payload_.textId();
}

TraceLoggerEvent&
TraceLoggerEvent::operator=(const TraceLoggerEvent& other)
{
    if (other.hasExtPayload())
        other.extPayload()->use();
    if (hasExtPayload())
        extPayload()->release();

    payload_ = other.payload_;

    return *this;
}

TraceLoggerEvent::TraceLoggerEvent(const TraceLoggerEvent& other)
  : payload_(other.payload_)
{
    if (hasExtPayload())
        extPayload()->use();
}
