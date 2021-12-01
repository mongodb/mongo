/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Debugger-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TypeTraits.h"

#include "jsfriendapi.h"
#include "jsnum.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/Parser.h"
#include "gc/FreeOp.h"
#include "gc/HashUtil.h"
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "gc/PublicIterators.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineJIT.h"
#include "js/Date.h"
#include "js/UbiNodeBreadthFirst.h"
#include "js/Vector.h"
#include "js/Wrapper.h"
#include "proxy/ScriptedProxyHandler.h"
#include "vm/ArgumentsObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/DebuggerMemory.h"
#include "vm/GeckoProfiler.h"
#include "vm/GeneratorObject.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/TraceLogging.h"
#include "vm/WrapperObject.h"
#include "wasm/WasmInstance.h"

#include "gc/GC-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using JS::dbg::AutoEntryMonitor;
using JS::dbg::Builder;
using js::frontend::IsIdentifier;
using mozilla::DebugOnly;
using mozilla::MakeScopeExit;
using mozilla::Maybe;
using mozilla::Some;
using mozilla::Nothing;
using mozilla::AsVariant;
using mozilla::TimeDuration;
using mozilla::TimeStamp;


/*** Forward declarations, ClassOps and Classes **************************************************/

static void DebuggerFrame_finalize(FreeOp* fop, JSObject* obj);
static void DebuggerFrame_trace(JSTracer* trc, JSObject* obj);
static void DebuggerEnv_trace(JSTracer* trc, JSObject* obj);
static void DebuggerObject_trace(JSTracer* trc, JSObject* obj);
static void DebuggerScript_trace(JSTracer* trc, JSObject* obj);
static void DebuggerSource_trace(JSTracer* trc, JSObject* obj);

enum {
    JSSLOT_DEBUGFRAME_OWNER,
    JSSLOT_DEBUGFRAME_ARGUMENTS,
    JSSLOT_DEBUGFRAME_ONSTEP_HANDLER,
    JSSLOT_DEBUGFRAME_ONPOP_HANDLER,
    JSSLOT_DEBUGFRAME_COUNT
};

const ClassOps DebuggerFrame::classOps_ = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    DebuggerFrame_finalize,
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    DebuggerFrame_trace
};

const Class DebuggerFrame::class_ = {
    "Frame",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGFRAME_COUNT) |
    JSCLASS_BACKGROUND_FINALIZE,
    &DebuggerFrame::classOps_
};

enum {
    JSSLOT_DEBUGARGUMENTS_FRAME,
    JSSLOT_DEBUGARGUMENTS_COUNT
};

const Class DebuggerArguments::class_ = {
    "Arguments",
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGARGUMENTS_COUNT)
};

const ClassOps DebuggerEnvironment::classOps_ = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    nullptr,    /* finalize    */
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    DebuggerEnv_trace
};

const Class DebuggerEnvironment::class_ = {
    "Environment",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(DebuggerEnvironment::RESERVED_SLOTS),
    &classOps_
};

enum {
    JSSLOT_DEBUGOBJECT_OWNER,
    JSSLOT_DEBUGOBJECT_COUNT
};

const ClassOps DebuggerObject::classOps_ = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    nullptr,    /* finalize    */
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    DebuggerObject_trace
};

const Class DebuggerObject::class_ = {
    "Object",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS),
    &classOps_
};

enum {
    JSSLOT_DEBUGSCRIPT_OWNER,
    JSSLOT_DEBUGSCRIPT_COUNT
};

static const ClassOps DebuggerScript_classOps = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    nullptr,    /* finalize    */
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    DebuggerScript_trace
};

static const Class DebuggerScript_class = {
    "Script",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGSCRIPT_COUNT),
    &DebuggerScript_classOps
};

enum {
    JSSLOT_DEBUGSOURCE_OWNER,
    JSSLOT_DEBUGSOURCE_TEXT,
    JSSLOT_DEBUGSOURCE_COUNT
};

static const ClassOps DebuggerSource_classOps = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    nullptr,    /* finalize    */
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    DebuggerSource_trace
};

static const Class DebuggerSource_class = {
    "Source",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGSOURCE_COUNT),
    &DebuggerSource_classOps
};


/*** Utils ***************************************************************************************/

/*
 * If fun is an interpreted function, remove any async function/generator
 * wrapper and return the underlying scripted function. Otherwise, return fun
 * unchanged.
 *
 * Async functions are implemented as native functions wrapped around a scripted
 * function. JSScripts hold ordinary inner JSFunctions in their object arrays,
 * and when we need to actually create a JS-visible function object, we build an
 * ordinary JS closure and apply the async wrapper to it. Async generators are
 * similar.
 *
 * This means that JSFunction::isInterpreted returns false for such functions,
 * even though their actual code is indeed JavaScript. Debugger should treat
 * async functions and generators like any other scripted function, so we must
 * carefully check for them whenever we want inspect a function.
 */

static JSFunction*
RemoveAsyncWrapper(JSFunction *fun)
{
    if (js::IsWrappedAsyncFunction(fun))
        fun = js::GetUnwrappedAsyncFunction(fun);
    else if (js::IsWrappedAsyncGenerator(fun))
        fun = js::GetUnwrappedAsyncGenerator(fun);

    return fun;
}

static inline bool
EnsureFunctionHasScript(JSContext* cx, HandleFunction fun)
{
    if (fun->isInterpretedLazy()) {
        AutoCompartment ac(cx, fun);
        return !!JSFunction::getOrCreateScript(cx, fun);
    }
    return true;
}

static inline JSScript*
GetOrCreateFunctionScript(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(fun->isInterpreted());
    if (!EnsureFunctionHasScript(cx, fun))
        return nullptr;
    return fun->nonLazyScript();
}

static bool
ValueToIdentifier(JSContext* cx, HandleValue v, MutableHandleId id)
{
    if (!ValueToId<CanGC>(cx, v, id))
        return false;
    if (!JSID_IS_ATOM(id) || !IsIdentifier(JSID_TO_ATOM(id))) {
        RootedValue val(cx, v);
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, val, nullptr, "not an identifier",
                              nullptr);
        return false;
    }
    return true;
}

class AutoRestoreCompartmentDebugMode
{
    JSCompartment* comp_;
    unsigned bits_;

  public:
    explicit AutoRestoreCompartmentDebugMode(JSCompartment* comp)
      : comp_(comp), bits_(comp->debugModeBits)
    {
        MOZ_ASSERT(comp_);
    }

    ~AutoRestoreCompartmentDebugMode() {
        if (comp_)
            comp_->debugModeBits = bits_;
    }

    void release() {
        comp_ = nullptr;
    }
};

// Given a Debugger instance dbg, if it is enabled, prevents all its debuggee
// compartments from executing scripts. Attempts to run script will throw an
// instance of Debugger.DebuggeeWouldRun from the topmost locked Debugger's
// compartment.
class MOZ_RAII js::EnterDebuggeeNoExecute
{
    friend class js::LeaveDebuggeeNoExecute;

    Debugger& dbg_;
    EnterDebuggeeNoExecute** stack_;
    EnterDebuggeeNoExecute* prev_;

    // Non-nullptr when unlocked temporarily by a LeaveDebuggeeNoExecute.
    LeaveDebuggeeNoExecute* unlocked_;

    // When DebuggeeWouldRun is a warning instead of an error, whether we've
    // reported a warning already.
    bool reported_;

  public:
    explicit EnterDebuggeeNoExecute(JSContext* cx, Debugger& dbg)
      : dbg_(dbg),
        unlocked_(nullptr),
        reported_(false)
    {
        stack_ = &cx->noExecuteDebuggerTop.ref();
        prev_ = *stack_;
        *stack_ = this;
    }

    ~EnterDebuggeeNoExecute() {
        MOZ_ASSERT(*stack_ == this);
        *stack_ = prev_;
    }

    Debugger& debugger() const {
        return dbg_;
    }

#ifdef DEBUG
    static bool isLockedInStack(JSContext* cx, Debugger& dbg) {
        for (EnterDebuggeeNoExecute* it = cx->noExecuteDebuggerTop; it; it = it->prev_) {
            if (&it->debugger() == &dbg)
                return !it->unlocked_;
        }
        return false;
    }
#endif

    // Given a JSContext entered into a debuggee compartment, find the lock
    // that locks it. Returns nullptr if not found.
    static EnterDebuggeeNoExecute* findInStack(JSContext* cx) {
        JSCompartment* debuggee = cx->compartment();
        for (EnterDebuggeeNoExecute* it = cx->noExecuteDebuggerTop; it; it = it->prev_) {
            Debugger& dbg = it->debugger();
            if (!it->unlocked_ && dbg.isEnabled() && dbg.observesGlobal(debuggee->maybeGlobal()))
                return it;
        }
        return nullptr;
    }

    // Given a JSContext entered into a debuggee compartment, report a
    // warning or an error if there is a lock that locks it.
    static bool reportIfFoundInStack(JSContext* cx, HandleScript script) {
        if (EnterDebuggeeNoExecute* nx = findInStack(cx)) {
            bool warning = !cx->options().throwOnDebuggeeWouldRun();
            if (!warning || !nx->reported_) {
                AutoCompartment ac(cx, nx->debugger().toJSObject());
                nx->reported_ = true;
                if (cx->options().dumpStackOnDebuggeeWouldRun()) {
                    fprintf(stdout, "Dumping stack for DebuggeeWouldRun:\n");
                    DumpBacktrace(cx);
                }
                const char* filename = script->filename() ? script->filename() : "(none)";
                char linenoStr[15];
                SprintfLiteral(linenoStr, "%zu", script->lineno());
                unsigned flags = warning ? JSREPORT_WARNING : JSREPORT_ERROR;
                // FIXME: filename should be UTF-8 (bug 987069).
                return JS_ReportErrorFlagsAndNumberLatin1(cx, flags, GetErrorMessage, nullptr,
                                                          JSMSG_DEBUGGEE_WOULD_RUN,
                                                          filename, linenoStr);
            }
        }
        return true;
    }
};

// Given a JSContext entered into a debuggee compartment, if it is in
// an NX section, unlock the topmost EnterDebuggeeNoExecute instance.
//
// Does nothing if debuggee is not in an NX section. For example, this
// situation arises when invocation functions are called without entering
// Debugger code, e.g., calling D.O.p.executeInGlobal or D.O.p.apply.
class MOZ_RAII js::LeaveDebuggeeNoExecute
{
    EnterDebuggeeNoExecute* prevLocked_;

  public:
    explicit LeaveDebuggeeNoExecute(JSContext* cx)
      : prevLocked_(EnterDebuggeeNoExecute::findInStack(cx))
    {
        if (prevLocked_) {
            MOZ_ASSERT(!prevLocked_->unlocked_);
            prevLocked_->unlocked_ = this;
        }
    }

    ~LeaveDebuggeeNoExecute() {
        if (prevLocked_) {
            MOZ_ASSERT(prevLocked_->unlocked_ == this);
            prevLocked_->unlocked_ = nullptr;
        }
    }
};

/* static */ bool
Debugger::slowPathCheckNoExecute(JSContext* cx, HandleScript script)
{
    MOZ_ASSERT(cx->compartment()->isDebuggee());
    MOZ_ASSERT(cx->noExecuteDebuggerTop);
    return EnterDebuggeeNoExecute::reportIfFoundInStack(cx, script);
}

static inline void
NukeDebuggerWrapper(NativeObject *wrapper)
{
    // In some OOM failure cases, we need to destroy the edge to the referent,
    // to avoid trying to trace it during untimely collections.
    wrapper->setPrivate(nullptr);
}

static bool
ValueToStableChars(JSContext* cx, const char *fnname, HandleValue value,
                   AutoStableStringChars& stableChars)
{
    if (!value.isString()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  fnname, "string", InformalValueTypeName(value));
        return false;
    }
    RootedLinearString linear(cx, value.toString()->ensureLinear(cx));
    if (!linear)
        return false;
    if (!stableChars.initTwoByte(cx, linear))
        return false;
    return true;
}

EvalOptions::~EvalOptions()
{
    js_free(const_cast<char*>(filename_));
}

bool
EvalOptions::setFilename(JSContext* cx, const char* filename)
{
    char* copy = nullptr;
    if (filename) {
        copy = JS_strdup(cx, filename);
        if (!copy)
            return false;
    }

    // EvalOptions always owns filename_, so this cast is okay.
    js_free(const_cast<char*>(filename_));

    filename_ = copy;
    return true;
}

static bool
ParseEvalOptions(JSContext* cx, HandleValue value, EvalOptions& options)
{
    if (!value.isObject())
        return true;

    RootedObject opts(cx, &value.toObject());

    RootedValue v(cx);
    if (!JS_GetProperty(cx, opts, "url", &v))
        return false;
    if (!v.isUndefined()) {
        RootedString url_str(cx, ToString<CanGC>(cx, v));
        if (!url_str)
            return false;
        JSAutoByteString url_bytes(cx, url_str);
        if (!url_bytes)
            return false;
        if (!options.setFilename(cx, url_bytes.ptr()))
            return false;
    }

    if (!JS_GetProperty(cx, opts, "lineNumber", &v))
        return false;
    if (!v.isUndefined()) {
        uint32_t lineno;
        if (!ToUint32(cx, v, &lineno))
            return false;
        options.setLineno(lineno);
    }

    return true;
}

static bool
RequireGlobalObject(JSContext* cx, HandleValue dbgobj, HandleObject referent)
{
    RootedObject obj(cx, referent);

    if (!obj->is<GlobalObject>()) {
        const char* isWrapper = "";
        const char* isWindowProxy = "";

        /* Help the poor programmer by pointing out wrappers around globals... */
        if (obj->is<WrapperObject>()) {
            obj = js::UncheckedUnwrap(obj);
            isWrapper = "a wrapper around ";
        }

        /* ... and WindowProxies around Windows. */
        if (IsWindowProxy(obj)) {
            obj = ToWindowIfWindowProxy(obj);
            isWindowProxy = "a WindowProxy referring to ";
        }

        if (obj->is<GlobalObject>()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_WRAPPER_IN_WAY,
                                  JSDVG_SEARCH_STACK, dbgobj, nullptr,
                                  isWrapper, isWindowProxy);
        } else {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                                  JSDVG_SEARCH_STACK, dbgobj, nullptr,
                                  "a global object", nullptr);
        }
        return false;
    }

    return true;
}


/*** Breakpoints *********************************************************************************/

BreakpointSite::BreakpointSite(Type type)
  : type_(type), enabledCount(0)
{
}

void
BreakpointSite::inc(FreeOp* fop)
{
    enabledCount++;
    if (enabledCount == 1)
        recompile(fop);
}

void
BreakpointSite::dec(FreeOp* fop)
{
    MOZ_ASSERT(enabledCount > 0);
    enabledCount--;
    if (enabledCount == 0)
        recompile(fop);
}

bool
BreakpointSite::isEmpty() const
{
    return breakpoints.isEmpty();
}

Breakpoint*
BreakpointSite::firstBreakpoint() const
{
    if (isEmpty())
        return nullptr;
    return &(*breakpoints.begin());
}

bool
BreakpointSite::hasBreakpoint(Breakpoint* toFind)
{
    const BreakpointList::Iterator bp(toFind);
    for (auto p = breakpoints.begin(); p; p++)
        if (p == bp)
            return true;
    return false;
}

Breakpoint::Breakpoint(Debugger* debugger, BreakpointSite* site, JSObject* handler)
    : debugger(debugger), site(site), handler(handler)
{
    MOZ_ASSERT(handler->compartment() == debugger->object->compartment());
    debugger->breakpoints.pushBack(this);
    site->breakpoints.pushBack(this);
}

void
Breakpoint::destroy(FreeOp* fop)
{
    if (debugger->enabled)
        site->dec(fop);
    debugger->breakpoints.remove(this);
    site->breakpoints.remove(this);
    site->destroyIfEmpty(fop);
    fop->delete_(this);
}

Breakpoint*
Breakpoint::nextInDebugger()
{
    return debuggerLink.mNext;
}

Breakpoint*
Breakpoint::nextInSite()
{
    return siteLink.mNext;
}

JSBreakpointSite::JSBreakpointSite(JSScript* script, jsbytecode* pc)
  : BreakpointSite(Type::JS),
    script(script),
    pc(pc)
{
    MOZ_ASSERT(!script->hasBreakpointsAt(pc));
}

void
JSBreakpointSite::recompile(FreeOp* fop)
{
    if (script->hasBaselineScript())
        script->baselineScript()->toggleDebugTraps(script, pc);
}

void
JSBreakpointSite::destroyIfEmpty(FreeOp* fop)
{
    if (isEmpty())
        script->destroyBreakpointSite(fop, pc);
}

WasmBreakpointSite::WasmBreakpointSite(wasm::DebugState* debug_, uint32_t offset_)
  : BreakpointSite(Type::Wasm), debug(debug_), offset(offset_)
{
    MOZ_ASSERT(debug_);
}

void
WasmBreakpointSite::recompile(FreeOp* fop)
{
    debug->toggleBreakpointTrap(fop->runtime(), offset, isEnabled());
}

void
WasmBreakpointSite::destroyIfEmpty(FreeOp* fop)
{
    if (isEmpty())
        debug->destroyBreakpointSite(fop, offset);
}

/*** Debugger hook dispatch **********************************************************************/

Debugger::Debugger(JSContext* cx, NativeObject* dbg)
  : object(dbg),
    debuggees(cx->zone()),
    uncaughtExceptionHook(nullptr),
    enabled(true),
    allowUnobservedAsmJS(false),
    allowWasmBinarySource(false),
    collectCoverageInfo(false),
    observedGCs(cx->zone()),
    allocationsLog(cx),
    trackingAllocationSites(false),
    allocationSamplingProbability(1.0),
    maxAllocationsLogLength(DEFAULT_MAX_LOG_LENGTH),
    allocationsLogOverflowed(false),
    frames(cx->zone()),
    scripts(cx),
    sources(cx),
    objects(cx),
    environments(cx),
    wasmInstanceScripts(cx),
    wasmInstanceSources(cx),
#ifdef NIGHTLY_BUILD
    traceLoggerLastDrainedSize(0),
    traceLoggerLastDrainedIteration(0),
#endif
    traceLoggerScriptedCallsLastDrainedSize(0),
    traceLoggerScriptedCallsLastDrainedIteration(0)
{
    assertSameCompartment(cx, dbg);

#ifdef JS_TRACE_LOGGING
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    if (logger) {
#ifdef NIGHTLY_BUILD
        logger->getIterationAndSize(&traceLoggerLastDrainedIteration, &traceLoggerLastDrainedSize);
#endif
        logger->getIterationAndSize(&traceLoggerScriptedCallsLastDrainedIteration,
                                    &traceLoggerScriptedCallsLastDrainedSize);
    }
#endif
}

Debugger::~Debugger()
{
    MOZ_ASSERT_IF(debuggees.initialized(), debuggees.empty());
    allocationsLog.clear();

    /*
     * We don't have to worry about locking here since Debugger is not
     * background finalized.
     */
    JSContext* cx = TlsContext.get();
    if (onNewGlobalObjectWatchersLink.mPrev ||
        onNewGlobalObjectWatchersLink.mNext ||
        cx->runtime()->onNewGlobalObjectWatchers().begin() == JSRuntime::WatchersList::Iterator(this))
        cx->runtime()->onNewGlobalObjectWatchers().remove(this);

    cx->runtime()->endSingleThreadedExecution(cx);
}

bool
Debugger::init(JSContext* cx)
{
    if (!debuggees.init() ||
        !debuggeeZones.init() ||
        !frames.init() ||
        !scripts.init() ||
        !sources.init() ||
        !objects.init() ||
        !observedGCs.init() ||
        !environments.init() ||
        !wasmInstanceScripts.init() ||
        !wasmInstanceSources.init())
    {
        ReportOutOfMemory(cx);
        return false;
    }

    cx->zone()->group()->debuggerList().insertBack(this);
    return true;
}

JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGSCRIPT_OWNER));
JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGSOURCE_OWNER));
JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGOBJECT_OWNER));
JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(DebuggerEnvironment::OWNER_SLOT));

/* static */ Debugger*
Debugger::fromChildJSObject(JSObject* obj)
{
    MOZ_ASSERT(obj->getClass() == &DebuggerFrame::class_ ||
               obj->getClass() == &DebuggerScript_class ||
               obj->getClass() == &DebuggerSource_class ||
               obj->getClass() == &DebuggerObject::class_ ||
               obj->getClass() == &DebuggerEnvironment::class_);
    JSObject* dbgobj = &obj->as<NativeObject>().getReservedSlot(JSSLOT_DEBUGOBJECT_OWNER).toObject();
    return fromJSObject(dbgobj);
}

bool
Debugger::hasMemory() const
{
    return object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE).isObject();
}

DebuggerMemory&
Debugger::memory() const
{
    MOZ_ASSERT(hasMemory());
    return object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE).toObject().as<DebuggerMemory>();
}

bool
Debugger::getScriptFrameWithIter(JSContext* cx, AbstractFramePtr referent,
                                 const FrameIter* maybeIter, MutableHandleValue vp)
{
    RootedDebuggerFrame result(cx);
    if (!Debugger::getScriptFrameWithIter(cx, referent, maybeIter, &result))
        return false;

    vp.setObject(*result);
    return true;
}

bool
Debugger::getScriptFrameWithIter(JSContext* cx, AbstractFramePtr referent,
                                 const FrameIter* maybeIter,
                                 MutableHandleDebuggerFrame result)
{
    MOZ_ASSERT_IF(maybeIter, maybeIter->abstractFramePtr() == referent);
    MOZ_ASSERT_IF(referent.hasScript(), !referent.script()->selfHosted());

    if (referent.hasScript() && !referent.script()->ensureHasAnalyzedArgsUsage(cx))
        return false;

    FrameMap::AddPtr p = frames.lookupForAdd(referent);
    if (!p) {
        /* Create and populate the Debugger.Frame object. */
        RootedObject proto(cx, &object->getReservedSlot(JSSLOT_DEBUG_FRAME_PROTO).toObject());
        RootedNativeObject debugger(cx, object);

        RootedDebuggerFrame frame(cx, DebuggerFrame::create(cx, proto, referent, maybeIter,
                                                            debugger));
        if (!frame)
            return false;

        if (!ensureExecutionObservabilityOfFrame(cx, referent))
            return false;

        if (!frames.add(p, referent, frame)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    result.set(&p->value()->as<DebuggerFrame>());
    return true;
}

/* static */ bool
Debugger::hasLiveHook(GlobalObject* global, Hook which)
{
    if (GlobalObject::DebuggerVector* debuggers = global->getDebuggers()) {
        for (auto p = debuggers->begin(); p != debuggers->end(); p++) {
            Debugger* dbg = *p;
            if (dbg->enabled && dbg->getHook(which))
                return true;
        }
    }
    return false;
}

JSObject*
Debugger::getHook(Hook hook) const
{
    MOZ_ASSERT(hook >= 0 && hook < HookCount);
    const Value& v = object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + hook);
    return v.isUndefined() ? nullptr : &v.toObject();
}

bool
Debugger::hasAnyLiveHooks(JSRuntime* rt) const
{
    if (!enabled)
        return false;

    if (getHook(OnDebuggerStatement) ||
        getHook(OnExceptionUnwind) ||
        getHook(OnNewScript) ||
        getHook(OnEnterFrame))
    {
        return true;
    }

    /* If any breakpoints are in live scripts, return true. */
    for (Breakpoint* bp = firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
        switch (bp->site->type()) {
          case BreakpointSite::Type::JS:
            if (IsMarkedUnbarriered(rt, &bp->site->asJS()->script))
                return true;
            break;
          case BreakpointSite::Type::Wasm:
            if (IsMarkedUnbarriered(rt, &bp->asWasm()->wasmInstance))
                return true;
            break;
        }
    }

    for (FrameMap::Range r = frames.all(); !r.empty(); r.popFront()) {
        NativeObject* frameObj = r.front().value();
        if (!frameObj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined() ||
            !frameObj->getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER).isUndefined())
            return true;
    }

    return false;
}

/* static */ JSTrapStatus
Debugger::slowPathOnEnterFrame(JSContext* cx, AbstractFramePtr frame)
{
    RootedValue rval(cx);
    JSTrapStatus status = dispatchHook(
        cx,
        [frame](Debugger* dbg) -> bool {
            return dbg->observesFrame(frame) && dbg->observesEnterFrame();
        },
        [&](Debugger* dbg) -> JSTrapStatus {
            return dbg->fireEnterFrame(cx, &rval);
        });

    switch (status) {
      case JSTRAP_CONTINUE:
        break;

      case JSTRAP_THROW:
        cx->setPendingException(rval);
        break;

      case JSTRAP_ERROR:
        cx->clearPendingException();
        break;

      case JSTRAP_RETURN:
        frame.setReturnValue(rval);
        break;

      default:
        MOZ_CRASH("bad Debugger::onEnterFrame JSTrapStatus value");
    }

    return status;
}

static void
DebuggerFrame_maybeDecrementFrameScriptStepModeCount(FreeOp* fop, AbstractFramePtr frame,
                                                     NativeObject* frameobj);

static void
DebuggerFrame_freeScriptFrameIterData(FreeOp* fop, JSObject* obj);

/*
 * Handle leaving a frame with debuggers watching. |frameOk| indicates whether
 * the frame is exiting normally or abruptly. Set |cx|'s exception and/or
 * |cx->fp()|'s return value, and return a new success value.
 */
/* static */ bool
Debugger::slowPathOnLeaveFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc, bool frameOk)
{
    mozilla::DebugOnly<Handle<GlobalObject*>> debuggeeGlobal = cx->global();

    auto frameMapsGuard = MakeScopeExit([&] {
        // Clean up all Debugger.Frame instances.
        removeFromFrameMapsAndClearBreakpointsIn(cx, frame);
    });

    // The onPop handler and associated clean up logic should not run multiple
    // times on the same frame. If slowPathOnLeaveFrame has already been
    // called, the frame will not be present in the Debugger frame maps.
    Rooted<DebuggerFrameVector> frames(cx, DebuggerFrameVector(cx));
    if (!getDebuggerFrames(frame, &frames))
        return false;
    if (frames.empty())
        return frameOk;

    /* Save the frame's completion value. */
    JSTrapStatus status;
    RootedValue value(cx);
    Debugger::resultToCompletion(cx, frameOk, frame.returnValue(), &status, &value);

    // This path can be hit via unwinding the stack due to over-recursion or
    // OOM. In those cases, don't fire the frames' onPop handlers, because
    // invoking JS will only trigger the same condition. See
    // slowPathOnExceptionUnwind.
    if (!cx->isThrowingOverRecursed() && !cx->isThrowingOutOfMemory()) {
        /* For each Debugger.Frame, fire its onPop handler, if any. */
        for (size_t i = 0; i < frames.length(); i++) {
            HandleDebuggerFrame frameobj = frames[i];
            Debugger* dbg = Debugger::fromChildJSObject(frameobj);
            EnterDebuggeeNoExecute nx(cx, *dbg);

            if (dbg->enabled && frameobj->onPopHandler())
            {
                OnPopHandler* handler = frameobj->onPopHandler();

                Maybe<AutoCompartment> ac;
                ac.emplace(cx, dbg->object);

                RootedValue wrappedValue(cx, value);
                RootedValue completion(cx);
                if (!dbg->wrapDebuggeeValue(cx, &wrappedValue))
                {
                    status = dbg->reportUncaughtException(ac);
                    break;
                }

                /* Call the onPop handler. */
                JSTrapStatus nextStatus = status;
                RootedValue nextValue(cx, wrappedValue);
                bool success = handler->onPop(cx, frameobj, nextStatus, &nextValue);
                nextStatus = dbg->processParsedHandlerResult(ac, frame, pc, success, nextStatus,
                                                             &nextValue);

                /*
                 * At this point, we are back in the debuggee compartment, and any error has
                 * been wrapped up as a completion value.
                 */
                MOZ_ASSERT(cx->compartment() == debuggeeGlobal->compartment());
                MOZ_ASSERT(!cx->isExceptionPending());

                /* JSTRAP_CONTINUE means "make no change". */
                if (nextStatus != JSTRAP_CONTINUE) {
                    status = nextStatus;
                    value = nextValue;
                }
            }
        }
    }

    /* Establish (status, value) as our resumption value. */
    switch (status) {
      case JSTRAP_RETURN:
        frame.setReturnValue(value);
        return true;

      case JSTRAP_THROW:
        cx->setPendingException(value);
        return false;

      case JSTRAP_ERROR:
        MOZ_ASSERT(!cx->isExceptionPending());
        return false;

      default:
        MOZ_CRASH("bad final trap status");
    }
}

/* static */ JSTrapStatus
Debugger::slowPathOnDebuggerStatement(JSContext* cx, AbstractFramePtr frame)
{
    RootedValue rval(cx);
    JSTrapStatus status = dispatchHook(
        cx,
        [](Debugger* dbg) -> bool { return dbg->getHook(OnDebuggerStatement); },
        [&](Debugger* dbg) -> JSTrapStatus {
            return dbg->fireDebuggerStatement(cx, &rval);
        });

    switch (status) {
      case JSTRAP_CONTINUE:
      case JSTRAP_ERROR:
        break;

      case JSTRAP_RETURN:
        frame.setReturnValue(rval);
        break;

      case JSTRAP_THROW:
        cx->setPendingException(rval);
        break;

      default:
        MOZ_CRASH("Invalid onDebuggerStatement trap status");
    }

    return status;
}

/* static */ JSTrapStatus
Debugger::slowPathOnExceptionUnwind(JSContext* cx, AbstractFramePtr frame)
{
    // Invoking more JS on an over-recursed stack or after OOM is only going
    // to result in more of the same error.
    if (cx->isThrowingOverRecursed() || cx->isThrowingOutOfMemory())
        return JSTRAP_CONTINUE;

    // The Debugger API mustn't muck with frames from self-hosted scripts.
    if (frame.hasScript() && frame.script()->selfHosted())
        return JSTRAP_CONTINUE;

    RootedValue rval(cx);
    JSTrapStatus status = dispatchHook(
        cx,
        [](Debugger* dbg) -> bool { return dbg->getHook(OnExceptionUnwind); },
        [&](Debugger* dbg) -> JSTrapStatus {
            return dbg->fireExceptionUnwind(cx, &rval);
        });

    switch (status) {
      case JSTRAP_CONTINUE:
        break;

      case JSTRAP_THROW:
        cx->setPendingException(rval);
        break;

      case JSTRAP_ERROR:
        cx->clearPendingException();
        break;

      case JSTRAP_RETURN:
        cx->clearPendingException();
        frame.setReturnValue(rval);
        break;

      default:
        MOZ_CRASH("Invalid onExceptionUnwind trap status");
    }

    return status;
}

// TODO: Remove Remove this function when all properties/methods returning a
///      DebuggerEnvironment have been given a C++ interface (bug 1271649).
bool
Debugger::wrapEnvironment(JSContext* cx, Handle<Env*> env, MutableHandleValue rval)
{
    if (!env) {
        rval.setNull();
        return true;
    }

    RootedDebuggerEnvironment envobj(cx);

    if (!wrapEnvironment(cx, env, &envobj))
        return false;

    rval.setObject(*envobj);
    return true;
}

bool
Debugger::wrapEnvironment(JSContext* cx, Handle<Env*> env,
                          MutableHandleDebuggerEnvironment result)
{
    MOZ_ASSERT(env);

    /*
     * DebuggerEnv should only wrap a debug scope chain obtained (transitively)
     * from GetDebugEnvironmentFor(Frame|Function).
     */
    MOZ_ASSERT(!IsSyntacticEnvironment(env));

    DependentAddPtr<ObjectWeakMap> p(cx, environments, env);
    if (p) {
        result.set(&p->value()->as<DebuggerEnvironment>());
    } else {
        /* Create a new Debugger.Environment for env. */
        RootedObject proto(cx, &object->getReservedSlot(JSSLOT_DEBUG_ENV_PROTO).toObject());
        RootedNativeObject debugger(cx, object);

        RootedDebuggerEnvironment envobj(cx,
            DebuggerEnvironment::create(cx, proto, env, debugger));
        if (!envobj)
            return false;

        if (!p.add(cx, environments, env, envobj)) {
            NukeDebuggerWrapper(envobj);
            return false;
        }

        CrossCompartmentKey key(object, env, CrossCompartmentKey::DebuggerEnvironment);
        if (!object->compartment()->putWrapper(cx, key, ObjectValue(*envobj))) {
            NukeDebuggerWrapper(envobj);
            environments.remove(env);
            return false;
        }

        result.set(envobj);
    }

    return true;
}

bool
Debugger::wrapDebuggeeValue(JSContext* cx, MutableHandleValue vp)
{
    assertSameCompartment(cx, object.get());

    if (vp.isObject()) {
        RootedObject obj(cx, &vp.toObject());
        RootedDebuggerObject dobj(cx);

        if (!wrapDebuggeeObject(cx, obj, &dobj))
            return false;

        vp.setObject(*dobj);
    } else if (vp.isMagic()) {
        RootedPlainObject optObj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!optObj)
            return false;

        // We handle three sentinel values: missing arguments (overloading
        // JS_OPTIMIZED_ARGUMENTS), optimized out slots (JS_OPTIMIZED_OUT),
        // and uninitialized bindings (JS_UNINITIALIZED_LEXICAL).
        //
        // Other magic values should not have escaped.
        PropertyName* name;
        switch (vp.whyMagic()) {
          case JS_OPTIMIZED_ARGUMENTS:   name = cx->names().missingArguments; break;
          case JS_OPTIMIZED_OUT:         name = cx->names().optimizedOut; break;
          case JS_UNINITIALIZED_LEXICAL: name = cx->names().uninitialized; break;
          default: MOZ_CRASH("Unsupported magic value escaped to Debugger");
        }

        RootedValue trueVal(cx, BooleanValue(true));
        if (!DefineDataProperty(cx, optObj, name, trueVal))
            return false;

        vp.setObject(*optObj);
    } else if (!cx->compartment()->wrap(cx, vp)) {
        vp.setUndefined();
        return false;
    }

    return true;
}

bool
Debugger::wrapDebuggeeObject(JSContext* cx, HandleObject obj,
                             MutableHandleDebuggerObject result)
{
    MOZ_ASSERT(obj);

    if (obj->is<JSFunction>()) {
        MOZ_ASSERT(!IsInternalFunctionObject(*obj));
        RootedFunction fun(cx, &obj->as<JSFunction>());
        if (!EnsureFunctionHasScript(cx, fun))
            return false;
    }

    DependentAddPtr<ObjectWeakMap> p(cx, objects, obj);
    if (p) {
        result.set(&p->value()->as<DebuggerObject>());
    } else {
        /* Create a new Debugger.Object for obj. */
        RootedNativeObject debugger(cx, object);
        RootedObject proto(cx, &object->getReservedSlot(JSSLOT_DEBUG_OBJECT_PROTO).toObject());
        RootedDebuggerObject dobj(cx, DebuggerObject::create(cx, proto, obj, debugger));
        if (!dobj)
            return false;

        if (!p.add(cx, objects, obj, dobj)) {
            NukeDebuggerWrapper(dobj);
            return false;
        }

        if (obj->compartment() != object->compartment()) {
            CrossCompartmentKey key(object, obj, CrossCompartmentKey::DebuggerObject);
            if (!object->compartment()->putWrapper(cx, key, ObjectValue(*dobj))) {
                NukeDebuggerWrapper(dobj);
                objects.remove(obj);
                ReportOutOfMemory(cx);
                return false;
            }
        }

        result.set(dobj);
    }

    return true;
}

static NativeObject*
ToNativeDebuggerObject(JSContext* cx, MutableHandleObject obj)
{
    if (obj->getClass() != &DebuggerObject::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "Debugger", "Debugger.Object", obj->getClass()->name);
        return nullptr;
    }

    NativeObject* ndobj = &obj->as<NativeObject>();

    Value owner = ndobj->getReservedSlot(JSSLOT_DEBUGOBJECT_OWNER);
    if (owner.isUndefined()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_PROTO, "Debugger.Object", "Debugger.Object");
        return nullptr;
    }

    return ndobj;
}

bool
Debugger::unwrapDebuggeeObject(JSContext* cx, MutableHandleObject obj)
{
    NativeObject* ndobj = ToNativeDebuggerObject(cx, obj);
    if (!ndobj)
        return false;

    Value owner = ndobj->getReservedSlot(JSSLOT_DEBUGOBJECT_OWNER);
    if (&owner.toObject() != object) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_WRONG_OWNER, "Debugger.Object");
        return false;
    }

    obj.set(static_cast<JSObject*>(ndobj->getPrivate()));
    return true;
}

bool
Debugger::unwrapDebuggeeValue(JSContext* cx, MutableHandleValue vp)
{
    assertSameCompartment(cx, object.get(), vp);
    if (vp.isObject()) {
        RootedObject dobj(cx, &vp.toObject());
        if (!unwrapDebuggeeObject(cx, &dobj))
            return false;
        vp.setObject(*dobj);
    }
    return true;
}

static bool
CheckArgCompartment(JSContext* cx, JSObject* obj, JSObject* arg,
                    const char* methodname, const char* propname)
{
    if (arg->compartment() != obj->compartment()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_COMPARTMENT_MISMATCH,
                                  methodname, propname);
        return false;
    }
    return true;
}

static bool
CheckArgCompartment(JSContext* cx, JSObject* obj, HandleValue v,
                    const char* methodname, const char* propname)
{
    if (v.isObject())
        return CheckArgCompartment(cx, obj, &v.toObject(), methodname, propname);
    return true;
}

bool
Debugger::unwrapPropertyDescriptor(JSContext* cx, HandleObject obj,
                                   MutableHandle<PropertyDescriptor> desc)
{
    if (desc.hasValue()) {
        RootedValue value(cx, desc.value());
        if (!unwrapDebuggeeValue(cx, &value) ||
            !CheckArgCompartment(cx, obj, value, "defineProperty", "value"))
        {
            return false;
        }
        desc.setValue(value);
    }

    if (desc.hasGetterObject()) {
        RootedObject get(cx, desc.getterObject());
        if (get) {
            if (!unwrapDebuggeeObject(cx, &get))
                return false;
            if (!CheckArgCompartment(cx, obj, get, "defineProperty", "get"))
                return false;
        }
        desc.setGetterObject(get);
    }

    if (desc.hasSetterObject()) {
        RootedObject set(cx, desc.setterObject());
        if (set) {
            if (!unwrapDebuggeeObject(cx, &set))
                return false;
            if (!CheckArgCompartment(cx, obj, set, "defineProperty", "set"))
                return false;
        }
        desc.setSetterObject(set);
    }

    return true;
}

JSTrapStatus
Debugger::reportUncaughtException(Maybe<AutoCompartment>& ac)
{
    JSContext* cx = ac->context();

    // Uncaught exceptions arise from Debugger code, and so we must already be
    // in an NX section.
    MOZ_ASSERT(EnterDebuggeeNoExecute::isLockedInStack(cx, *this));

    if (cx->isExceptionPending()) {
        /*
         * We want to report the pending exception, but we want to let the
         * embedding handle it however it wants to.  So pretend like we're
         * starting a new script execution on our current compartment (which
         * is the debugger compartment, so reported errors won't get
         * reported to various onerror handlers in debuggees) and as part of
         * that "execution" simply throw our exception so the embedding can
         * deal.
         */
        RootedValue exn(cx);
        if (cx->getPendingException(&exn)) {
            /*
             * Clear the exception, because ReportErrorToGlobal will assert that
             * we don't have one.
             */
            cx->clearPendingException();
            ReportErrorToGlobal(cx, cx->global(), exn);
        }
        /*
         * And if not, or if PrepareScriptEnvironmentAndInvoke somehow left
         * an exception on cx (which it totally shouldn't do), just give
         * up.
         */
        cx->clearPendingException();
    }

    ac.reset();
    return JSTRAP_ERROR;
}

JSTrapStatus
Debugger::handleUncaughtExceptionHelper(Maybe<AutoCompartment>& ac, MutableHandleValue* vp,
                                        const Maybe<HandleValue>& thisVForCheck,
                                        AbstractFramePtr frame)
{
    JSContext* cx = ac->context();

    // Uncaught exceptions arise from Debugger code, and so we must already be
    // in an NX section.
    MOZ_ASSERT(EnterDebuggeeNoExecute::isLockedInStack(cx, *this));

    if (cx->isExceptionPending()) {
        if (uncaughtExceptionHook) {
            RootedValue exc(cx);
            if (!cx->getPendingException(&exc))
                return JSTRAP_ERROR;
            cx->clearPendingException();

            RootedValue fval(cx, ObjectValue(*uncaughtExceptionHook));
            RootedValue rv(cx);
            if (js::Call(cx, fval, object, exc, &rv)) {
                if (vp) {
                    JSTrapStatus status = JSTRAP_CONTINUE;
                    if (processResumptionValue(ac, frame, thisVForCheck, rv, status, *vp))
                        return status;
                } else {
                    return JSTRAP_CONTINUE;
                }
            }
        }

        return reportUncaughtException(ac);
    }

    ac.reset();
    return JSTRAP_ERROR;
}

JSTrapStatus
Debugger::handleUncaughtException(Maybe<AutoCompartment>& ac, MutableHandleValue vp,
                                  const Maybe<HandleValue>& thisVForCheck, AbstractFramePtr frame)
{
    return handleUncaughtExceptionHelper(ac, &vp, thisVForCheck, frame);
}

JSTrapStatus
Debugger::handleUncaughtException(Maybe<AutoCompartment>& ac)
{
    return handleUncaughtExceptionHelper(ac, nullptr, mozilla::Nothing(), NullFramePtr());
}

/* static */ void
Debugger::resultToCompletion(JSContext* cx, bool ok, const Value& rv,
                             JSTrapStatus* status, MutableHandleValue value)
{
    MOZ_ASSERT_IF(ok, !cx->isExceptionPending());

    if (ok) {
        *status = JSTRAP_RETURN;
        value.set(rv);
    } else if (cx->isExceptionPending()) {
        *status = JSTRAP_THROW;
        if (!cx->getPendingException(value))
            *status = JSTRAP_ERROR;
        cx->clearPendingException();
    } else {
        *status = JSTRAP_ERROR;
        value.setUndefined();
    }
}

bool
Debugger::newCompletionValue(JSContext* cx, JSTrapStatus status, const Value& value_,
                             MutableHandleValue result)
{
    /*
     * We must be in the debugger's compartment, since that's where we want
     * to construct the completion value.
     */
    assertSameCompartment(cx, object.get());
    assertSameCompartment(cx, value_);

    RootedId key(cx);
    RootedValue value(cx, value_);

    switch (status) {
      case JSTRAP_RETURN:
        key = NameToId(cx->names().return_);
        break;

      case JSTRAP_THROW:
        key = NameToId(cx->names().throw_);
        break;

      case JSTRAP_ERROR:
        result.setNull();
        return true;

      default:
        MOZ_CRASH("bad status passed to Debugger::newCompletionValue");
    }

    /* Common tail for JSTRAP_RETURN and JSTRAP_THROW. */
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj ||
        !NativeDefineDataProperty(cx, obj, key, value, JSPROP_ENUMERATE))
    {
        return false;
    }

    result.setObject(*obj);
    return true;
}

bool
Debugger::receiveCompletionValue(Maybe<AutoCompartment>& ac, bool ok,
                                 HandleValue val,
                                 MutableHandleValue vp)
{
    JSContext* cx = ac->context();

    JSTrapStatus status;
    RootedValue value(cx);
    resultToCompletion(cx, ok, val, &status, &value);
    ac.reset();
    return wrapDebuggeeValue(cx, &value) &&
           newCompletionValue(cx, status, value, vp);
}

static bool
GetStatusProperty(JSContext* cx, HandleObject obj, HandlePropertyName name, JSTrapStatus status,
                  JSTrapStatus& statusp, MutableHandleValue vp, int* hits)
{
    bool found;
    if (!HasProperty(cx, obj, name, &found))
        return false;
    if (found) {
        ++*hits;
        statusp = status;
        if (!GetProperty(cx, obj, obj, name, vp))
            return false;
    }
    return true;
}

static bool
ParseResumptionValueAsObject(JSContext* cx, HandleValue rv, JSTrapStatus& statusp,
                             MutableHandleValue vp)
{
    int hits = 0;
    if (rv.isObject()) {
        RootedObject obj(cx, &rv.toObject());
        if (!GetStatusProperty(cx, obj, cx->names().return_, JSTRAP_RETURN, statusp, vp, &hits))
            return false;
        if (!GetStatusProperty(cx, obj, cx->names().throw_, JSTRAP_THROW, statusp, vp, &hits))
            return false;
    }

    if (hits != 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_RESUMPTION);
        return false;
    }
    return true;
}

static bool
ParseResumptionValue(JSContext* cx, HandleValue rval, JSTrapStatus& statusp, MutableHandleValue vp)
{
    if (rval.isUndefined()) {
        statusp = JSTRAP_CONTINUE;
        vp.setUndefined();
        return true;
    }
    if (rval.isNull()) {
        statusp = JSTRAP_ERROR;
        vp.setUndefined();
        return true;
    }
    return ParseResumptionValueAsObject(cx, rval, statusp, vp);
}

static bool
CheckResumptionValue(JSContext* cx, AbstractFramePtr frame, const Maybe<HandleValue>& maybeThisv,
                     JSTrapStatus status, MutableHandleValue vp)
{
    if (status == JSTRAP_RETURN && frame && frame.isFunctionFrame()) {
        // Don't let a { return: ... } resumption value make a generator
        // function violate the iterator protocol. The return value from
        // such a frame must have the form { done: <bool>, value: <anything> }.
        RootedFunction callee(cx, frame.callee());
        if (callee->isGenerator()) {
            if (!CheckGeneratorResumptionValue(cx, vp)) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_YIELD);
                return false;
            }
        }
    }

    if (maybeThisv.isSome()) {
        const HandleValue& thisv = maybeThisv.ref();
        if (status == JSTRAP_RETURN && vp.isPrimitive()) {
            if (vp.isUndefined()) {
                if (thisv.isMagic(JS_UNINITIALIZED_LEXICAL))
                    return ThrowUninitializedThis(cx, frame);

                vp.set(thisv);
            } else {
                ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, vp, nullptr);
                return false;
            }
        }
    }
    return true;
}

static bool
GetThisValueForCheck(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc,
                     MutableHandleValue thisv, Maybe<HandleValue>& maybeThisv)
{
    if (frame.debuggerNeedsCheckPrimitiveReturn()) {
        {
            AutoCompartment ac(cx, frame.environmentChain());
            if (!GetThisValueForDebuggerMaybeOptimizedOut(cx, frame, pc, thisv))
                return false;
        }

        if (!cx->compartment()->wrap(cx, thisv))
            return false;

        MOZ_ASSERT_IF(thisv.isMagic(), thisv.isMagic(JS_UNINITIALIZED_LEXICAL));
        maybeThisv.emplace(HandleValue(thisv));
    }

    return true;
}

bool
Debugger::processResumptionValue(Maybe<AutoCompartment>& ac, AbstractFramePtr frame,
                                 const Maybe<HandleValue>& maybeThisv, HandleValue rval,
                                 JSTrapStatus& statusp, MutableHandleValue vp)
{
    JSContext* cx = ac->context();

    if (!ParseResumptionValue(cx, rval, statusp, vp) ||
        !unwrapDebuggeeValue(cx, vp) ||
        !CheckResumptionValue(cx, frame, maybeThisv, statusp, vp))
    {
        return false;
    }

    ac.reset();
    if (!cx->compartment()->wrap(cx, vp)) {
        statusp = JSTRAP_ERROR;
        vp.setUndefined();
    }

    return true;
}

JSTrapStatus
Debugger::processParsedHandlerResultHelper(Maybe<AutoCompartment>& ac, AbstractFramePtr frame,
                                           const Maybe<HandleValue>& maybeThisv, bool success,
                                           JSTrapStatus status, MutableHandleValue vp)
{
    if (!success)
        return handleUncaughtException(ac, vp, maybeThisv, frame);

    JSContext* cx = ac->context();

    if (!unwrapDebuggeeValue(cx, vp) ||
        !CheckResumptionValue(cx, frame, maybeThisv, status, vp))
    {
        return handleUncaughtException(ac, vp, maybeThisv, frame);
    }

    ac.reset();
    if (!cx->compartment()->wrap(cx, vp)) {
        status = JSTRAP_ERROR;
        vp.setUndefined();
    }

    return status;
}

JSTrapStatus
Debugger::processParsedHandlerResult(Maybe<AutoCompartment>& ac, AbstractFramePtr frame,
                                     jsbytecode* pc, bool success, JSTrapStatus status,
                                     MutableHandleValue vp)
{
    JSContext* cx = ac->context();

    RootedValue thisv(cx);
    Maybe<HandleValue> maybeThisv;
    if (!GetThisValueForCheck(cx, frame, pc, &thisv, maybeThisv)) {
        ac.reset();
        return JSTRAP_ERROR;
    }

    return processParsedHandlerResultHelper(ac, frame, maybeThisv, success, status, vp);
}

JSTrapStatus
Debugger::processHandlerResult(Maybe<AutoCompartment>& ac, bool success, const Value& rv,
                               AbstractFramePtr frame, jsbytecode* pc, MutableHandleValue vp)
{
    JSContext* cx = ac->context();

    RootedValue thisv(cx);
    Maybe<HandleValue> maybeThisv;
    if (!GetThisValueForCheck(cx, frame, pc, &thisv, maybeThisv)) {
        ac.reset();
        return JSTRAP_ERROR;
    }

    if (!success)
        return handleUncaughtException(ac, vp, maybeThisv, frame);

    RootedValue rootRv(cx, rv);
    JSTrapStatus status = JSTRAP_CONTINUE;
    success = ParseResumptionValue(cx, rootRv, status, vp);

    return processParsedHandlerResultHelper(ac, frame, maybeThisv, success, status, vp);
}

static bool
CallMethodIfPresent(JSContext* cx, HandleObject obj, const char* name, size_t argc, Value* argv,
                    MutableHandleValue rval)
{
    rval.setUndefined();
    JSAtom* atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return false;

    RootedId id(cx, AtomToId(atom));
    RootedValue fval(cx);
    if (!GetProperty(cx, obj, obj, id, &fval))
        return false;

    if (!IsCallable(fval))
        return true;

    InvokeArgs args(cx);
    if (!args.init(cx, argc))
        return false;

    for (size_t i = 0; i < argc; i++)
        args[i].set(argv[i]);

    rval.setObject(*obj); // overwritten by successful Call
    return js::Call(cx, fval, rval, args, rval);
}

JSTrapStatus
Debugger::fireDebuggerStatement(JSContext* cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnDebuggerStatement));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    ScriptFrameIter iter(cx);
    RootedValue scriptFrame(cx);
    if (!getScriptFrame(cx, iter, &scriptFrame))
        return reportUncaughtException(ac);

    RootedValue fval(cx, ObjectValue(*hook));
    RootedValue rv(cx);
    bool ok = js::Call(cx, fval, object, scriptFrame, &rv);
    return processHandlerResult(ac, ok, rv, iter.abstractFramePtr(), iter.pc(), vp);
}

JSTrapStatus
Debugger::fireExceptionUnwind(JSContext* cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnExceptionUnwind));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    RootedValue exc(cx);
    if (!cx->getPendingException(&exc))
        return JSTRAP_ERROR;
    cx->clearPendingException();

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    RootedValue scriptFrame(cx);
    RootedValue wrappedExc(cx, exc);

    FrameIter iter(cx);
    if (!getScriptFrame(cx, iter, &scriptFrame) || !wrapDebuggeeValue(cx, &wrappedExc))
        return reportUncaughtException(ac);

    RootedValue fval(cx, ObjectValue(*hook));
    RootedValue rv(cx);
    bool ok = js::Call(cx, fval, object, scriptFrame, wrappedExc, &rv);
    JSTrapStatus st = processHandlerResult(ac, ok, rv, iter.abstractFramePtr(), iter.pc(), vp);
    if (st == JSTRAP_CONTINUE)
        cx->setPendingException(exc);
    return st;
}

JSTrapStatus
Debugger::fireEnterFrame(JSContext* cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnEnterFrame));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    RootedValue scriptFrame(cx);

    FrameIter iter(cx);
    if (!getScriptFrame(cx, iter, &scriptFrame))
        return reportUncaughtException(ac);

    RootedValue fval(cx, ObjectValue(*hook));
    RootedValue rv(cx);
    bool ok = js::Call(cx, fval, object, scriptFrame, &rv);

    return processHandlerResult(ac, ok, rv, iter.abstractFramePtr(), iter.pc(), vp);
}

void
Debugger::fireNewScript(JSContext* cx, Handle<DebuggerScriptReferent> scriptReferent)
{
    RootedObject hook(cx, getHook(OnNewScript));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    JSObject* dsobj = wrapVariantReferent(cx, scriptReferent);
    if (!dsobj) {
        reportUncaughtException(ac);
        return;
    }

    RootedValue fval(cx, ObjectValue(*hook));
    RootedValue dsval(cx, ObjectValue(*dsobj));
    RootedValue rv(cx);
    if (!js::Call(cx, fval, object, dsval, &rv))
        handleUncaughtException(ac);
}

void
Debugger::fireOnGarbageCollectionHook(JSContext* cx,
                                      const JS::dbg::GarbageCollectionEvent::Ptr& gcData)
{
    MOZ_ASSERT(observedGC(gcData->majorGCNumber()));
    observedGCs.remove(gcData->majorGCNumber());

    RootedObject hook(cx, getHook(OnGarbageCollection));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    JSObject* dataObj = gcData->toJSObject(cx);
    if (!dataObj) {
        reportUncaughtException(ac);
        return;
    }

    RootedValue fval(cx, ObjectValue(*hook));
    RootedValue dataVal(cx, ObjectValue(*dataObj));
    RootedValue rv(cx);
    if (!js::Call(cx, fval, object, dataVal, &rv))
        handleUncaughtException(ac);
}

template <typename HookIsEnabledFun /* bool (Debugger*) */,
          typename FireHookFun /* JSTrapStatus (Debugger*) */>
/* static */ JSTrapStatus
Debugger::dispatchHook(JSContext* cx, HookIsEnabledFun hookIsEnabled, FireHookFun fireHook)
{
    /*
     * Determine which debuggers will receive this event, and in what order.
     * Make a copy of the list, since the original is mutable and we will be
     * calling into arbitrary JS.
     *
     * Note: In the general case, 'triggered' contains references to objects in
     * different compartments--every compartment *except* this one.
     */
    AutoValueVector triggered(cx);
    Handle<GlobalObject*> global = cx->global();
    if (GlobalObject::DebuggerVector* debuggers = global->getDebuggers()) {
        for (auto p = debuggers->begin(); p != debuggers->end(); p++) {
            Debugger* dbg = *p;
            if (dbg->enabled && hookIsEnabled(dbg)) {
                if (!triggered.append(ObjectValue(*dbg->toJSObject())))
                    return JSTRAP_ERROR;
            }
        }
    }

    /*
     * Deliver the event to each debugger, checking again to make sure it
     * should still be delivered.
     */
    for (Value* p = triggered.begin(); p != triggered.end(); p++) {
        Debugger* dbg = Debugger::fromJSObject(&p->toObject());
        EnterDebuggeeNoExecute nx(cx, *dbg);
        if (dbg->debuggees.has(global) && dbg->enabled && hookIsEnabled(dbg)) {
            JSTrapStatus st = fireHook(dbg);
            if (st != JSTRAP_CONTINUE)
                return st;
        }
    }
    return JSTRAP_CONTINUE;
}

void
Debugger::slowPathOnNewScript(JSContext* cx, HandleScript script)
{
    JSTrapStatus status = dispatchHook(
        cx,
        [script](Debugger* dbg) -> bool {
            return dbg->observesNewScript() && dbg->observesScript(script);
        },
        [&](Debugger* dbg) -> JSTrapStatus {
            Rooted<DebuggerScriptReferent> scriptReferent(cx, script.get());
            dbg->fireNewScript(cx, scriptReferent);
            return JSTRAP_CONTINUE;
        });

    // dispatchHook may fail due to OOM. This OOM is not handlable at the
    // callsites of onNewScript in the engine.
    if (status == JSTRAP_ERROR) {
        cx->clearPendingException();
        return;
    }

    MOZ_ASSERT(status == JSTRAP_CONTINUE);
}

void
Debugger::slowPathOnNewWasmInstance(JSContext* cx, Handle<WasmInstanceObject*> wasmInstance)
{
    JSTrapStatus status = dispatchHook(
        cx,
        [wasmInstance](Debugger* dbg) -> bool {
            return dbg->observesNewScript() && dbg->observesGlobal(&wasmInstance->global());
        },
        [&](Debugger* dbg) -> JSTrapStatus {
            Rooted<DebuggerScriptReferent> scriptReferent(cx, wasmInstance.get());
            dbg->fireNewScript(cx, scriptReferent);
            return JSTRAP_CONTINUE;
        });

    // dispatchHook may fail due to OOM. This OOM is not handlable at the
    // callsites of onNewWasmInstance in the engine.
    if (status == JSTRAP_ERROR) {
        cx->clearPendingException();
        return;
    }

    MOZ_ASSERT(status == JSTRAP_CONTINUE);
}

/* static */ JSTrapStatus
Debugger::onTrap(JSContext* cx, MutableHandleValue vp)
{
    FrameIter iter(cx);
    JS::AutoSaveExceptionState saveExc(cx);
    Rooted<GlobalObject*> global(cx);
    BreakpointSite* site;
    bool isJS; // true when iter.hasScript(), false when iter.isWasm()
    jsbytecode* pc; // valid when isJS == true
    uint32_t bytecodeOffset; // valid when isJS == false
    if (iter.hasScript()) {
        RootedScript script(cx, iter.script());
        MOZ_ASSERT(script->isDebuggee());
        global.set(&script->global());
        isJS = true;
        pc = iter.pc();
        bytecodeOffset = 0;
        site = script->getBreakpointSite(pc);
    } else {
        MOZ_ASSERT(iter.isWasm());
        global.set(&iter.wasmInstance()->object()->global());
        isJS = false;
        pc = nullptr;
        bytecodeOffset = iter.wasmBytecodeOffset();
        site = iter.wasmInstance()->debug().getOrCreateBreakpointSite(cx, bytecodeOffset);
    }

    /* Build list of breakpoint handlers. */
    Vector<Breakpoint*> triggered(cx);
    for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = bp->nextInSite()) {
        // Skip a breakpoint that is not set for the current wasm::Instance --
        // single wasm::Code can handle breakpoints for mutiple instances.
        if (!isJS && &bp->asWasm()->wasmInstance->instance() != iter.wasmInstance())
            continue;
        if (!triggered.append(bp))
            return JSTRAP_ERROR;
    }

    for (Breakpoint** p = triggered.begin(); p != triggered.end(); p++) {
        Breakpoint* bp = *p;

        /* Handlers can clear breakpoints. Check that bp still exists. */
        if (!site || !site->hasBreakpoint(bp))
            continue;

        /*
         * There are two reasons we have to check whether dbg is enabled and
         * debugging global.
         *
         * One is just that one breakpoint handler can disable other Debuggers
         * or remove debuggees.
         *
         * The other has to do with non-compile-and-go scripts, which have no
         * specific global--until they are executed. Only now do we know which
         * global the script is running against.
         */
        Debugger* dbg = bp->debugger;
        bool hasDebuggee = dbg->enabled && dbg->debuggees.has(global);
        if (hasDebuggee) {
            Maybe<AutoCompartment> ac;
            ac.emplace(cx, dbg->object);
            EnterDebuggeeNoExecute nx(cx, *dbg);

            RootedValue scriptFrame(cx);
            if (!dbg->getScriptFrame(cx, iter, &scriptFrame))
                return dbg->reportUncaughtException(ac);
            RootedValue rv(cx);
            Rooted<JSObject*> handler(cx, bp->handler);
            bool ok = CallMethodIfPresent(cx, handler, "hit", 1, scriptFrame.address(), &rv);
            JSTrapStatus st = dbg->processHandlerResult(ac, ok, rv,  iter.abstractFramePtr(),
                                                        iter.pc(), vp);
            if (st != JSTRAP_CONTINUE)
                return st;

            /* Calling JS code invalidates site. Reload it. */
            if (isJS)
                site = iter.script()->getBreakpointSite(pc);
            else
                site = iter.wasmInstance()->debug().getOrCreateBreakpointSite(cx, bytecodeOffset);
        }
    }

    // By convention, return the true op to the interpreter in vp, and return
    // undefined in vp to the wasm debug trap.
    if (isJS)
        vp.setInt32(JSOp(*pc));
    else
        vp.set(UndefinedValue());
    return JSTRAP_CONTINUE;
}

/* static */ JSTrapStatus
Debugger::onSingleStep(JSContext* cx, MutableHandleValue vp)
{
    FrameIter iter(cx);

    /*
     * We may be stepping over a JSOP_EXCEPTION, that pushes the context's
     * pending exception for a 'catch' clause to handle. Don't let the
     * onStep handlers mess with that (other than by returning a resumption
     * value).
     */
    JS::AutoSaveExceptionState saveExc(cx);

    /*
     * Build list of Debugger.Frame instances referring to this frame with
     * onStep handlers.
     */
    Rooted<DebuggerFrameVector> frames(cx, DebuggerFrameVector(cx));
    if (!getDebuggerFrames(iter.abstractFramePtr(), &frames))
        return JSTRAP_ERROR;

#ifdef DEBUG
    /*
     * Validate the single-step count on this frame's script, to ensure that
     * we're not receiving traps we didn't ask for. Even when frames is
     * non-empty (and thus we know this trap was requested), do the check
     * anyway, to make sure the count has the correct non-zero value.
     *
     * The converse --- ensuring that we do receive traps when we should --- can
     * be done with unit tests.
     */
    if (iter.hasScript()) {
        uint32_t stepperCount = 0;
        JSScript* trappingScript = iter.script();
        GlobalObject* global = cx->global();
        if (GlobalObject::DebuggerVector* debuggers = global->getDebuggers()) {
            for (auto p = debuggers->begin(); p != debuggers->end(); p++) {
                Debugger* dbg = *p;
                for (FrameMap::Range r = dbg->frames.all(); !r.empty(); r.popFront()) {
                    AbstractFramePtr frame = r.front().key();
                    NativeObject* frameobj = r.front().value();
                    if (frame.isWasmDebugFrame())
                        continue;
                    if (frame.script() == trappingScript &&
                        !frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined())
                    {
                        stepperCount++;
                    }
                }
            }
        }
        MOZ_ASSERT(stepperCount == trappingScript->stepModeCount());
    }
#endif

    // Call onStep for frames that have the handler set.
    for (size_t i = 0; i < frames.length(); i++) {
        HandleDebuggerFrame frame = frames[i];
        OnStepHandler* handler = frame->onStepHandler();
        if (!handler)
            continue;

        Debugger* dbg = Debugger::fromChildJSObject(frame);
        EnterDebuggeeNoExecute nx(cx, *dbg);

        Maybe<AutoCompartment> ac;
        ac.emplace(cx, dbg->object);

        JSTrapStatus status = JSTRAP_CONTINUE;
        bool success = handler->onStep(cx, frame, status, vp);
        status = dbg->processParsedHandlerResult(ac, iter.abstractFramePtr(), iter.pc(), success,
                                                 status, vp);
        if (status != JSTRAP_CONTINUE)
            return status;
    }

    vp.setUndefined();
    return JSTRAP_CONTINUE;
}

JSTrapStatus
Debugger::fireNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnNewGlobalObject));
    MOZ_ASSERT(hook);
    MOZ_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    RootedValue wrappedGlobal(cx, ObjectValue(*global));
    if (!wrapDebuggeeValue(cx, &wrappedGlobal))
        return reportUncaughtException(ac);

    // onNewGlobalObject is infallible, and thus is only allowed to return
    // undefined as a resumption value. If it returns anything else, we throw.
    // And if that happens, or if the hook itself throws, we invoke the
    // uncaughtExceptionHook so that we never leave an exception pending on the
    // cx. This allows JS_NewGlobalObject to avoid handling failures from debugger
    // hooks.
    RootedValue rv(cx);
    RootedValue fval(cx, ObjectValue(*hook));
    bool ok = js::Call(cx, fval, object, wrappedGlobal, &rv);
    if (ok && !rv.isUndefined()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_RESUMPTION_VALUE_DISALLOWED);
        ok = false;
    }
    // NB: Even though we don't care about what goes into it, we have to pass vp
    // to handleUncaughtException so that it parses resumption values from the
    // uncaughtExceptionHook and tells the caller whether we should execute the
    // rest of the onNewGlobalObject hooks or not.
    JSTrapStatus status = ok ? JSTRAP_CONTINUE
                             : handleUncaughtException(ac, vp);
    MOZ_ASSERT(!cx->isExceptionPending());
    return status;
}

void
Debugger::slowPathOnNewGlobalObject(JSContext* cx, Handle<GlobalObject*> global)
{
    MOZ_ASSERT(!cx->runtime()->onNewGlobalObjectWatchers().isEmpty());
    if (global->compartment()->creationOptions().invisibleToDebugger())
        return;

    /*
     * Make a copy of the runtime's onNewGlobalObjectWatchers before running the
     * handlers. Since one Debugger's handler can disable another's, the list
     * can be mutated while we're walking it.
     */
    AutoObjectVector watchers(cx);
    for (auto& dbg : cx->runtime()->onNewGlobalObjectWatchers()) {
        MOZ_ASSERT(dbg.observesNewGlobalObject());
        JSObject* obj = dbg.object;
        JS::ExposeObjectToActiveJS(obj);
        if (!watchers.append(obj)) {
            if (cx->isExceptionPending())
                cx->clearPendingException();
            return;
        }
    }

    JSTrapStatus status = JSTRAP_CONTINUE;
    RootedValue value(cx);

    for (size_t i = 0; i < watchers.length(); i++) {
        Debugger* dbg = fromJSObject(watchers[i]);
        EnterDebuggeeNoExecute nx(cx, *dbg);

        // We disallow resumption values from onNewGlobalObject hooks, because we
        // want the debugger hooks for global object creation to be infallible.
        // But if an onNewGlobalObject hook throws, and the uncaughtExceptionHook
        // decides to raise an error, we want to at least avoid invoking the rest
        // of the onNewGlobalObject handlers in the list (not for any super
        // compelling reason, just because it seems like the right thing to do).
        // So we ignore whatever comes out in |value|, but break out of the loop
        // if a non-success trap status is returned.
        if (dbg->observesNewGlobalObject()) {
            status = dbg->fireNewGlobalObject(cx, global, &value);
            if (status != JSTRAP_CONTINUE && status != JSTRAP_RETURN)
                break;
        }
    }
    MOZ_ASSERT(!cx->isExceptionPending());
}

/* static */ bool
Debugger::slowPathOnLogAllocationSite(JSContext* cx, HandleObject obj, HandleSavedFrame frame,
                                      mozilla::TimeStamp when, GlobalObject::DebuggerVector& dbgs)
{
    MOZ_ASSERT(!dbgs.empty());
    mozilla::DebugOnly<ReadBarriered<Debugger*>*> begin = dbgs.begin();

    // Root all the Debuggers while we're iterating over them;
    // appendAllocationSite calls JSCompartment::wrap, and thus can GC.
    //
    // SpiderMonkey protocol is generally for the caller to prove that it has
    // rooted the stuff it's asking you to operate on (i.e. by passing a
    // Handle), but in this case, we're iterating over a global's list of
    // Debuggers, and globals only hold their Debuggers weakly.
    Rooted<GCVector<JSObject*>> activeDebuggers(cx, GCVector<JSObject*>(cx));
    for (auto dbgp = dbgs.begin(); dbgp < dbgs.end(); dbgp++) {
        if (!activeDebuggers.append((*dbgp)->object))
            return false;
    }

    for (auto dbgp = dbgs.begin(); dbgp < dbgs.end(); dbgp++) {
        // The set of debuggers had better not change while we're iterating,
        // such that the vector gets reallocated.
        MOZ_ASSERT(dbgs.begin() == begin);

        if ((*dbgp)->trackingAllocationSites &&
            (*dbgp)->enabled &&
            !(*dbgp)->appendAllocationSite(cx, obj, frame, when))
        {
            return false;
        }
    }

    return true;
}

bool
Debugger::isDebuggeeUnbarriered(const JSCompartment* compartment) const
{
    MOZ_ASSERT(compartment);
    return compartment->isDebuggee() && debuggees.has(compartment->unsafeUnbarrieredMaybeGlobal());
}

bool
Debugger::appendAllocationSite(JSContext* cx, HandleObject obj, HandleSavedFrame frame,
                               mozilla::TimeStamp when)
{
    MOZ_ASSERT(trackingAllocationSites && enabled);

    AutoCompartment ac(cx, object);
    RootedObject wrappedFrame(cx, frame);
    if (!cx->compartment()->wrap(cx, &wrappedFrame))
        return false;

    RootedAtom ctorName(cx);
    {
        AutoCompartment ac(cx, obj);
        if (!JSObject::constructorDisplayAtom(cx, obj, &ctorName))
            return false;
    }
    if (ctorName)
        cx->markAtom(ctorName);

    auto className = obj->getClass()->name;
    auto size = JS::ubi::Node(obj.get()).size(cx->runtime()->debuggerMallocSizeOf);
    auto inNursery = gc::IsInsideNursery(obj);

    if (!allocationsLog.emplaceBack(wrappedFrame, when, className, ctorName, size, inNursery)) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (allocationsLog.length() > maxAllocationsLogLength) {
        allocationsLog.popFront();
        MOZ_ASSERT(allocationsLog.length() == maxAllocationsLogLength);
        allocationsLogOverflowed = true;
    }

    return true;
}

JSTrapStatus
Debugger::firePromiseHook(JSContext* cx, Hook hook, HandleObject promise, MutableHandleValue vp)
{
    MOZ_ASSERT(hook == OnNewPromise || hook == OnPromiseSettled);

    RootedObject hookObj(cx, getHook(hook));
    MOZ_ASSERT(hookObj);
    MOZ_ASSERT(hookObj->isCallable());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, object);

    RootedValue dbgObj(cx, ObjectValue(*promise));
    if (!wrapDebuggeeValue(cx, &dbgObj))
        return reportUncaughtException(ac);

    // Like onNewGlobalObject, the Promise hooks are infallible and the comments
    // in |Debugger::fireNewGlobalObject| apply here as well.
    RootedValue fval(cx, ObjectValue(*hookObj));
    RootedValue rv(cx);
    bool ok = js::Call(cx, fval, object, dbgObj, &rv);
    if (ok && !rv.isUndefined()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_RESUMPTION_VALUE_DISALLOWED);
        ok = false;
    }

    JSTrapStatus status = ok ? JSTRAP_CONTINUE
                             : handleUncaughtException(ac, vp);
    MOZ_ASSERT(!cx->isExceptionPending());
    return status;
}

/* static */ void
Debugger::slowPathPromiseHook(JSContext* cx, Hook hook, Handle<PromiseObject*> promise)
{
    MOZ_ASSERT(hook == OnNewPromise || hook == OnPromiseSettled);

    Maybe<AutoCompartment> ac;
    if (hook == OnNewPromise)
        ac.emplace(cx, promise);

    assertSameCompartment(cx, promise);

    RootedValue rval(cx);
    JSTrapStatus status = dispatchHook(
        cx,
        [hook](Debugger* dbg) -> bool { return dbg->getHook(hook); },
        [&](Debugger* dbg) -> JSTrapStatus {
            (void) dbg->firePromiseHook(cx, hook, promise, &rval);
            return JSTRAP_CONTINUE;
        });

    if (status == JSTRAP_ERROR) {
        // The dispatch hook function might fail to append into the list of
        // Debuggers which are watching for the hook.
        cx->clearPendingException();
        return;
    }

    // Promise hooks are infallible and we ignore errors from uncaught
    // exceptions by design.
    MOZ_ASSERT(status == JSTRAP_CONTINUE);
}


/*** Debugger code invalidation for observing execution ******************************************/

class MOZ_RAII ExecutionObservableCompartments : public Debugger::ExecutionObservableSet
{
    HashSet<JSCompartment*> compartments_;
    HashSet<Zone*> zones_;

  public:
    explicit ExecutionObservableCompartments(JSContext* cx
                                             MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : compartments_(cx),
        zones_(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    bool init() { return compartments_.init() && zones_.init(); }
    bool add(JSCompartment* comp) { return compartments_.put(comp) && zones_.put(comp->zone()); }

    typedef HashSet<JSCompartment*>::Range CompartmentRange;
    const HashSet<JSCompartment*>* compartments() const { return &compartments_; }

    const HashSet<Zone*>* zones() const override { return &zones_; }
    bool shouldRecompileOrInvalidate(JSScript* script) const override {
        return script->hasBaselineScript() && compartments_.has(script->compartment());
    }
    bool shouldMarkAsDebuggee(FrameIter& iter) const override {
        // AbstractFramePtr can't refer to non-remateralized Ion frames or
        // non-debuggee wasm frames, so if iter refers to one such, we know we
        // don't match.
        return iter.hasUsableAbstractFramePtr() && compartments_.has(iter.compartment());
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

// Given a particular AbstractFramePtr F that has become observable, this
// represents the stack frames that need to be bailed out or marked as
// debuggees, and the scripts that need to be recompiled, taking inlining into
// account.
class MOZ_RAII ExecutionObservableFrame : public Debugger::ExecutionObservableSet
{
    AbstractFramePtr frame_;

  public:
    explicit ExecutionObservableFrame(AbstractFramePtr frame
                                      MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : frame_(frame)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    Zone* singleZone() const override {
        // We never inline across compartments, let alone across zones, so
        // frames_'s script's zone is the only one of interest.
        return frame_.script()->compartment()->zone();
    }

    JSScript* singleScriptForZoneInvalidation() const override {
        MOZ_CRASH("ExecutionObservableFrame shouldn't need zone-wide invalidation.");
        return nullptr;
    }

    bool shouldRecompileOrInvalidate(JSScript* script) const override {
        // Normally, *this represents exactly one script: the one frame_ is
        // running.
        //
        // However, debug-mode OSR uses *this for both invalidating Ion frames,
        // and recompiling the Baseline scripts that those Ion frames will bail
        // out into. Suppose frame_ is an inline frame, executing a copy of its
        // JSScript, S_inner, that has been inlined into the IonScript of some
        // other JSScript, S_outer. We must match S_outer, to decide which Ion
        // frame to invalidate; and we must match S_inner, to decide which
        // Baseline script to recompile.
        //
        // Note that this does not, by design, invalidate *all* inliners of
        // frame_.script(), as only frame_ is made observable, not
        // frame_.script().
        if (!script->hasBaselineScript())
            return false;

        if (frame_.hasScript() && script == frame_.script())
            return true;

        return frame_.isRematerializedFrame() &&
               script == frame_.asRematerializedFrame()->outerScript();
    }

    bool shouldMarkAsDebuggee(FrameIter& iter) const override {
        // AbstractFramePtr can't refer to non-remateralized Ion frames or
        // non-debuggee wasm frames, so if iter refers to one such, we know we
        // don't match.
        //
        // We never use this 'has' overload for frame invalidation, only for
        // frame debuggee marking; so this overload doesn't need a parallel to
        // the just-so inlining logic above.
        return iter.hasUsableAbstractFramePtr() && iter.abstractFramePtr() == frame_;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class MOZ_RAII ExecutionObservableScript : public Debugger::ExecutionObservableSet
{
    RootedScript script_;

  public:
    ExecutionObservableScript(JSContext* cx, JSScript* script
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : script_(cx, script)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    Zone* singleZone() const override { return script_->compartment()->zone(); }
    JSScript* singleScriptForZoneInvalidation() const override { return script_; }
    bool shouldRecompileOrInvalidate(JSScript* script) const override {
        return script->hasBaselineScript() && script == script_;
    }
    bool shouldMarkAsDebuggee(FrameIter& iter) const override {
        // AbstractFramePtr can't refer to non-remateralized Ion frames, and
        // while a non-rematerialized Ion frame may indeed be running script_,
        // we cannot mark them as debuggees until they bail out.
        //
        // Upon bailing out, any newly constructed Baseline frames that came
        // from Ion frames with scripts that are isDebuggee() is marked as
        // debuggee. This is correct in that the only other way a frame may be
        // marked as debuggee is via Debugger.Frame reflection, which would
        // have rematerialized any Ion frames.
        //
        // Also AbstractFramePtr can't refer to non-debuggee wasm frames, so if
        // iter refers to one such, we know we don't match.
        return iter.hasUsableAbstractFramePtr() && !iter.isWasm() &&
               iter.abstractFramePtr().script() == script_;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/* static */ bool
Debugger::updateExecutionObservabilityOfFrames(JSContext* cx, const ExecutionObservableSet& obs,
                                               IsObserving observing)
{
    AutoSuppressProfilerSampling suppressProfilerSampling(cx);

    {
        jit::JitContext jctx(cx, nullptr);
        if (!jit::RecompileOnStackBaselineScriptsForDebugMode(cx, obs, observing)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    AbstractFramePtr oldestEnabledFrame;
    for (FrameIter iter(cx);
         !iter.done();
         ++iter)
    {
        if (obs.shouldMarkAsDebuggee(iter)) {
            if (observing) {
                if (!iter.abstractFramePtr().isDebuggee()) {
                    oldestEnabledFrame = iter.abstractFramePtr();
                    oldestEnabledFrame.setIsDebuggee();
                }
                if (iter.abstractFramePtr().isWasmDebugFrame())
                    iter.abstractFramePtr().asWasmDebugFrame()->observe(cx);
            } else {
#ifdef DEBUG
                // Debugger.Frame lifetimes are managed by the debug epilogue,
                // so in general it's unsafe to unmark a frame if it has a
                // Debugger.Frame associated with it.
                MOZ_ASSERT(!inFrameMaps(iter.abstractFramePtr()));
#endif
                iter.abstractFramePtr().unsetIsDebuggee();
            }
        }
    }

    // See comment in unsetPrevUpToDateUntil.
    if (oldestEnabledFrame) {
        AutoCompartment ac(cx, oldestEnabledFrame.environmentChain());
        DebugEnvironments::unsetPrevUpToDateUntil(cx, oldestEnabledFrame);
    }

    return true;
}

static inline void
MarkBaselineScriptActiveIfObservable(JSScript* script, const Debugger::ExecutionObservableSet& obs)
{
    if (obs.shouldRecompileOrInvalidate(script))
        script->baselineScript()->setActive();
}

static bool
AppendAndInvalidateScript(JSContext* cx, Zone* zone, JSScript* script, Vector<JSScript*>& scripts)
{
    // Enter the script's compartment as addPendingRecompile attempts to
    // cancel off-thread compilations, whose books are kept on the
    // script's compartment.
    MOZ_ASSERT(script->compartment()->zone() == zone);
    AutoCompartment ac(cx, script);
    zone->types.addPendingRecompile(cx, script);
    return scripts.append(script);
}

static bool
UpdateExecutionObservabilityOfScriptsInZone(JSContext* cx, Zone* zone,
                                            const Debugger::ExecutionObservableSet& obs,
                                            Debugger::IsObserving observing)
{
    using namespace js::jit;

    AutoSuppressProfilerSampling suppressProfilerSampling(cx);

    FreeOp* fop = cx->runtime()->defaultFreeOp();

    Vector<JSScript*> scripts(cx);

    // Iterate through observable scripts, invalidating their Ion scripts and
    // appending them to a vector for discarding their baseline scripts later.
    {
        AutoEnterAnalysis enter(fop, zone);
        if (JSScript* script = obs.singleScriptForZoneInvalidation()) {
            if (obs.shouldRecompileOrInvalidate(script)) {
                if (!AppendAndInvalidateScript(cx, zone, script, scripts))
                    return false;
            }
        } else {
            for (auto iter = zone->cellIter<JSScript>(); !iter.done(); iter.next()) {
                JSScript* script = iter;
                if (obs.shouldRecompileOrInvalidate(script) &&
                    !gc::IsAboutToBeFinalizedUnbarriered(&script))
                {
                    if (!AppendAndInvalidateScript(cx, zone, script, scripts))
                        return false;
                }
            }
        }
    }

    // Code below this point must be infallible to ensure the active bit of
    // BaselineScripts is in a consistent state.
    //
    // Mark active baseline scripts in the observable set so that they don't
    // get discarded. They will be recompiled.
    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (JitActivationIterator actIter(cx, target); !actIter.done(); ++actIter) {
            if (actIter->compartment()->zone() != zone)
                continue;

            for (OnlyJSJitFrameIter iter(actIter); !iter.done(); ++iter) {
                const jit::JSJitFrameIter& frame = iter.frame();
                switch (frame.type()) {
                  case JitFrame_BaselineJS:
                    MarkBaselineScriptActiveIfObservable(frame.script(), obs);
                    break;
                  case JitFrame_IonJS:
                    MarkBaselineScriptActiveIfObservable(frame.script(), obs);
                    for (InlineFrameIterator inlineIter(cx, &frame); inlineIter.more(); ++inlineIter)
                        MarkBaselineScriptActiveIfObservable(inlineIter.script(), obs);
                    break;
                  default:;
                }
            }
        }
    }

    // Iterate through the scripts again and finish discarding
    // BaselineScripts. This must be done as a separate phase as we can only
    // discard the BaselineScript on scripts that have no IonScript.
    for (size_t i = 0; i < scripts.length(); i++) {
        MOZ_ASSERT_IF(scripts[i]->isDebuggee(), observing);
        FinishDiscardBaselineScript(fop, scripts[i]);
    }

    // Iterate through all wasm instances to find ones that need to be updated.
    for (JSCompartment* c : zone->compartments()) {
        for (wasm::Instance* instance : c->wasm.instances()) {
            if (!instance->debugEnabled())
                continue;

            bool enableTrap = observing == Debugger::IsObserving::Observing;
            instance->ensureEnterFrameTrapsState(cx, enableTrap);
        }
    }

    return true;
}

/* static */ bool
Debugger::updateExecutionObservabilityOfScripts(JSContext* cx, const ExecutionObservableSet& obs,
                                                IsObserving observing)
{
    if (Zone* zone = obs.singleZone())
        return UpdateExecutionObservabilityOfScriptsInZone(cx, zone, obs, observing);

    typedef ExecutionObservableSet::ZoneRange ZoneRange;
    for (ZoneRange r = obs.zones()->all(); !r.empty(); r.popFront()) {
        if (!UpdateExecutionObservabilityOfScriptsInZone(cx, r.front(), obs, observing))
            return false;
    }

    return true;
}

template <typename FrameFn>
/* static */ void
Debugger::forEachDebuggerFrame(AbstractFramePtr frame, FrameFn fn)
{
    GlobalObject* global = frame.global();
    if (GlobalObject::DebuggerVector* debuggers = global->getDebuggers()) {
        for (auto p = debuggers->begin(); p != debuggers->end(); p++) {
            Debugger* dbg = *p;
            if (FrameMap::Ptr entry = dbg->frames.lookup(frame))
                fn(entry->value());
        }
    }
}

/* static */ bool
Debugger::getDebuggerFrames(AbstractFramePtr frame, MutableHandle<DebuggerFrameVector> frames)
{
    bool hadOOM = false;
    forEachDebuggerFrame(frame, [&](DebuggerFrame* frameobj) {
        if (!hadOOM && !frames.append(frameobj))
            hadOOM = true;
    });
    return !hadOOM;
}

/* static */ bool
Debugger::updateExecutionObservability(JSContext* cx, ExecutionObservableSet& obs,
                                       IsObserving observing)
{
    if (!obs.singleZone() && obs.zones()->empty())
        return true;

    // Invalidate scripts first so we can set the needsArgsObj flag on scripts
    // before patching frames.
    return updateExecutionObservabilityOfScripts(cx, obs, observing) &&
           updateExecutionObservabilityOfFrames(cx, obs, observing);
}

/* static */ bool
Debugger::ensureExecutionObservabilityOfScript(JSContext* cx, JSScript* script)
{
    if (script->isDebuggee())
        return true;
    ExecutionObservableScript obs(cx, script);
    return updateExecutionObservability(cx, obs, Observing);
}

/* static */ bool
Debugger::ensureExecutionObservabilityOfOsrFrame(JSContext* cx, InterpreterFrame* frame)
{
    MOZ_ASSERT(frame->isDebuggee());
    if (frame->script()->hasBaselineScript() &&
        frame->script()->baselineScript()->hasDebugInstrumentation())
    {
        return true;
    }
    ExecutionObservableFrame obs(frame);
    return updateExecutionObservabilityOfFrames(cx, obs, Observing);
}

/* static */ bool
Debugger::ensureExecutionObservabilityOfFrame(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT_IF(frame.hasScript() && frame.script()->isDebuggee(), frame.isDebuggee());
    MOZ_ASSERT_IF(frame.isWasmDebugFrame(), frame.wasmInstance()->debugEnabled());
    if (frame.isDebuggee())
        return true;
    ExecutionObservableFrame obs(frame);
    return updateExecutionObservabilityOfFrames(cx, obs, Observing);
}

/* static */ bool
Debugger::ensureExecutionObservabilityOfCompartment(JSContext* cx, JSCompartment* comp)
{
    if (comp->debuggerObservesAllExecution())
        return true;
    ExecutionObservableCompartments obs(cx);
    if (!obs.init() || !obs.add(comp))
        return false;
    comp->updateDebuggerObservesAllExecution();
    return updateExecutionObservability(cx, obs, Observing);
}

/* static */ bool
Debugger::hookObservesAllExecution(Hook which)
{
    return which == OnEnterFrame;
}

Debugger::IsObserving
Debugger::observesAllExecution() const
{
    if (enabled && !!getHook(OnEnterFrame))
        return Observing;
    return NotObserving;
}

Debugger::IsObserving
Debugger::observesAsmJS() const
{
    if (enabled && !allowUnobservedAsmJS)
        return Observing;
    return NotObserving;
}

Debugger::IsObserving
Debugger::observesBinarySource() const
{
    if (enabled && allowWasmBinarySource)
        return Observing;
    return NotObserving;
}

Debugger::IsObserving
Debugger::observesCoverage() const
{
    if (enabled && collectCoverageInfo)
        return Observing;
    return NotObserving;
}

// Toggle whether this Debugger's debuggees observe all execution. This is
// called when a hook that observes all execution is set or unset. See
// hookObservesAllExecution.
bool
Debugger::updateObservesAllExecutionOnDebuggees(JSContext* cx, IsObserving observing)
{
    ExecutionObservableCompartments obs(cx);
    if (!obs.init())
        return false;

    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();

        if (comp->debuggerObservesAllExecution() == observing)
            continue;

        // It's expensive to eagerly invalidate and recompile a compartment,
        // so add the compartment to the set only if we are observing.
        if (observing && !obs.add(comp))
            return false;
    }

    if (!updateExecutionObservability(cx, obs, observing))
        return false;

    typedef ExecutionObservableCompartments::CompartmentRange CompartmentRange;
    for (CompartmentRange r = obs.compartments()->all(); !r.empty(); r.popFront())
        r.front()->updateDebuggerObservesAllExecution();

    return true;
}

bool
Debugger::updateObservesCoverageOnDebuggees(JSContext* cx, IsObserving observing)
{
    ExecutionObservableCompartments obs(cx);
    if (!obs.init())
        return false;

    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();

        if (comp->debuggerObservesCoverage() == observing)
            continue;

        // Invalidate and recompile a compartment to add or remove PCCounts
        // increments. We have to eagerly invalidate, as otherwise we might have
        // dangling pointers to freed PCCounts.
        if (!obs.add(comp))
            return false;
    }

    // If any frame on the stack belongs to the debuggee, then we cannot update
    // the ScriptCounts, because this would imply to invalidate a Debugger.Frame
    // to recompile it with/without ScriptCount support.
    for (FrameIter iter(cx);
         !iter.done();
         ++iter)
    {
        if (obs.shouldMarkAsDebuggee(iter)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_IDLE);
            return false;
        }
    }

    if (!updateExecutionObservability(cx, obs, observing))
        return false;

    // All compartments can safely be toggled, and all scripts will be
    // recompiled. Thus we can update each compartment accordingly.
    typedef ExecutionObservableCompartments::CompartmentRange CompartmentRange;
    for (CompartmentRange r = obs.compartments()->all(); !r.empty(); r.popFront())
        r.front()->updateDebuggerObservesCoverage();

    return true;
}

void
Debugger::updateObservesAsmJSOnDebuggees(IsObserving observing)
{
    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();

        if (comp->debuggerObservesAsmJS() == observing)
            continue;

        comp->updateDebuggerObservesAsmJS();
    }
}

void
Debugger::updateObservesBinarySourceDebuggees(IsObserving observing)
{
    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();

        if (comp->debuggerObservesBinarySource() == observing)
            continue;

        comp->updateDebuggerObservesBinarySource();
    }
}


/*** Allocations Tracking *************************************************************************/

/* static */ bool
Debugger::cannotTrackAllocations(const GlobalObject& global)
{
    auto existingCallback = global.compartment()->getAllocationMetadataBuilder();
    return existingCallback && existingCallback != &SavedStacks::metadataBuilder;
}

/* static */ bool
Debugger::isObservedByDebuggerTrackingAllocations(const GlobalObject& debuggee)
{
    if (auto* v = debuggee.getDebuggers()) {
        for (auto p = v->begin(); p != v->end(); p++) {
            if ((*p)->trackingAllocationSites && (*p)->enabled) {
                return true;
            }
        }
    }

    return false;
}

/* static */ bool
Debugger::addAllocationsTracking(JSContext* cx, Handle<GlobalObject*> debuggee)
{
    // Precondition: the given global object is being observed by at least one
    // Debugger that is tracking allocations.
    MOZ_ASSERT(isObservedByDebuggerTrackingAllocations(*debuggee));

    if (Debugger::cannotTrackAllocations(*debuggee)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_OBJECT_METADATA_CALLBACK_ALREADY_SET);
        return false;
    }

    debuggee->compartment()->setAllocationMetadataBuilder(&SavedStacks::metadataBuilder);
    debuggee->compartment()->chooseAllocationSamplingProbability();
    return true;
}

/* static */ void
Debugger::removeAllocationsTracking(GlobalObject& global)
{
    // If there are still Debuggers that are observing allocations, we cannot
    // remove the metadata callback yet. Recompute the sampling probability
    // based on the remaining debuggers' needs.
    if (isObservedByDebuggerTrackingAllocations(global)) {
        global.compartment()->chooseAllocationSamplingProbability();
        return;
    }

    global.compartment()->forgetAllocationMetadataBuilder();
}

bool
Debugger::addAllocationsTrackingForAllDebuggees(JSContext* cx)
{
    MOZ_ASSERT(trackingAllocationSites);

    // We don't want to end up in a state where we added allocations
    // tracking to some of our debuggees, but failed to do so for
    // others. Before attempting to start tracking allocations in *any* of
    // our debuggees, ensure that we will be able to track allocations for
    // *all* of our debuggees.
    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        if (Debugger::cannotTrackAllocations(*r.front().get())) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_OBJECT_METADATA_CALLBACK_ALREADY_SET);
            return false;
        }
    }

    Rooted<GlobalObject*> g(cx);
    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
        // This should always succeed, since we already checked for the
        // error case above.
        g = r.front().get();
        MOZ_ALWAYS_TRUE(Debugger::addAllocationsTracking(cx, g));
    }

    return true;
}

void
Debugger::removeAllocationsTrackingForAllDebuggees()
{
    for (WeakGlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront())
        Debugger::removeAllocationsTracking(*r.front().get());

    allocationsLog.clear();
}



/*** Debugger JSObjects **************************************************************************/

void
Debugger::traceCrossCompartmentEdges(JSTracer* trc)
{
    objects.traceCrossCompartmentEdges<DebuggerObject_trace>(trc);
    environments.traceCrossCompartmentEdges<DebuggerEnv_trace>(trc);
    scripts.traceCrossCompartmentEdges<DebuggerScript_trace>(trc);
    sources.traceCrossCompartmentEdges<DebuggerSource_trace>(trc);
    wasmInstanceScripts.traceCrossCompartmentEdges<DebuggerScript_trace>(trc);
    wasmInstanceSources.traceCrossCompartmentEdges<DebuggerSource_trace>(trc);
}

/*
 * Ordinarily, WeakMap keys and values are marked because at some point it was
 * discovered that the WeakMap was live; that is, some object containing the
 * WeakMap was marked during mark phase.
 *
 * However, during zone GC, we have to do something about cross-compartment
 * edges in non-GC'd compartments. Since the source may be live, we
 * conservatively assume it is and mark the edge.
 *
 * Each Debugger object keeps four cross-compartment WeakMaps: objects, scripts,
 * script source objects, and environments. They have the property that all
 * their values are in the same compartment as the Debugger object, but we have
 * to mark the keys and the private pointer in the wrapper object.
 *
 * We must scan all Debugger objects regardless of whether they *currently* have
 * any debuggees in a compartment being GC'd, because the WeakMap entries
 * persist even when debuggees are removed.
 *
 * This happens during the initial mark phase, not iterative marking, because
 * all the edges being reported here are strong references.
 *
 * This method is also used during compacting GC to update cross compartment
 * pointers into zones that are being compacted.
 */
/* static */ void
Debugger::traceIncomingCrossCompartmentEdges(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();
    gc::State state = rt->gc.state();
    MOZ_ASSERT(state == gc::State::MarkRoots || state == gc::State::Compact);

    for (ZoneGroupsIter group(rt); !group.done(); group.next()) {
        for (Debugger* dbg : group->debuggerList()) {
            Zone* zone = MaybeForwarded(dbg->object.get())->zone();
            if (!zone->isCollecting() || state == gc::State::Compact)
                dbg->traceCrossCompartmentEdges(trc);
        }
    }
}

/*
 * This method has two tasks:
 *   1. Mark Debugger objects that are unreachable except for debugger hooks that
 *      may yet be called.
 *   2. Mark breakpoint handlers.
 *
 * This happens during the iterative part of the GC mark phase. This method
 * returns true if it has to mark anything; GC calls it repeatedly until it
 * returns false.
 */
/* static */ bool
Debugger::markIteratively(GCMarker* marker)
{
    bool markedAny = false;

    /*
     * Find all Debugger objects in danger of GC. This code is a little
     * convoluted since the easiest way to find them is via their debuggees.
     */
    JSRuntime* rt = marker->runtime();
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        if (c->isDebuggee()) {
            GlobalObject* global = c->unsafeUnbarrieredMaybeGlobal();
            if (!IsMarkedUnbarriered(rt, &global))
                continue;

            /*
             * Every debuggee has at least one debugger, so in this case
             * getDebuggers can't return nullptr.
             */
            const GlobalObject::DebuggerVector* debuggers = global->getDebuggers();
            MOZ_ASSERT(debuggers);
            for (auto p = debuggers->begin(); p != debuggers->end(); p++) {
                Debugger* dbg = *p;

                /*
                 * dbg is a Debugger with at least one debuggee. Check three things:
                 *   - dbg is actually in a compartment that is being marked
                 *   - it isn't already marked
                 *   - it actually has hooks that might be called
                 */
                GCPtrNativeObject& dbgobj = dbg->toJSObjectRef();
                if (!dbgobj->zone()->isGCMarking())
                    continue;

                bool dbgMarked = IsMarked(rt, &dbgobj);
                if (!dbgMarked && dbg->hasAnyLiveHooks(rt)) {
                    /*
                     * obj could be reachable only via its live, enabled
                     * debugger hooks, which may yet be called.
                     */
                    TraceEdge(marker, &dbgobj, "enabled Debugger");
                    markedAny = true;
                    dbgMarked = true;
                }

                if (dbgMarked) {
                    /* Search for breakpoints to mark. */
                    for (Breakpoint* bp = dbg->firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
                        switch (bp->site->type()) {
                          case BreakpointSite::Type::JS:
                            if (IsMarkedUnbarriered(rt, &bp->site->asJS()->script)) {
                               /*
                                * The debugger and the script are both live.
                                * Therefore the breakpoint handler is live.
                                */
                               if (!IsMarked(rt, &bp->getHandlerRef())) {
                                   TraceEdge(marker, &bp->getHandlerRef(), "breakpoint handler");
                                   markedAny = true;
                               }
                            }
                            break;
                          case BreakpointSite::Type::Wasm:
                            if (IsMarkedUnbarriered(rt, &bp->asWasm()->wasmInstance)) {
                                /*
                                 * The debugger and the wasm instance are both live.
                                 * Therefore the breakpoint handler is live.
                                 */
                                if (!IsMarked(rt, &bp->getHandlerRef())) {
                                    TraceEdge(marker, &bp->getHandlerRef(), "wasm breakpoint handler");
                                    markedAny = true;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    return markedAny;
}

/* static */ void
Debugger::traceAllForMovingGC(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();
    for (ZoneGroupsIter group(rt); !group.done(); group.next()) {
        for (Debugger* dbg : group->debuggerList())
            dbg->traceForMovingGC(trc);
    }
}

/*
 * Trace all debugger-owned GC things unconditionally. This is used during
 * compacting GC and in minor GC: the minor GC cannot apply the weak constraints
 * of the full GC because it visits only part of the heap.
 */
void
Debugger::traceForMovingGC(JSTracer* trc)
{
    trace(trc);

    for (WeakGlobalObjectSet::Enum e(debuggees); !e.empty(); e.popFront())
        TraceManuallyBarrieredEdge(trc, e.mutableFront().unsafeGet(), "Global Object");

    for (Breakpoint* bp = firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
        switch (bp->site->type()) {
          case BreakpointSite::Type::JS:
            TraceManuallyBarrieredEdge(trc, &bp->site->asJS()->script,
                                       "breakpoint script");
            break;
          case BreakpointSite::Type::Wasm:
            TraceManuallyBarrieredEdge(trc, &bp->asWasm()->wasmInstance, "breakpoint wasm instance");
            break;
        }
        TraceEdge(trc, &bp->getHandlerRef(), "breakpoint handler");
    }
}

/* static */ void
Debugger::traceObject(JSTracer* trc, JSObject* obj)
{
    if (Debugger* dbg = Debugger::fromJSObject(obj))
        dbg->trace(trc);
}

void
Debugger::trace(JSTracer* trc)
{
    TraceEdge(trc, &object, "Debugger Object");

    TraceNullableEdge(trc, &uncaughtExceptionHook, "hooks");

    /*
     * Mark Debugger.Frame objects. These are all reachable from JS, because the
     * corresponding JS frames are still on the stack.
     *
     * (Once we support generator frames properly, we will need
     * weakly-referenced Debugger.Frame objects as well, for suspended generator
     * frames.)
     */
    if (frames.initialized()) {
        for (FrameMap::Range r = frames.all(); !r.empty(); r.popFront()) {
            HeapPtr<DebuggerFrame*>& frameobj = r.front().value();
            MOZ_ASSERT(MaybeForwarded(frameobj.get())->getPrivate());
            TraceEdge(trc, &frameobj, "live Debugger.Frame");
        }
    }

    allocationsLog.trace(trc);

    /* Trace the weak map from JSScript instances to Debugger.Script objects. */
    scripts.trace(trc);

    /* Trace the referent -> Debugger.Source weak map */
    sources.trace(trc);

    /* Trace the referent -> Debugger.Object weak map. */
    objects.trace(trc);

    /* Trace the referent -> Debugger.Environment weak map. */
    environments.trace(trc);

    /* Trace the WasmInstanceObject -> synthesized Debugger.Script weak map. */
    wasmInstanceScripts.trace(trc);

    /* Trace the WasmInstanceObject -> synthesized Debugger.Source weak map. */
    wasmInstanceSources.trace(trc);
}

/* static */ void
Debugger::sweepAll(FreeOp* fop)
{
    JSRuntime* rt = fop->runtime();

    for (ZoneGroupsIter group(rt); !group.done(); group.next()) {
        Debugger* dbg = group->debuggerList().getFirst();
        while (dbg) {
            Debugger* next = dbg->getNext();

            // Detach dying debuggers and debuggees from each other. Since this
            // requires access to both objects it must be done before either
            // object is finalized.
            bool debuggerDying = IsAboutToBeFinalized(&dbg->object);
            for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront()) {
                GlobalObject* global = e.front().unbarrieredGet();
                if (debuggerDying || IsAboutToBeFinalizedUnbarriered(&global))
                    dbg->removeDebuggeeGlobal(fop, e.front().unbarrieredGet(), &e);
            }

            if (debuggerDying)
                fop->delete_(dbg);

            dbg = next;
        }
    }
}

/* static */ void
Debugger::detachAllDebuggersFromGlobal(FreeOp* fop, GlobalObject* global)
{
    const GlobalObject::DebuggerVector* debuggers = global->getDebuggers();
    MOZ_ASSERT(!debuggers->empty());
    while (!debuggers->empty())
        debuggers->back()->removeDebuggeeGlobal(fop, global, nullptr);
}

/* static */ void
Debugger::findZoneEdges(Zone* zone, js::gc::ZoneComponentFinder& finder)
{
    for (ZoneGroupsIter group(zone->runtimeFromActiveCooperatingThread()); !group.done(); group.next()) {
        for (Debugger* dbg : group->debuggerList()) {
            Zone* debuggerZone = dbg->object->zone();
            if (!debuggerZone->isGCMarking())
                continue;

            if (debuggerZone == zone) {
                /*
                 * Add edges to debuggee zones. These are weak references that are
                 * not in the cross compartment wrapper map.
                 */
                for (auto e = dbg->debuggeeZones.all(); !e.empty(); e.popFront()) {
                    Zone* debuggeeZone = e.front();
                    if (debuggeeZone->isGCMarking())
                        finder.addEdgeTo(debuggeeZone);
                }
            } else {
                /*
                 * For debugger cross compartment wrappers, add edges in the
                 * opposite direction to those already added by
                 * JSCompartment::findOutgoingEdges and above.  This ensure that
                 * debuggers and their debuggees are finalized in the same group.
                 */
                if (dbg->debuggeeZones.has(zone) ||
                    dbg->scripts.hasKeyInZone(zone) ||
                    dbg->sources.hasKeyInZone(zone) ||
                    dbg->objects.hasKeyInZone(zone) ||
                    dbg->environments.hasKeyInZone(zone) ||
                    dbg->wasmInstanceScripts.hasKeyInZone(zone) ||
                    dbg->wasmInstanceSources.hasKeyInZone(zone))
                {
                    finder.addEdgeTo(debuggerZone);
                }
            }
        }
    }
}

const ClassOps Debugger::classOps_ = {
    nullptr,    /* addProperty */
    nullptr,    /* delProperty */
    nullptr,    /* enumerate   */
    nullptr,    /* newEnumerate */
    nullptr,    /* resolve     */
    nullptr,    /* mayResolve  */
    nullptr,    /* finalize    */
    nullptr,    /* call        */
    nullptr,    /* hasInstance */
    nullptr,    /* construct   */
    Debugger::traceObject
};

const Class Debugger::class_ = {
    "Debugger",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUG_COUNT),
    &Debugger::classOps_
};

static Debugger*
Debugger_fromThisValue(JSContext* cx, const CallArgs& args, const char* fnname)
{
    JSObject* thisobj = NonNullObject(cx, args.thisv());
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &Debugger::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    /*
     * Forbid Debugger.prototype, which is of the Debugger JSClass but isn't
     * really a Debugger object. The prototype object is distinguished by
     * having a nullptr private value.
     */
    Debugger* dbg = Debugger::fromJSObject(thisobj);
    if (!dbg) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger", fnname, "prototype object");
    }
    return dbg;
}

#define THIS_DEBUGGER(cx, argc, vp, fnname, args, dbg)                       \
    CallArgs args = CallArgsFromVp(argc, vp);                                \
    Debugger* dbg = Debugger_fromThisValue(cx, args, fnname);                \
    if (!dbg)                                                                \
        return false

/* static */ bool
Debugger::getEnabled(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get enabled", args, dbg);
    args.rval().setBoolean(dbg->enabled);
    return true;
}

/* static */ bool
Debugger::setEnabled(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "set enabled", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.set enabled", 1))
        return false;

    bool wasEnabled = dbg->enabled;
    dbg->enabled = ToBoolean(args[0]);

    if (wasEnabled != dbg->enabled) {
        if (dbg->trackingAllocationSites) {
            if (wasEnabled) {
                dbg->removeAllocationsTrackingForAllDebuggees();
            } else {
                if (!dbg->addAllocationsTrackingForAllDebuggees(cx)) {
                    dbg->enabled = false;
                    return false;
                }
            }
        }

        for (Breakpoint* bp = dbg->firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
            if (!wasEnabled)
                bp->site->inc(cx->runtime()->defaultFreeOp());
            else
                bp->site->dec(cx->runtime()->defaultFreeOp());
        }

        /*
         * Add or remove ourselves from the runtime's list of Debuggers
         * that care about new globals.
         */
        if (dbg->getHook(OnNewGlobalObject)) {
            if (!wasEnabled) {
                cx->runtime()->onNewGlobalObjectWatchers().pushBack(dbg);
            } else {
                cx->runtime()->onNewGlobalObjectWatchers().remove(dbg);
            }
        }

        // Ensure the compartment is observable if we are re-enabling a
        // Debugger with hooks that observe all execution.
        if (!dbg->updateObservesAllExecutionOnDebuggees(cx, dbg->observesAllExecution()))
            return false;

        // Note: To toogle code coverage, we currently need to have no live
        // stack frame, thus the coverage does not depend on the enabled flag.

        dbg->updateObservesAsmJSOnDebuggees(dbg->observesAsmJS());
        dbg->updateObservesBinarySourceDebuggees(dbg->observesBinarySource());
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getHookImpl(JSContext* cx, CallArgs& args, Debugger& dbg, Hook which)
{
    MOZ_ASSERT(which >= 0 && which < HookCount);
    args.rval().set(dbg.object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + which));
    return true;
}

/* static */ bool
Debugger::setHookImpl(JSContext* cx, CallArgs& args, Debugger& dbg, Hook which)
{
    MOZ_ASSERT(which >= 0 && which < HookCount);
    if (!args.requireAtLeast(cx, "Debugger.setHook", 1))
        return false;
    if (args[0].isObject()) {
        if (!args[0].toObject().isCallable())
            return ReportIsNotFunction(cx, args[0], args.length() - 1);
    } else if (!args[0].isUndefined()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }
    uint32_t slot = JSSLOT_DEBUG_HOOK_START + which;
    RootedValue oldHook(cx, dbg.object->getReservedSlot(slot));
    dbg.object->setReservedSlot(slot, args[0]);
    if (hookObservesAllExecution(which)) {
        if (!dbg.updateObservesAllExecutionOnDebuggees(cx, dbg.observesAllExecution())) {
            dbg.object->setReservedSlot(slot, oldHook);
            return false;
        }
    }
    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getOnDebuggerStatement(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onDebuggerStatement)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnDebuggerStatement);
}

/* static */ bool
Debugger::setOnDebuggerStatement(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onDebuggerStatement)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnDebuggerStatement);
}

/* static */ bool
Debugger::getOnExceptionUnwind(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onExceptionUnwind)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnExceptionUnwind);
}

/* static */ bool
Debugger::setOnExceptionUnwind(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onExceptionUnwind)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnExceptionUnwind);
}

/* static */ bool
Debugger::getOnNewScript(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onNewScript)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnNewScript);
}

/* static */ bool
Debugger::setOnNewScript(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onNewScript)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnNewScript);
}

/* static */ bool
Debugger::getOnNewPromise(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onNewPromise)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnNewPromise);
}

/* static */ bool
Debugger::setOnNewPromise(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onNewPromise)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnNewPromise);
}

/* static */ bool
Debugger::getOnPromiseSettled(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onPromiseSettled)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnPromiseSettled);
}

/* static */ bool
Debugger::setOnPromiseSettled(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onPromiseSettled)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnPromiseSettled);
}

/* static */ bool
Debugger::getOnEnterFrame(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onEnterFrame)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnEnterFrame);
}

/* static */ bool
Debugger::setOnEnterFrame(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(set onEnterFrame)", args, dbg);
    return setHookImpl(cx, args, *dbg, OnEnterFrame);
}

/* static */ bool
Debugger::getOnNewGlobalObject(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "(get onNewGlobalObject)", args, dbg);
    return getHookImpl(cx, args, *dbg, OnNewGlobalObject);
}

/* static */ bool
Debugger::setOnNewGlobalObject(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "setOnNewGlobalObject", args, dbg);
    RootedObject oldHook(cx, dbg->getHook(OnNewGlobalObject));

    if (!setHookImpl(cx, args, *dbg, OnNewGlobalObject))
        return false;

    /*
     * Add or remove ourselves from the runtime's list of Debuggers that
     * care about new globals.
     */
    if (dbg->enabled) {
        JSObject* newHook = dbg->getHook(OnNewGlobalObject);
        if (!oldHook && newHook) {
            cx->runtime()->onNewGlobalObjectWatchers().pushBack(dbg);
        } else if (oldHook && !newHook) {
            cx->runtime()->onNewGlobalObjectWatchers().remove(dbg);
        }
    }

    return true;
}

/* static */ bool
Debugger::getUncaughtExceptionHook(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get uncaughtExceptionHook", args, dbg);
    args.rval().setObjectOrNull(dbg->uncaughtExceptionHook);
    return true;
}

/* static */ bool
Debugger::setUncaughtExceptionHook(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "set uncaughtExceptionHook", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.set uncaughtExceptionHook", 1))
        return false;
    if (!args[0].isNull() && (!args[0].isObject() || !args[0].toObject().isCallable())) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_ASSIGN_FUNCTION_OR_NULL,
                                  "uncaughtExceptionHook");
        return false;
    }
    dbg->uncaughtExceptionHook = args[0].toObjectOrNull();
    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getAllowUnobservedAsmJS(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get allowUnobservedAsmJS", args, dbg);
    args.rval().setBoolean(dbg->allowUnobservedAsmJS);
    return true;
}

/* static */ bool
Debugger::setAllowUnobservedAsmJS(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "set allowUnobservedAsmJS", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.set allowUnobservedAsmJS", 1))
        return false;
    dbg->allowUnobservedAsmJS = ToBoolean(args[0]);

    for (WeakGlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();
        comp->updateDebuggerObservesAsmJS();
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getAllowWasmBinarySource(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get allowWasmBinarySource", args, dbg);
    args.rval().setBoolean(dbg->allowWasmBinarySource);
    return true;
}

/* static */ bool
Debugger::setAllowWasmBinarySource(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "set allowWasmBinarySource", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.set allowWasmBinarySource", 1))
        return false;
    dbg->allowWasmBinarySource = ToBoolean(args[0]);

    for (WeakGlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront()) {
        GlobalObject* global = r.front();
        JSCompartment* comp = global->compartment();
        comp->updateDebuggerObservesBinarySource();
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getCollectCoverageInfo(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get collectCoverageInfo", args, dbg);
    args.rval().setBoolean(dbg->collectCoverageInfo);
    return true;
}

/* static */ bool
Debugger::setCollectCoverageInfo(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "set collectCoverageInfo", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.set collectCoverageInfo", 1))
        return false;
    dbg->collectCoverageInfo = ToBoolean(args[0]);

    IsObserving observing = dbg->collectCoverageInfo ? Observing : NotObserving;
    if (!dbg->updateObservesCoverageOnDebuggees(cx, observing))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::getMemory(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get memory", args, dbg);
    Value memoryValue = dbg->object->getReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE);

    if (!memoryValue.isObject()) {
        RootedObject memory(cx, DebuggerMemory::create(cx, dbg));
        if (!memory)
            return false;
        memoryValue = ObjectValue(*memory);
    }

    args.rval().set(memoryValue);
    return true;
}

/*
 * Given a value used to designate a global (there's quite a variety; see the
 * docs), return the actual designee.
 *
 * Note that this does not check whether the designee is marked "invisible to
 * Debugger" or not; different callers need to handle invisible-to-Debugger
 * globals in different ways.
 */
GlobalObject*
Debugger::unwrapDebuggeeArgument(JSContext* cx, const Value& v)
{
    if (!v.isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                  "argument", "not a global object");
        return nullptr;
    }

    RootedObject obj(cx, &v.toObject());

    /* If it's a Debugger.Object belonging to this debugger, dereference that. */
    if (obj->getClass() == &DebuggerObject::class_) {
        RootedValue rv(cx, v);
        if (!unwrapDebuggeeValue(cx, &rv))
            return nullptr;
        obj = &rv.toObject();
    }

    /* If we have a cross-compartment wrapper, dereference as far as is secure. */
    obj = CheckedUnwrap(obj);
    if (!obj) {
        ReportAccessDenied(cx);
        return nullptr;
    }

    /* If that produced a WindowProxy, get the Window (global). */
    obj = ToWindowIfWindowProxy(obj);

    /* If that didn't produce a global object, it's an error. */
    if (!obj->is<GlobalObject>()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                  "argument", "not a global object");
        return nullptr;
    }

    return &obj->as<GlobalObject>();
}

/* static */ bool
Debugger::addDebuggee(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "addDebuggee", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.addDebuggee", 1))
        return false;
    Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
    if (!global)
        return false;

    if (!dbg->addDebuggeeGlobal(cx, global))
        return false;

    RootedValue v(cx, ObjectValue(*global));
    if (!dbg->wrapDebuggeeValue(cx, &v))
        return false;
    args.rval().set(v);
    return true;
}

/* static */ bool
Debugger::addAllGlobalsAsDebuggees(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "addAllGlobalsAsDebuggees", args, dbg);
    for (ZonesIter zone(cx->runtime(), SkipAtoms); !zone.done(); zone.next()) {
        for (CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
            if (c == dbg->object->compartment() || c->creationOptions().invisibleToDebugger())
                continue;
            c->scheduledForDestruction = false;
            GlobalObject* global = c->maybeGlobal();
            if (global) {
                Rooted<GlobalObject*> rg(cx, global);
                if (!dbg->addDebuggeeGlobal(cx, rg))
                    return false;
            }
        }
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::removeDebuggee(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "removeDebuggee", args, dbg);

    if (!args.requireAtLeast(cx, "Debugger.removeDebuggee", 1))
        return false;
    Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
    if (!global)
        return false;

    ExecutionObservableCompartments obs(cx);
    if (!obs.init())
        return false;

    if (dbg->debuggees.has(global)) {
        dbg->removeDebuggeeGlobal(cx->runtime()->defaultFreeOp(), global, nullptr);

        // Only update the compartment if there are no Debuggers left, as it's
        // expensive to check if no other Debugger has a live script or frame hook
        // on any of the current on-stack debuggee frames.
        if (global->getDebuggers()->empty() && !obs.add(global->compartment()))
            return false;
        if (!updateExecutionObservability(cx, obs, NotObserving))
            return false;
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::removeAllDebuggees(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "removeAllDebuggees", args, dbg);

    ExecutionObservableCompartments obs(cx);
    if (!obs.init())
        return false;

    for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront()) {
        Rooted<GlobalObject*> global(cx, e.front());
        dbg->removeDebuggeeGlobal(cx->runtime()->defaultFreeOp(), global, &e);

        // See note about adding to the observable set in removeDebuggee.
        if (global->getDebuggers()->empty() && !obs.add(global->compartment()))
            return false;
    }

    if (!updateExecutionObservability(cx, obs, NotObserving))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
Debugger::hasDebuggee(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "hasDebuggee", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.hasDebuggee", 1))
        return false;
    GlobalObject* global = dbg->unwrapDebuggeeArgument(cx, args[0]);
    if (!global)
        return false;
    args.rval().setBoolean(!!dbg->debuggees.lookup(global));
    return true;
}

/* static */ bool
Debugger::getDebuggees(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "getDebuggees", args, dbg);

    // Obtain the list of debuggees before wrapping each debuggee, as a GC could
    // update the debuggees set while we are iterating it.
    unsigned count = dbg->debuggees.count();
    AutoValueVector debuggees(cx);
    if (!debuggees.resize(count))
        return false;
    unsigned i = 0;
    {
        JS::AutoCheckCannotGC nogc;
        for (WeakGlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront())
            debuggees[i++].setObject(*e.front().get());
    }

    RootedArrayObject arrobj(cx, NewDenseFullyAllocatedArray(cx, count));
    if (!arrobj)
        return false;
    arrobj->ensureDenseInitializedLength(cx, 0, count);
    for (i = 0; i < count; i++) {
        RootedValue v(cx, debuggees[i]);
        if (!dbg->wrapDebuggeeValue(cx, &v))
            return false;
        arrobj->setDenseElement(i, v);
    }

    args.rval().setObject(*arrobj);
    return true;
}

/* static */ bool
Debugger::getNewestFrame(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "getNewestFrame", args, dbg);

    /* Since there may be multiple contexts, use AllFramesIter. */
    for (AllFramesIter i(cx); !i.done(); ++i) {
        if (dbg->observesFrame(i)) {
            // Ensure that Ion frames are rematerialized. Only rematerialized
            // Ion frames may be used as AbstractFramePtrs.
            if (i.isIon() && !i.ensureHasRematerializedFrame(cx))
                return false;
            AbstractFramePtr frame = i.abstractFramePtr();
            FrameIter iter(i.activation()->cx());
            while (!iter.hasUsableAbstractFramePtr() || iter.abstractFramePtr() != frame)
                ++iter;
            return dbg->getScriptFrame(cx, iter, args.rval());
        }
    }
    args.rval().setNull();
    return true;
}

/* static */ bool
Debugger::clearAllBreakpoints(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "clearAllBreakpoints", args, dbg);
    for (WeakGlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront())
        r.front()->compartment()->clearBreakpointsIn(cx->runtime()->defaultFreeOp(),
                                                     dbg, nullptr);
    return true;
}

/* static */ bool
Debugger::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Check that the arguments, if any, are cross-compartment wrappers. */
    for (unsigned i = 0; i < args.length(); i++) {
        JSObject* argobj = NonNullObject(cx, args[i]);
        if (!argobj)
            return false;
        if (!argobj->is<CrossCompartmentWrapperObject>()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_CCW_REQUIRED,
                                      "Debugger");
            return false;
        }
    }

    /* Get Debugger.prototype. */
    RootedValue v(cx);
    RootedObject callee(cx, &args.callee());
    if (!GetProperty(cx, callee, callee, cx->names().prototype, &v))
        return false;
    RootedNativeObject proto(cx, &v.toObject().as<NativeObject>());
    MOZ_ASSERT(proto->getClass() == &Debugger::class_);
    /*
     * Make the new Debugger object. Each one has a reference to
     * Debugger.{Frame,Object,Script,Memory}.prototype in reserved slots. The
     * rest of the reserved slots are for hooks; they default to undefined.
     */
    RootedNativeObject obj(cx, NewNativeObjectWithGivenProto(cx, &Debugger::class_, proto,
                                                             TenuredObject));
    if (!obj)
        return false;
    for (unsigned slot = JSSLOT_DEBUG_PROTO_START; slot < JSSLOT_DEBUG_PROTO_STOP; slot++)
        obj->setReservedSlot(slot, proto->getReservedSlot(slot));
    obj->setReservedSlot(JSSLOT_DEBUG_MEMORY_INSTANCE, NullValue());

    // Debuggers currently require single threaded execution. A debugger may be
    // used to debug content in other zone groups, and may be used to observe
    // all activity in the runtime via hooks like OnNewGlobalObject.
    if (!cx->runtime()->beginSingleThreadedExecution(cx)) {
        JS_ReportErrorASCII(cx, "Cannot ensure single threaded execution in Debugger");
        return false;
    }

    Debugger* debugger;
    {
        /* Construct the underlying C++ object. */
        auto dbg = cx->make_unique<Debugger>(cx, obj.get());
        if (!dbg) {
            JS::AutoSuppressGCAnalysis nogc; // Suppress warning about |dbg|.
            cx->runtime()->endSingleThreadedExecution(cx);
            return false;
        }
        if (!dbg->init(cx))
            return false;

        debugger = dbg.release();
        obj->setPrivate(debugger); // owns the released pointer
    }

    /* Add the initial debuggees, if any. */
    for (unsigned i = 0; i < args.length(); i++) {
        Rooted<GlobalObject*>
            debuggee(cx, &args[i].toObject().as<ProxyObject>().private_().toObject().global());
        if (!debugger->addDebuggeeGlobal(cx, debuggee))
            return false;
    }

    args.rval().setObject(*obj);
    return true;
}

bool
Debugger::addDebuggeeGlobal(JSContext* cx, Handle<GlobalObject*> global)
{
    if (debuggees.has(global))
        return true;

    // Callers should generally be unable to get a reference to a debugger-
    // invisible global in order to pass it to addDebuggee. But this is possible
    // with certain testing aides we expose in the shell, so just make addDebuggee
    // throw in that case.
    JSCompartment* debuggeeCompartment = global->compartment();
    if (debuggeeCompartment->creationOptions().invisibleToDebugger()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_CANT_DEBUG_GLOBAL);
        return false;
    }

    /*
     * Check for cycles. If global's compartment is reachable from this
     * Debugger object's compartment by following debuggee-to-debugger links,
     * then adding global would create a cycle. (Typically nobody is debugging
     * the debugger, in which case we zip through this code without looping.)
     */
    Vector<JSCompartment*> visited(cx);
    if (!visited.append(object->compartment()))
        return false;
    for (size_t i = 0; i < visited.length(); i++) {
        JSCompartment* c = visited[i];
        if (c == debuggeeCompartment) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_LOOP);
            return false;
        }

        /*
         * Find all compartments containing debuggers debugging c's global
         * object. Add those compartments to visited.
         */
        if (c->isDebuggee()) {
            GlobalObject::DebuggerVector* v = c->maybeGlobal()->getDebuggers();
            for (auto p = v->begin(); p != v->end(); p++) {
                JSCompartment* next = (*p)->object->compartment();
                if (Find(visited, next) == visited.end() && !visited.append(next))
                    return false;
            }
        }
    }

    /*
     * For global to become this js::Debugger's debuggee:
     *
     * 1. this js::Debugger must be in global->getDebuggers(),
     * 2. global must be in this->debuggees,
     * 3. it must be in zone->getDebuggers(),
     * 4. the debuggee's zone must be in this->debuggeeZones,
     * 5. if we are tracking allocations, the SavedStacksMetadataBuilder must be
     *    installed for this compartment, and
     * 6. JSCompartment::isDebuggee()'s bit must be set.
     *
     * All six indications must be kept consistent.
     */

    AutoCompartment ac(cx, global);
    Zone* zone = global->zone();

    // (1)
    auto* globalDebuggers = GlobalObject::getOrCreateDebuggers(cx, global);
    if (!globalDebuggers)
        return false;
    if (!globalDebuggers->append(this)) {
        ReportOutOfMemory(cx);
        return false;
    }
    auto globalDebuggersGuard = MakeScopeExit([&] {
        globalDebuggers->popBack();
    });

    // (2)
    if (!debuggees.put(global)) {
        ReportOutOfMemory(cx);
        return false;
    }
    auto debuggeesGuard = MakeScopeExit([&] {
        debuggees.remove(global);
    });

    bool addingZoneRelation = !debuggeeZones.has(zone);

    // (3)
    auto* zoneDebuggers = zone->getOrCreateDebuggers(cx);
    if (!zoneDebuggers)
        return false;
    if (addingZoneRelation && !zoneDebuggers->append(this)) {
        ReportOutOfMemory(cx);
        return false;
    }
    auto zoneDebuggersGuard = MakeScopeExit([&] {
        if (addingZoneRelation)
            zoneDebuggers->popBack();
    });

    // (4)
    if (addingZoneRelation && !debuggeeZones.put(zone)) {
        ReportOutOfMemory(cx);
        return false;
    }
    auto debuggeeZonesGuard = MakeScopeExit([&] {
        if (addingZoneRelation)
            debuggeeZones.remove(zone);
    });

    // (5)
    if (trackingAllocationSites && enabled && !Debugger::addAllocationsTracking(cx, global))
        return false;

    auto allocationsTrackingGuard = MakeScopeExit([&] {
        if (trackingAllocationSites && enabled)
            Debugger::removeAllocationsTracking(*global);
    });

    // (6)
    AutoRestoreCompartmentDebugMode debugModeGuard(debuggeeCompartment);
    debuggeeCompartment->setIsDebuggee();
    debuggeeCompartment->updateDebuggerObservesAsmJS();
    debuggeeCompartment->updateDebuggerObservesBinarySource();
    debuggeeCompartment->updateDebuggerObservesCoverage();
    if (observesAllExecution() && !ensureExecutionObservabilityOfCompartment(cx, debuggeeCompartment))
        return false;

    globalDebuggersGuard.release();
    debuggeesGuard.release();
    zoneDebuggersGuard.release();
    debuggeeZonesGuard.release();
    allocationsTrackingGuard.release();
    debugModeGuard.release();
    return true;
}

void
Debugger::recomputeDebuggeeZoneSet()
{
    AutoEnterOOMUnsafeRegion oomUnsafe;
    debuggeeZones.clear();
    for (auto range = debuggees.all(); !range.empty(); range.popFront()) {
        if (!debuggeeZones.put(range.front().unbarrieredGet()->zone()))
            oomUnsafe.crash("Debugger::removeDebuggeeGlobal");
    }
}

template <typename T>
static T*
findDebuggerInVector(Debugger* dbg, Vector<T, 0, js::SystemAllocPolicy>* vec)
{
    T* p;
    for (p = vec->begin(); p != vec->end(); p++) {
        if (*p == dbg)
            break;
    }
    MOZ_ASSERT(p != vec->end());
    return p;
}

void
Debugger::removeDebuggeeGlobal(FreeOp* fop, GlobalObject* global,
                               WeakGlobalObjectSet::Enum* debugEnum)
{
    /*
     * The caller might have found global by enumerating this->debuggees; if
     * so, use HashSet::Enum::removeFront rather than HashSet::remove below,
     * to avoid invalidating the live enumerator.
     */
    MOZ_ASSERT(debuggees.has(global));
    MOZ_ASSERT(debuggeeZones.has(global->zone()));
    MOZ_ASSERT_IF(debugEnum, debugEnum->front().unbarrieredGet() == global);

    /*
     * FIXME Debugger::slowPathOnLeaveFrame needs to kill all Debugger.Frame
     * objects referring to a particular JS stack frame. This is hard if
     * Debugger objects that are no longer debugging the relevant global might
     * have live Frame objects. So we take the easy way out and kill them here.
     * This is a bug, since it's observable and contrary to the spec. One
     * possible fix would be to put such objects into a compartment-wide bag
     * which slowPathOnLeaveFrame would have to examine.
     */
    for (FrameMap::Enum e(frames); !e.empty(); e.popFront()) {
        AbstractFramePtr frame = e.front().key();
        NativeObject* frameobj = e.front().value();
        if (frame.global() == global) {
            DebuggerFrame_freeScriptFrameIterData(fop, frameobj);
            DebuggerFrame_maybeDecrementFrameScriptStepModeCount(fop, frame, frameobj);
            e.removeFront();
        }
    }

    auto *globalDebuggersVector = global->getDebuggers();
    auto *zoneDebuggersVector = global->zone()->getDebuggers();

    /*
     * The relation must be removed from up to three places:
     * globalDebuggersVector and debuggees for sure, and possibly the
     * compartment's debuggee set.
     *
     * The debuggee zone set is recomputed on demand. This avoids refcounting
     * and in practice we have relatively few debuggees that tend to all be in
     * the same zone. If after recomputing the debuggee zone set, this global's
     * zone is not in the set, then we must remove ourselves from the zone's
     * vector of observing debuggers.
     */
    globalDebuggersVector->erase(findDebuggerInVector(this, globalDebuggersVector));

    if (debugEnum)
        debugEnum->removeFront();
    else
        debuggees.remove(global);

    recomputeDebuggeeZoneSet();

    if (!debuggeeZones.has(global->zone()))
        zoneDebuggersVector->erase(findDebuggerInVector(this, zoneDebuggersVector));

    /* Remove all breakpoints for the debuggee. */
    Breakpoint* nextbp;
    for (Breakpoint* bp = firstBreakpoint(); bp; bp = nextbp) {
        nextbp = bp->nextInDebugger();
        switch (bp->site->type()) {
          case BreakpointSite::Type::JS:
            if (bp->site->asJS()->script->compartment() == global->compartment())
                bp->destroy(fop);
            break;
          case BreakpointSite::Type::Wasm:
            if (bp->asWasm()->wasmInstance->compartment() == global->compartment())
                bp->destroy(fop);
            break;
        }
    }
    MOZ_ASSERT_IF(debuggees.empty(), !firstBreakpoint());

    /*
     * If we are tracking allocation sites, we need to remove the object
     * metadata callback from this global's compartment.
     */
    if (trackingAllocationSites)
        Debugger::removeAllocationsTracking(*global);

    if (global->getDebuggers()->empty()) {
        global->compartment()->unsetIsDebuggee();
    } else {
        global->compartment()->updateDebuggerObservesAllExecution();
        global->compartment()->updateDebuggerObservesAsmJS();
        global->compartment()->updateDebuggerObservesBinarySource();
        global->compartment()->updateDebuggerObservesCoverage();
    }
}


static inline DebuggerSourceReferent GetSourceReferent(JSObject* obj);

/*
 * A class for parsing 'findScripts' query arguments and searching for
 * scripts that match the criteria they represent.
 */
class MOZ_STACK_CLASS Debugger::ScriptQuery
{
  public:
    /* Construct a ScriptQuery to use matching scripts for |dbg|. */
    ScriptQuery(JSContext* cx, Debugger* dbg):
        cx(cx),
        debugger(dbg),
        iterMarker(&cx->runtime()->gc),
        compartments(cx->zone()),
        url(cx),
        displayURLString(cx),
        hasSource(false),
        source(cx, AsVariant(static_cast<ScriptSourceObject*>(nullptr))),
        innermostForCompartment(cx->zone()),
        vector(cx, ScriptVector(cx)),
        wasmInstanceVector(cx, WasmInstanceObjectVector(cx))
    {}

    /*
     * Initialize this ScriptQuery. Raise an error and return false if we
     * haven't enough memory.
     */
    bool init() {
        if (!compartments.init() ||
            !innermostForCompartment.init())
        {
            ReportOutOfMemory(cx);
            return false;
        }

        return true;
    }

    /*
     * Parse the query object |query|, and prepare to match only the scripts
     * it specifies.
     */
    bool parseQuery(HandleObject query) {
        /*
         * Check for a 'global' property, which limits the results to those
         * scripts scoped to a particular global object.
         */
        RootedValue global(cx);
        if (!GetProperty(cx, query, query, cx->names().global, &global))
            return false;
        if (global.isUndefined()) {
            if (!matchAllDebuggeeGlobals())
                return false;
        } else {
            GlobalObject* globalObject = debugger->unwrapDebuggeeArgument(cx, global);
            if (!globalObject)
                return false;

            /*
             * If the given global isn't a debuggee, just leave the set of
             * acceptable globals empty; we'll return no scripts.
             */
            if (debugger->debuggees.has(globalObject)) {
                if (!matchSingleGlobal(globalObject))
                    return false;
            }
        }

        /* Check for a 'url' property. */
        if (!GetProperty(cx, query, query, cx->names().url, &url))
            return false;
        if (!url.isUndefined() && !url.isString()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                      "query object's 'url' property",
                                      "neither undefined nor a string");
            return false;
        }

        /* Check for a 'source' property */
        RootedValue debuggerSource(cx);
        if (!GetProperty(cx, query, query, cx->names().source, &debuggerSource))
            return false;
        if (!debuggerSource.isUndefined()) {
            if (!debuggerSource.isObject() ||
                debuggerSource.toObject().getClass() != &DebuggerSource_class) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                          "query object's 'source' property",
                                          "not undefined nor a Debugger.Source object");
                return false;
            }

            Value owner = debuggerSource.toObject()
                          .as<NativeObject>()
                          .getReservedSlot(JSSLOT_DEBUGSOURCE_OWNER);

            /*
             * The given source must have an owner. Otherwise, it's a
             * Debugger.Source.prototype, which would match no scripts, and is
             * probably a mistake.
             */
            if (!owner.isObject()) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROTO,
                                          "Debugger.Source", "Debugger.Source");
                return false;
            }

            /*
             * If it does have an owner, it should match the Debugger we're
             * calling findScripts on. It would work fine even if it didn't,
             * but mixing Debugger.Sources is probably a sign of confusion.
             */
            if (&owner.toObject() != debugger->object) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_WRONG_OWNER,
                                          "Debugger.Source");
                return false;
            }

            hasSource = true;
            source = GetSourceReferent(&debuggerSource.toObject());
        }

        /* Check for a 'displayURL' property. */
        RootedValue displayURL(cx);
        if (!GetProperty(cx, query, query, cx->names().displayURL, &displayURL))
            return false;
        if (!displayURL.isUndefined() && !displayURL.isString()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                      "query object's 'displayURL' property",
                                      "neither undefined nor a string");
            return false;
        }

        if (displayURL.isString()) {
            displayURLString = displayURL.toString()->ensureLinear(cx);
            if (!displayURLString)
                return false;
        }

        /* Check for a 'line' property. */
        RootedValue lineProperty(cx);
        if (!GetProperty(cx, query, query, cx->names().line, &lineProperty))
            return false;
        if (lineProperty.isUndefined()) {
            hasLine = false;
        } else if (lineProperty.isNumber()) {
            if (displayURL.isUndefined() && url.isUndefined() && !hasSource) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_QUERY_LINE_WITHOUT_URL);
                return false;
            }
            double doubleLine = lineProperty.toNumber();
            if (doubleLine <= 0 || (unsigned int) doubleLine != doubleLine) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_LINE);
                return false;
            }
            hasLine = true;
            line = doubleLine;
        } else {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                      "query object's 'line' property",
                                      "neither undefined nor an integer");
            return false;
        }

        /* Check for an 'innermost' property. */
        PropertyName* innermostName = cx->names().innermost;
        RootedValue innermostProperty(cx);
        if (!GetProperty(cx, query, query, innermostName, &innermostProperty))
            return false;
        innermost = ToBoolean(innermostProperty);
        if (innermost) {
            /* Technically, we need only check hasLine, but this is clearer. */
            if ((displayURL.isUndefined() && url.isUndefined() && !hasSource) || !hasLine) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_QUERY_INNERMOST_WITHOUT_LINE_URL);
                return false;
            }
        }

        return true;
    }

    /* Set up this ScriptQuery appropriately for a missing query argument. */
    bool omittedQuery() {
        url.setUndefined();
        hasLine = false;
        innermost = false;
        displayURLString = nullptr;
        return matchAllDebuggeeGlobals();
    }

    /*
     * Search all relevant compartments and the stack for scripts matching
     * this query, and append the matching scripts to |vector|.
     */
    bool findScripts() {
        if (!prepareQuery() || !delazifyScripts())
            return false;

        JSCompartment* singletonComp = nullptr;
        if (compartments.count() == 1)
            singletonComp = compartments.all().front();

        /* Search each compartment for debuggee scripts. */
        MOZ_ASSERT(vector.empty());
        oom = false;
        IterateScripts(cx, singletonComp, this, considerScript);
        if (oom) {
            ReportOutOfMemory(cx);
            return false;
        }

        /* We cannot touch the gray bits while isHeapBusy, so do this now. */
        for (JSScript** i = vector.begin(); i != vector.end(); ++i)
            JS::ExposeScriptToActiveJS(*i);

        /*
         * For most queries, we just accumulate results in 'vector' as we find
         * them. But if this is an 'innermost' query, then we've accumulated the
         * results in the 'innermostForCompartment' map. In that case, we now need to
         * walk that map and populate 'vector'.
         */
        if (innermost) {
            for (CompartmentToScriptMap::Range r = innermostForCompartment.all();
                 !r.empty();
                 r.popFront())
            {
                JS::ExposeScriptToActiveJS(r.front().value());
                if (!vector.append(r.front().value())) {
                    ReportOutOfMemory(cx);
                    return false;
                }
            }
        }

        // TODOshu: Until such time that wasm modules are real ES6 modules,
        // unconditionally consider all wasm toplevel instance scripts.
        for (WeakGlobalObjectSet::Range r = debugger->allDebuggees(); !r.empty(); r.popFront()) {
            for (wasm::Instance* instance : r.front()->compartment()->wasm.instances()) {
                consider(instance->object());
                if (oom) {
                    ReportOutOfMemory(cx);
                    return false;
                }
            }
        }

        return true;
    }

    Handle<ScriptVector> foundScripts() const {
        return vector;
    }

    Handle<WasmInstanceObjectVector> foundWasmInstances() const {
        return wasmInstanceVector;
    }

  private:
    /* The context in which we should do our work. */
    JSContext* cx;

    /* The debugger for which we conduct queries. */
    Debugger* debugger;

    /* Require the set of compartments to stay fixed while the ScriptQuery is alive. */
    gc::AutoEnterIteration iterMarker;

    typedef HashSet<JSCompartment*, DefaultHasher<JSCompartment*>, ZoneAllocPolicy>
        CompartmentSet;

    /* A script must be in one of these compartments to match the query. */
    CompartmentSet compartments;

    /* If this is a string, matching scripts have urls equal to it. */
    RootedValue url;

    /* url as a C string. */
    JSAutoByteString urlCString;

    /* If this is a string, matching scripts' sources have displayURLs equal to
     * it. */
    RootedLinearString displayURLString;

    /*
     * If this is a source referent, matching scripts will have sources equal
     * to this instance. Ideally we'd use a Maybe here, but Maybe interacts
     * very badly with Rooted's LIFO invariant.
     */
    bool hasSource;
    Rooted<DebuggerSourceReferent> source;

    /* True if the query contained a 'line' property. */
    bool hasLine;

    /* The line matching scripts must cover. */
    unsigned int line;

    /* True if the query has an 'innermost' property whose value is true. */
    bool innermost;

    typedef HashMap<JSCompartment*, JSScript*, DefaultHasher<JSCompartment*>, ZoneAllocPolicy>
        CompartmentToScriptMap;

    /*
     * For 'innermost' queries, a map from compartments to the innermost script
     * we've seen so far in that compartment. (Template instantiation code size
     * explosion ho!)
     */
    CompartmentToScriptMap innermostForCompartment;

    /*
     * Accumulate the scripts in an Rooted<ScriptVector>, instead of creating
     * the JS array as we go, because we mustn't allocate JS objects or GC
     * while we use the CellIter.
     */
    Rooted<ScriptVector> vector;

    /*
     * Like above, but for wasm modules.
     */
    Rooted<WasmInstanceObjectVector> wasmInstanceVector;

    /* Indicates whether OOM has occurred while matching. */
    bool oom;

    bool addCompartment(JSCompartment* comp) {
        return compartments.put(comp);
    }

    /* Arrange for this ScriptQuery to match only scripts that run in |global|. */
    bool matchSingleGlobal(GlobalObject* global) {
        MOZ_ASSERT(compartments.count() == 0);
        if (!addCompartment(global->compartment())) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    /*
     * Arrange for this ScriptQuery to match all scripts running in debuggee
     * globals.
     */
    bool matchAllDebuggeeGlobals() {
        MOZ_ASSERT(compartments.count() == 0);
        /* Build our compartment set from the debugger's set of debuggee globals. */
        for (WeakGlobalObjectSet::Range r = debugger->debuggees.all(); !r.empty(); r.popFront()) {
            if (!addCompartment(r.front()->compartment())) {
                ReportOutOfMemory(cx);
                return false;
            }
        }
        return true;
    }

    /*
     * Given that parseQuery or omittedQuery has been called, prepare to match
     * scripts. Set urlCString and displayURLChars as appropriate.
     */
    bool prepareQuery() {
        /* Compute urlCString and displayURLChars, if a url or displayURL was
         * given respectively. */
        if (url.isString()) {
            if (!urlCString.encodeLatin1(cx, url.toString()))
                return false;
        }

        return true;
    }

    bool delazifyScripts() {
        // All scripts in debuggee compartments must be visible, so delazify
        // everything.
        for (auto r = compartments.all(); !r.empty(); r.popFront()) {
            JSCompartment* comp = r.front();
            if (!comp->ensureDelazifyScriptsForDebugger(cx))
                return false;
        }
        return true;
    }

    static void considerScript(JSRuntime* rt, void* data, JSScript* script) {
        ScriptQuery* self = static_cast<ScriptQuery*>(data);
        self->consider(script);
    }

    /*
     * If |script| matches this query, append it to |vector| or place it in
     * |innermostForCompartment|, as appropriate. Set |oom| if an out of memory
     * condition occurred.
     */
    void consider(JSScript* script) {
        // We check for presence of script->code() because it is possible that
        // the script was created and thus exposed to GC, but *not* fully
        // initialized from fullyInit{FromEmitter,Trivial} due to errors.
        if (oom || script->selfHosted() || !script->code())
            return;
        JSCompartment* compartment = script->compartment();
        if (!compartments.has(compartment))
            return;
        if (urlCString.ptr()) {
            bool gotFilename = false;
            if (script->filename() && strcmp(script->filename(), urlCString.ptr()) == 0)
                gotFilename = true;

            bool gotSourceURL = false;
            if (!gotFilename && script->scriptSource()->introducerFilename() &&
                strcmp(script->scriptSource()->introducerFilename(), urlCString.ptr()) == 0)
            {
                gotSourceURL = true;
            }
            if (!gotFilename && !gotSourceURL)
                return;
        }
        if (hasLine) {
            if (line < script->lineno() || script->lineno() + GetScriptLineExtent(script) < line)
                return;
        }
        if (displayURLString) {
            if (!script->scriptSource() || !script->scriptSource()->hasDisplayURL())
                return;

            const char16_t* s = script->scriptSource()->displayURL();
            if (CompareChars(s, js_strlen(s), displayURLString) != 0)
                return;
        }
        if (hasSource && !(source.is<ScriptSourceObject*>() &&
                           source.as<ScriptSourceObject*>()->source() == script->scriptSource()))
        {
            return;
        }

        if (innermost) {
            /*
             * For 'innermost' queries, we don't place scripts in |vector| right
             * away; we may later find another script that is nested inside this
             * one. Instead, we record the innermost script we've found so far
             * for each compartment in innermostForCompartment, and only
             * populate |vector| at the bottom of findScripts, when we've
             * traversed all the scripts.
             *
             * So: check this script against the innermost one we've found so
             * far (if any), as recorded in innermostForCompartment, and replace
             * that if it's better.
             */
            CompartmentToScriptMap::AddPtr p = innermostForCompartment.lookupForAdd(compartment);
            if (p) {
                /* Is our newly found script deeper than the last one we found? */
                JSScript* incumbent = p->value();
                if (script->innermostScope()->chainLength() >
                    incumbent->innermostScope()->chainLength())
                {
                    p->value() = script;
                }
            } else {
                /*
                 * This is the first matching script we've encountered for this
                 * compartment, so it is thus the innermost such script.
                 */
                if (!innermostForCompartment.add(p, compartment, script)) {
                    oom = true;
                    return;
                }
            }
        } else {
            /* Record this matching script in the results vector. */
            if (!vector.append(script)) {
                oom = true;
                return;
            }
        }
    }

    /*
     * If |instanceObject| matches this query, append it to |wasmInstanceVector|.
     * Set |oom| if an out of memory condition occurred.
     */
    void consider(WasmInstanceObject* instanceObject) {
        if (oom)
            return;

        if (hasSource && source != AsVariant(instanceObject))
            return;

        if (!wasmInstanceVector.append(instanceObject))
            oom = true;
    }
};

/* static */ bool
Debugger::findScripts(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "findScripts", args, dbg);

    ScriptQuery query(cx, dbg);
    if (!query.init())
        return false;

    if (args.length() >= 1) {
        RootedObject queryObject(cx, NonNullObject(cx, args[0]));
        if (!queryObject || !query.parseQuery(queryObject))
            return false;
    } else {
        if (!query.omittedQuery())
            return false;
    }

    if (!query.findScripts())
        return false;

    Handle<ScriptVector> scripts(query.foundScripts());
    Handle<WasmInstanceObjectVector> wasmInstances(query.foundWasmInstances());

    size_t resultLength = scripts.length() + wasmInstances.length();
    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, resultLength));
    if (!result)
        return false;

    result->ensureDenseInitializedLength(cx, 0, resultLength);

    for (size_t i = 0; i < scripts.length(); i++) {
        JSObject* scriptObject = dbg->wrapScript(cx, scripts[i]);
        if (!scriptObject)
            return false;
        result->setDenseElement(i, ObjectValue(*scriptObject));
    }

    size_t wasmStart = scripts.length();
    for (size_t i = 0; i < wasmInstances.length(); i++) {
        JSObject* scriptObject = dbg->wrapWasmScript(cx, wasmInstances[i]);
        if (!scriptObject)
            return false;
        result->setDenseElement(wasmStart + i, ObjectValue(*scriptObject));
    }

    args.rval().setObject(*result);
    return true;
}

/*
 * A class for parsing 'findObjects' query arguments and searching for objects
 * that match the criteria they represent.
 */
class MOZ_STACK_CLASS Debugger::ObjectQuery
{
  public:
    /* Construct an ObjectQuery to use matching scripts for |dbg|. */
    ObjectQuery(JSContext* cx, Debugger* dbg) :
        objects(cx), cx(cx), dbg(dbg), className(cx)
    { }

    /* The vector that we are accumulating results in. */
    AutoObjectVector objects;

    /*
     * Parse the query object |query|, and prepare to match only the objects it
     * specifies.
     */
    bool parseQuery(HandleObject query) {
        /* Check for the 'class' property */
        RootedValue cls(cx);
        if (!GetProperty(cx, query, query, cx->names().class_, &cls))
            return false;
        if (!cls.isUndefined()) {
            if (!cls.isString()) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                                          "query object's 'class' property",
                                          "neither undefined nor a string");
                return false;
            }
            className = cls;
        }
        return true;
    }

    /* Set up this ObjectQuery appropriately for a missing query argument. */
    void omittedQuery() {
        className.setUndefined();
    }

    /*
     * Traverse the heap to find all relevant objects and add them to the
     * provided vector.
     */
    bool findObjects() {
        if (!prepareQuery())
            return false;

        {
            /*
             * We can't tolerate the GC moving things around while we're
             * searching the heap. Check that nothing we do causes a GC.
             */
            Maybe<JS::AutoCheckCannotGC> maybeNoGC;
            RootedObject dbgObj(cx, dbg->object);
            JS::ubi::RootList rootList(cx, maybeNoGC);
            if (!rootList.init(dbgObj)) {
                ReportOutOfMemory(cx);
                return false;
            }

            Traversal traversal(cx, *this, maybeNoGC.ref());
            if (!traversal.init()) {
                ReportOutOfMemory(cx);
                return false;
            }
            traversal.wantNames = false;

            return traversal.addStart(JS::ubi::Node(&rootList)) &&
                   traversal.traverse();
        }
    }

    /*
     * |ubi::Node::BreadthFirst| interface.
     */
    class NodeData {};
    typedef JS::ubi::BreadthFirst<ObjectQuery> Traversal;
    bool operator() (Traversal& traversal, JS::ubi::Node origin, const JS::ubi::Edge& edge,
                     NodeData*, bool first)
    {
        if (!first)
            return true;

        JS::ubi::Node referent = edge.referent;
        /*
         * Only follow edges within our set of debuggee compartments; we don't
         * care about the heap's subgraphs outside of our debuggee compartments,
         * so we abandon the referent. Either (1) there is not a path from this
         * non-debuggee node back to a node in our debuggee compartments, and we
         * don't need to follow edges to or from this node, or (2) there does
         * exist some path from this non-debuggee node back to a node in our
         * debuggee compartments. However, if that were true, then the incoming
         * cross compartment edge back into a debuggee compartment is already
         * listed as an edge in the RootList we started traversal with, and
         * therefore we don't need to follow edges to or from this non-debuggee
         * node.
         */
        JSCompartment* comp = referent.compartment();
        if (comp && !dbg->isDebuggeeUnbarriered(comp)) {
            traversal.abandonReferent();
            return true;
        }

        /*
         * If the referent is an object and matches our query's restrictions,
         * add it to the vector accumulating results. Skip objects that should
         * never be exposed to JS, like EnvironmentObjects and internal
         * functions.
         */

        if (!referent.is<JSObject>() || referent.exposeToJS().isUndefined())
            return true;

        JSObject* obj = referent.as<JSObject>();

        if (!className.isUndefined()) {
            const char* objClassName = obj->getClass()->name;
            if (strcmp(objClassName, classNameCString.ptr()) != 0)
                return true;
        }

        return objects.append(obj);
    }

  private:
    /* The context in which we should do our work. */
    JSContext* cx;

    /* The debugger for which we conduct queries. */
    Debugger* dbg;

    /*
     * If this is non-null, matching objects will have a class whose name is
     * this property.
     */
    RootedValue className;

    /* The className member, as a C string. */
    JSAutoByteString classNameCString;

    /*
     * Given that either omittedQuery or parseQuery has been called, prepare the
     * query for matching objects.
     */
    bool prepareQuery() {
        if (className.isString()) {
            if (!classNameCString.encodeLatin1(cx, className.toString()))
                return false;
        }

        return true;
    }
};

bool
Debugger::findObjects(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "findObjects", args, dbg);

    ObjectQuery query(cx, dbg);

    if (args.length() >= 1) {
        RootedObject queryObject(cx, NonNullObject(cx, args[0]));
        if (!queryObject || !query.parseQuery(queryObject))
            return false;
    } else {
        query.omittedQuery();
    }

    if (!query.findObjects())
        return false;

    size_t length = query.objects.length();
    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;

    result->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++) {
        RootedValue debuggeeVal(cx, ObjectValue(*query.objects[i]));
        if (!dbg->wrapDebuggeeValue(cx, &debuggeeVal))
            return false;
        result->setDenseElement(i, debuggeeVal);
    }

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
Debugger::findAllGlobals(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "findAllGlobals", args, dbg);

    AutoObjectVector globals(cx);

    {
        // Accumulate the list of globals before wrapping them, because
        // wrapping can GC and collect compartments from under us, while
        // iterating.
        JS::AutoCheckCannotGC nogc;

        for (CompartmentsIter c(cx->runtime(), SkipAtoms); !c.done(); c.next()) {
            if (c->creationOptions().invisibleToDebugger())
                continue;

            c->scheduledForDestruction = false;

            GlobalObject* global = c->maybeGlobal();

            if (cx->runtime()->isSelfHostingGlobal(global))
                continue;

            if (global) {
                /*
                 * We pulled |global| out of nowhere, so it's possible that it was
                 * marked gray by XPConnect. Since we're now exposing it to JS code,
                 * we need to mark it black.
                 */
                JS::ExposeObjectToActiveJS(global);
                if (!globals.append(global))
                    return false;
            }
        }
    }

    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;

    for (size_t i = 0; i < globals.length(); i++) {
        RootedValue globalValue(cx, ObjectValue(*globals[i]));
        if (!dbg->wrapDebuggeeValue(cx, &globalValue))
            return false;
        if (!NewbornArrayPush(cx, result, globalValue))
            return false;
    }

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
Debugger::makeGlobalObjectReference(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "makeGlobalObjectReference", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.makeGlobalObjectReference", 1))
        return false;

    Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
    if (!global)
        return false;

    // If we create a D.O referring to a global in an invisible compartment,
    // then from it we can reach function objects, scripts, environments, etc.,
    // none of which we're ever supposed to see.
    JSCompartment* globalCompartment = global->compartment();
    if (globalCompartment->creationOptions().invisibleToDebugger()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_INVISIBLE_COMPARTMENT);
        return false;
    }

    args.rval().setObject(*global);
    return dbg->wrapDebuggeeValue(cx, args.rval());
}

bool
Debugger::isCompilableUnit(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.requireAtLeast(cx, "Debugger.isCompilableUnit", 1))
        return false;

    if (!args[0].isString()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "Debugger.isCompilableUnit", "string",
                                  InformalValueTypeName(args[0]));
        return false;
    }

    JSString* str = args[0].toString();
    size_t length = GetStringLength(str);

    AutoStableStringChars chars(cx);
    if (!chars.initTwoByte(cx, str))
        return false;

    bool result = true;

    CompileOptions options(cx);
    frontend::UsedNameTracker usedNames(cx);
    if (!usedNames.init())
        return false;
    frontend::Parser<frontend::FullParseHandler, char16_t> parser(cx, cx->tempLifoAlloc(),
                                                                  options, chars.twoByteChars(),
                                                                  length,
                                                                  /* foldConstants = */ true,
                                                                  usedNames, nullptr, nullptr);
    JS::WarningReporter older = JS::SetWarningReporter(cx, nullptr);
    if (!parser.checkOptions() || !parser.parse()) {
        // We ran into an error. If it was because we ran out of memory we report
        // it in the usual way.
        if (cx->isThrowingOutOfMemory()) {
            JS::SetWarningReporter(cx, older);
            return false;
        }

        // If it was because we ran out of source, we return false so our caller
        // knows to try to collect more [source].
        if (parser.isUnexpectedEOF())
            result = false;

        cx->clearPendingException();
    }
    JS::SetWarningReporter(cx, older);
    args.rval().setBoolean(result);
    return true;
}

bool
Debugger::adoptDebuggeeValue(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER(cx, argc, vp, "adoptDebuggeeValue", args, dbg);
    if (!args.requireAtLeast(cx, "Debugger.adoptDebuggeeValue", 1))
        return false;

    RootedValue v(cx, args[0]);
    if (v.isObject()) {
        RootedObject obj(cx, &v.toObject());
        NativeObject* ndobj = ToNativeDebuggerObject(cx, &obj);
        if (!ndobj) {
            return false;
        }

        obj.set(static_cast<JSObject*>(ndobj->getPrivate()));
        v = ObjectValue(*obj);

        if (!dbg->wrapDebuggeeValue(cx, &v)) {
            return false;
        }
    }

    args.rval().set(v);
    return true;
}

const JSPropertySpec Debugger::properties[] = {
    JS_PSGS("enabled", Debugger::getEnabled, Debugger::setEnabled, 0),
    JS_PSGS("onDebuggerStatement", Debugger::getOnDebuggerStatement,
            Debugger::setOnDebuggerStatement, 0),
    JS_PSGS("onExceptionUnwind", Debugger::getOnExceptionUnwind,
            Debugger::setOnExceptionUnwind, 0),
    JS_PSGS("onNewScript", Debugger::getOnNewScript, Debugger::setOnNewScript, 0),
    JS_PSGS("onNewPromise", Debugger::getOnNewPromise, Debugger::setOnNewPromise, 0),
    JS_PSGS("onPromiseSettled", Debugger::getOnPromiseSettled, Debugger::setOnPromiseSettled, 0),
    JS_PSGS("onEnterFrame", Debugger::getOnEnterFrame, Debugger::setOnEnterFrame, 0),
    JS_PSGS("onNewGlobalObject", Debugger::getOnNewGlobalObject, Debugger::setOnNewGlobalObject, 0),
    JS_PSGS("uncaughtExceptionHook", Debugger::getUncaughtExceptionHook,
            Debugger::setUncaughtExceptionHook, 0),
    JS_PSGS("allowUnobservedAsmJS", Debugger::getAllowUnobservedAsmJS,
            Debugger::setAllowUnobservedAsmJS, 0),
    JS_PSGS("allowWasmBinarySource", Debugger::getAllowWasmBinarySource,
            Debugger::setAllowWasmBinarySource, 0),
    JS_PSGS("collectCoverageInfo", Debugger::getCollectCoverageInfo,
            Debugger::setCollectCoverageInfo, 0),
    JS_PSG("memory", Debugger::getMemory, 0),
    JS_PS_END
};

const JSFunctionSpec Debugger::methods[] = {
    JS_FN("addDebuggee", Debugger::addDebuggee, 1, 0),
    JS_FN("addAllGlobalsAsDebuggees", Debugger::addAllGlobalsAsDebuggees, 0, 0),
    JS_FN("removeDebuggee", Debugger::removeDebuggee, 1, 0),
    JS_FN("removeAllDebuggees", Debugger::removeAllDebuggees, 0, 0),
    JS_FN("hasDebuggee", Debugger::hasDebuggee, 1, 0),
    JS_FN("getDebuggees", Debugger::getDebuggees, 0, 0),
    JS_FN("getNewestFrame", Debugger::getNewestFrame, 0, 0),
    JS_FN("clearAllBreakpoints", Debugger::clearAllBreakpoints, 0, 0),
    JS_FN("findScripts", Debugger::findScripts, 1, 0),
    JS_FN("findObjects", Debugger::findObjects, 1, 0),
    JS_FN("findAllGlobals", Debugger::findAllGlobals, 0, 0),
    JS_FN("makeGlobalObjectReference", Debugger::makeGlobalObjectReference, 1, 0),
    JS_FN("adoptDebuggeeValue", Debugger::adoptDebuggeeValue, 1, 0),
    JS_FS_END
};

const JSFunctionSpec Debugger::static_methods[] {
    JS_FN("isCompilableUnit", Debugger::isCompilableUnit, 1, 0),
    JS_FS_END
};

/*** Debugger.Script *****************************************************************************/

// Get the Debugger.Script referent as bare Cell. This should only be used for
// GC operations like tracing. Please use GetScriptReferent below.
static inline gc::Cell*
GetScriptReferentCell(JSObject* obj)
{
    MOZ_ASSERT(obj->getClass() == &DebuggerScript_class);
    return static_cast<gc::Cell*>(obj->as<NativeObject>().getPrivate());
}

static inline DebuggerScriptReferent
GetScriptReferent(JSObject* obj)
{
    MOZ_ASSERT(obj->getClass() == &DebuggerScript_class);
    if (gc::Cell* cell = GetScriptReferentCell(obj)) {
        if (cell->is<JSScript>())
            return AsVariant(cell->as<JSScript>());
        MOZ_ASSERT(cell->is<JSObject>());
        return AsVariant(&static_cast<NativeObject*>(cell)->as<WasmInstanceObject>());
    }
    return AsVariant(static_cast<JSScript*>(nullptr));
}

void
DebuggerScript_trace(JSTracer* trc, JSObject* obj)
{
    /* This comes from a private pointer, so no barrier needed. */
    gc::Cell* cell = GetScriptReferentCell(obj);
    if (cell) {
        if (cell->is<JSScript>()) {
            JSScript* script = cell->as<JSScript>();
            TraceManuallyBarrieredCrossCompartmentEdge(trc, obj, &script,
                                                       "Debugger.Script script referent");
            obj->as<NativeObject>().setPrivateUnbarriered(script);
        } else {
            JSObject* wasm = cell->as<JSObject>();
            TraceManuallyBarrieredCrossCompartmentEdge(trc, obj, &wasm,
                                                       "Debugger.Script wasm referent");
            MOZ_ASSERT(wasm->is<WasmInstanceObject>());
            obj->as<NativeObject>().setPrivateUnbarriered(wasm);
        }
    }
}

class DebuggerScriptSetPrivateMatcher
{
    NativeObject* obj_;
  public:
    explicit DebuggerScriptSetPrivateMatcher(NativeObject* obj) : obj_(obj) { }
    using ReturnType = void;
    ReturnType match(HandleScript script) { obj_->setPrivateGCThing(script); }
    ReturnType match(Handle<WasmInstanceObject*> instance) { obj_->setPrivateGCThing(instance); }
};

NativeObject*
Debugger::newDebuggerScript(JSContext* cx, Handle<DebuggerScriptReferent> referent)
{
    assertSameCompartment(cx, object.get());

    RootedObject proto(cx, &object->getReservedSlot(JSSLOT_DEBUG_SCRIPT_PROTO).toObject());
    MOZ_ASSERT(proto);
    NativeObject* scriptobj = NewNativeObjectWithGivenProto(cx, &DebuggerScript_class,
                                                            proto, TenuredObject);
    if (!scriptobj)
        return nullptr;
    scriptobj->setReservedSlot(JSSLOT_DEBUGSCRIPT_OWNER, ObjectValue(*object));
    DebuggerScriptSetPrivateMatcher matcher(scriptobj);
    referent.match(matcher);

    return scriptobj;
}

template <typename ReferentVariant, typename Referent, typename Map>
JSObject*
Debugger::wrapVariantReferent(JSContext* cx, Map& map, Handle<CrossCompartmentKey> key,
                              Handle<ReferentVariant> referent)
{
    assertSameCompartment(cx, object);

    Handle<Referent> untaggedReferent = referent.template as<Referent>();
    MOZ_ASSERT(cx->compartment() != untaggedReferent->compartment());

    DependentAddPtr<Map> p(cx, map, untaggedReferent);
    if (!p) {
        NativeObject* wrapper = newVariantWrapper(cx, referent);
        if (!wrapper)
            return nullptr;

        if (!p.add(cx, map, untaggedReferent, wrapper)) {
            NukeDebuggerWrapper(wrapper);
            return nullptr;
        }

        if (!object->compartment()->putWrapper(cx, key, ObjectValue(*wrapper))) {
            NukeDebuggerWrapper(wrapper);
            map.remove(untaggedReferent);
            ReportOutOfMemory(cx);
            return nullptr;
        }

    }

    return p->value();
}

JSObject*
Debugger::wrapVariantReferent(JSContext* cx, Handle<DebuggerScriptReferent> referent)
{
    JSObject* obj;
    if (referent.is<JSScript*>()) {
        Handle<JSScript*> untaggedReferent = referent.template as<JSScript*>();
        Rooted<CrossCompartmentKey> key(cx, CrossCompartmentKey(object, untaggedReferent));
        obj = wrapVariantReferent<DebuggerScriptReferent, JSScript*, ScriptWeakMap>(
            cx, scripts, key, referent);
    } else {
        Handle<WasmInstanceObject*> untaggedReferent = referent.template as<WasmInstanceObject*>();
        Rooted<CrossCompartmentKey> key(cx, CrossCompartmentKey(object, untaggedReferent,
                                        CrossCompartmentKey::DebuggerObjectKind::DebuggerWasmScript));
        obj = wrapVariantReferent<DebuggerScriptReferent, WasmInstanceObject*, WasmInstanceWeakMap>(
            cx, wasmInstanceScripts, key, referent);
    }
    MOZ_ASSERT_IF(obj, GetScriptReferent(obj) == referent);
    return obj;
}

JSObject*
Debugger::wrapScript(JSContext* cx, HandleScript script)
{
    Rooted<DebuggerScriptReferent> referent(cx, script.get());
    return wrapVariantReferent(cx, referent);
}

JSObject*
Debugger::wrapWasmScript(JSContext* cx, Handle<WasmInstanceObject*> wasmInstance)
{
    Rooted<DebuggerScriptReferent> referent(cx, wasmInstance.get());
    return wrapVariantReferent(cx, referent);
}

static JSObject*
DebuggerScript_check(JSContext* cx, const Value& v, const char* fnname)
{
    JSObject* thisobj = NonNullObject(cx, v);
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &DebuggerScript_class) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Script", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    /*
     * Check for Debugger.Script.prototype, which is of class DebuggerScript_class
     * but whose script is null.
     */
    if (!GetScriptReferentCell(thisobj)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Script", fnname, "prototype object");
        return nullptr;
    }

    return thisobj;
}

template <typename ReferentT>
static JSObject*
DebuggerScript_checkThis(JSContext* cx, const CallArgs& args, const char* fnname,
                         const char* refname)
{
    JSObject* thisobj = DebuggerScript_check(cx, args.thisv(), fnname);
    if (!thisobj)
        return nullptr;

    if (!GetScriptReferent(thisobj).is<ReferentT>()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                              JSDVG_SEARCH_STACK, args.thisv(), nullptr,
                              refname, nullptr);
        return nullptr;
    }

    return thisobj;
}

#define THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, fnname, args, obj, referent)        \
    CallArgs args = CallArgsFromVp(argc, vp);                                       \
    RootedObject obj(cx, DebuggerScript_check(cx, args.thisv(), fnname));           \
    if (!obj)                                                                       \
        return false;                                                               \
    Rooted<DebuggerScriptReferent> referent(cx, GetScriptReferent(obj))

#define THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, fnname, args, obj, script)            \
    CallArgs args = CallArgsFromVp(argc, vp);                                       \
    RootedObject obj(cx, DebuggerScript_checkThis<JSScript*>(cx, args, fnname,      \
                                                             "a JS script"));       \
    if (!obj)                                                                       \
        return false;                                                               \
    RootedScript script(cx, GetScriptReferent(obj).as<JSScript*>())

static bool
DebuggerScript_getIsGeneratorFunction(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get isGeneratorFunction)", args, obj, script);
    args.rval().setBoolean(script->isGenerator());
    return true;
}

static bool
DebuggerScript_getIsAsyncFunction(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get isAsyncFunction)", args, obj, script);
    args.rval().setBoolean(script->isAsync());
    return true;
}

static bool
DebuggerScript_getDisplayName(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get displayName)", args, obj, script);
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    JSFunction* func = script->functionNonDelazifying();
    JSString* name = func ? func->displayAtom() : nullptr;
    if (!name) {
        args.rval().setUndefined();
        return true;
    }

    RootedValue namev(cx, StringValue(name));
    if (!dbg->wrapDebuggeeValue(cx, &namev))
        return false;
    args.rval().set(namev);
    return true;
}

static bool
DebuggerScript_getUrl(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get url)", args, obj, script);

    if (script->filename()) {
        JSString* str;
        if (script->scriptSource()->introducerFilename())
            str = NewStringCopyZ<CanGC>(cx, script->scriptSource()->introducerFilename());
        else
            str = NewStringCopyZ<CanGC>(cx, script->filename());
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setNull();
    }
    return true;
}

struct DebuggerScriptGetStartLineMatcher
{
    using ReturnType = uint32_t;

    ReturnType match(HandleScript script) {
        return uint32_t(script->lineno());
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return 1;
    }
};

static bool
DebuggerScript_getStartLine(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "(get startLine)", args, obj, referent);
    DebuggerScriptGetStartLineMatcher matcher;
    args.rval().setNumber(referent.match(matcher));
    return true;
}

struct DebuggerScriptGetLineCountMatcher
{
    JSContext* cx_;
    double totalLines;

    explicit DebuggerScriptGetLineCountMatcher(JSContext* cx) : cx_(cx) {}
    using ReturnType = bool;

    ReturnType match(HandleScript script) {
        totalLines = double(GetScriptLineExtent(script));
        return true;
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        uint32_t result;
        if (!wasmInstance->instance().debug().totalSourceLines(cx_, &result))
            return false;
        totalLines = double(result);
        return true;
    }
};

static bool
DebuggerScript_getLineCount(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "(get lineCount)", args, obj, referent);
    DebuggerScriptGetLineCountMatcher matcher(cx);
    if (!referent.match(matcher))
        return false;
    args.rval().setNumber(matcher.totalLines);
    return true;
}

class DebuggerScriptGetSourceMatcher
{
    JSContext* cx_;
    Debugger* dbg_;

  public:
    DebuggerScriptGetSourceMatcher(JSContext* cx, Debugger* dbg)
      : cx_(cx), dbg_(dbg)
    { }

    using ReturnType = JSObject*;

    ReturnType match(HandleScript script) {
        RootedScriptSource source(cx_,
            &UncheckedUnwrap(script->sourceObject())->as<ScriptSourceObject>());
        return dbg_->wrapSource(cx_, source);
    }

    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return dbg_->wrapWasmSource(cx_, wasmInstance);
    }
};

static bool
DebuggerScript_getSource(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "(get source)", args, obj, referent);
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    DebuggerScriptGetSourceMatcher matcher(cx, dbg);
    RootedObject sourceObject(cx, referent.match(matcher));
    if (!sourceObject)
        return false;

    args.rval().setObject(*sourceObject);
    return true;
}

static bool
DebuggerScript_getSourceStart(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get sourceStart)", args, obj, script);
    args.rval().setNumber(uint32_t(script->sourceStart()));
    return true;
}

static bool
DebuggerScript_getSourceLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get sourceEnd)", args, obj, script);
    args.rval().setNumber(uint32_t(script->sourceEnd() - script->sourceStart()));
    return true;
}

static bool
DebuggerScript_getGlobal(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get global)", args, obj, script);
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    RootedValue v(cx, ObjectValue(script->global()));
    if (!dbg->wrapDebuggeeValue(cx, &v))
        return false;
    args.rval().set(v);
    return true;
}

class DebuggerScriptGetFormatMatcher
{
    const JSAtomState& names_;
  public:
    explicit DebuggerScriptGetFormatMatcher(const JSAtomState& names) : names_(names) { }
    using ReturnType = JSAtom*;
    ReturnType match(HandleScript script) { return names_.js; }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) { return names_.wasm; }
};

static bool
DebuggerScript_getFormat(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "(get format)", args, obj, referent);
    DebuggerScriptGetFormatMatcher matcher(cx->names());
    args.rval().setString(referent.match(matcher));
    return true;
}

static bool
DebuggerScript_getChildScripts(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getChildScripts", args, obj, script);
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    if (script->hasObjects()) {
        /*
         * script->savedCallerFun indicates that this is a direct eval script
         * and the calling function is stored as script->objects()->vector[0].
         * It is not really a child script of this script, so skip it using
         * innerObjectsStart().
         */
        ObjectArray* objects = script->objects();
        RootedFunction fun(cx);
        RootedScript funScript(cx);
        RootedObject obj(cx), s(cx);
        for (uint32_t i = 0; i < objects->length; i++) {
            obj = objects->vector[i];
            if (obj->is<JSFunction>()) {
                fun = &obj->as<JSFunction>();
                // The inner function could be a wasm native.
                if (fun->isNative())
                    continue;
                funScript = GetOrCreateFunctionScript(cx, fun);
                if (!funScript)
                    return false;
                s = dbg->wrapScript(cx, funScript);
                if (!s || !NewbornArrayPush(cx, result, ObjectValue(*s)))
                    return false;
            }
        }
    }
    args.rval().setObject(*result);
    return true;
}

static bool
ScriptOffset(JSContext* cx, const Value& v, size_t* offsetp)
{
    double d;
    size_t off;

    bool ok = v.isNumber();
    if (ok) {
        d = v.toNumber();
        off = size_t(d);
    }
    if (!ok || off != d) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_OFFSET);
        return false;
    }
    *offsetp = off;
    return true;
}

static bool
EnsureScriptOffsetIsValid(JSContext* cx, JSScript* script, size_t offset)
{
    if (IsValidBytecodeOffset(cx, script, offset))
        return true;
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_OFFSET);
    return false;
}

namespace {

class BytecodeRangeWithPosition : private BytecodeRange
{
  public:
    using BytecodeRange::empty;
    using BytecodeRange::frontPC;
    using BytecodeRange::frontOpcode;
    using BytecodeRange::frontOffset;

    BytecodeRangeWithPosition(JSContext* cx, JSScript* script)
      : BytecodeRange(cx, script), lineno(script->lineno()), column(0),
        sn(script->notes()), snpc(script->code()), isEntryPoint(false),
        wasArtifactEntryPoint(false)
    {
        if (!SN_IS_TERMINATOR(sn))
            snpc += SN_DELTA(sn);
        updatePosition();
        while (frontPC() != script->main())
            popFront();

        if (frontOpcode() != JSOP_JUMPTARGET)
            isEntryPoint = true;
        else
            wasArtifactEntryPoint =  true;
    }

    void popFront() {
        BytecodeRange::popFront();
        if (empty())
            isEntryPoint = false;
        else
            updatePosition();

        // The following conditions are handling artifacts introduced by the
        // bytecode emitter, such that we do not add breakpoints on empty
        // statements of the source code of the user.
        if (wasArtifactEntryPoint) {
            wasArtifactEntryPoint = false;
            isEntryPoint = true;
        }

        if (isEntryPoint && frontOpcode() == JSOP_JUMPTARGET) {
            wasArtifactEntryPoint = isEntryPoint;
            isEntryPoint = false;
        }
    }

    size_t frontLineNumber() const { return lineno; }
    size_t frontColumnNumber() const { return column; }

    // Entry points are restricted to bytecode offsets that have an
    // explicit mention in the line table.  This restriction avoids a
    // number of failing cases caused by some instructions not having
    // sensible (to the user) line numbers, and it is one way to
    // implement the idea that the bytecode emitter should tell the
    // debugger exactly which offsets represent "interesting" (to the
    // user) places to stop.
    bool frontIsEntryPoint() const { return isEntryPoint; }

  private:
    void updatePosition() {
        /*
         * Determine the current line number by reading all source notes up to
         * and including the current offset.
         */
        jsbytecode *lastLinePC = nullptr;
        while (!SN_IS_TERMINATOR(sn) && snpc <= frontPC()) {
            SrcNoteType type = SN_TYPE(sn);
            if (type == SRC_COLSPAN) {
                ptrdiff_t colspan = SN_OFFSET_TO_COLSPAN(GetSrcNoteOffset(sn, 0));
                MOZ_ASSERT(ptrdiff_t(column) + colspan >= 0);
                column += colspan;
                lastLinePC = snpc;
            } else if (type == SRC_SETLINE) {
                lineno = size_t(GetSrcNoteOffset(sn, 0));
                column = 0;
                lastLinePC = snpc;
            } else if (type == SRC_NEWLINE) {
                lineno++;
                column = 0;
                lastLinePC = snpc;
            }

            sn = SN_NEXT(sn);
            snpc += SN_DELTA(sn);
        }
        isEntryPoint = lastLinePC == frontPC();
    }

    size_t lineno;
    size_t column;
    jssrcnote* sn;
    jsbytecode* snpc;
    bool isEntryPoint;
    bool wasArtifactEntryPoint;
};

/*
 * FlowGraphSummary::populate(cx, script) computes a summary of script's
 * control flow graph used by DebuggerScript_{getAllOffsets,getLineOffsets}.
 *
 * An instruction on a given line is an entry point for that line if it can be
 * reached from (an instruction on) a different line. We distinguish between the
 * following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the line it is on.
 *   - hasSingleEdge:
 *       The instruction can be reached from a single line. If this line is
 *       different from the line the instruction is on, the instruction is an
 *       entry point for that line.
 *
 * Similarly, an instruction on a given position (line/column pair) is an
 * entry point for that position if it can be reached from (an instruction on) a
 * different position. Again, we distinguish between the following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the position it is on.
 *   - hasSingleEdge:
 *       The instruction can be reached from a single position. If this line is
 *       different from the position the instruction is on, the instruction is
 *       an entry point for that position.
 */
class FlowGraphSummary {
  public:
    class Entry {
      public:
        static Entry createWithSingleEdge(size_t lineno, size_t column) {
            return Entry(lineno, column);
        }

        static Entry createWithMultipleEdgesFromSingleLine(size_t lineno) {
            return Entry(lineno, SIZE_MAX);
        }

        static Entry createWithMultipleEdgesFromMultipleLines() {
            return Entry(SIZE_MAX, SIZE_MAX);
        }

        Entry() : lineno_(SIZE_MAX), column_(0) {}

        bool hasNoEdges() const {
            return lineno_ == SIZE_MAX && column_ != SIZE_MAX;
        }

        bool hasSingleEdge() const {
            return lineno_ != SIZE_MAX && column_ != SIZE_MAX;
        }

        size_t lineno() const {
            return lineno_;
        }

        size_t column() const {
            return column_;
        }

      private:
        Entry(size_t lineno, size_t column) : lineno_(lineno), column_(column) {}

        size_t lineno_;
        size_t column_;
    };

    explicit FlowGraphSummary(JSContext* cx) : entries_(cx) {}

    Entry& operator[](size_t index) {
        return entries_[index];
    }

    bool populate(JSContext* cx, JSScript* script) {
        if (!entries_.growBy(script->length()))
            return false;
        unsigned mainOffset = script->pcToOffset(script->main());
        entries_[mainOffset] = Entry::createWithMultipleEdgesFromMultipleLines();

        size_t prevLineno = script->lineno();
        size_t prevColumn = 0;
        JSOp prevOp = JSOP_NOP;
        for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
            size_t lineno = prevLineno;
            size_t column = prevColumn;
            JSOp op = r.frontOpcode();

            if (FlowsIntoNext(prevOp))
                addEdge(prevLineno, prevColumn, r.frontOffset());

            // If we visit the branch target before we visit the
            // branch op itself, just reuse the previous location.
            // This is reasonable for the time being because this
            // situation can currently only arise from loop heads,
            // where this assumption holds.
            if (BytecodeIsJumpTarget(op) && !entries_[r.frontOffset()].hasNoEdges()) {
                lineno = entries_[r.frontOffset()].lineno();
                column = entries_[r.frontOffset()].column();
            }

            if (r.frontIsEntryPoint()) {
                lineno = r.frontLineNumber();
                column = r.frontColumnNumber();
            }

            if (CodeSpec[op].type() == JOF_JUMP) {
                addEdge(lineno, column, r.frontOffset() + GET_JUMP_OFFSET(r.frontPC()));
            } else if (op == JSOP_TABLESWITCH) {
                jsbytecode* pc = r.frontPC();
                size_t offset = r.frontOffset();
                ptrdiff_t step = JUMP_OFFSET_LEN;
                size_t defaultOffset = offset + GET_JUMP_OFFSET(pc);
                pc += step;
                addEdge(lineno, column, defaultOffset);

                int32_t low = GET_JUMP_OFFSET(pc);
                pc += JUMP_OFFSET_LEN;
                int ncases = GET_JUMP_OFFSET(pc) - low + 1;
                pc += JUMP_OFFSET_LEN;

                for (int i = 0; i < ncases; i++) {
                    size_t target = offset + GET_JUMP_OFFSET(pc);
                    addEdge(lineno, column, target);
                    pc += step;
                }
            } else if (op == JSOP_TRY) {
                // As there is no literal incoming edge into the catch block, we
                // make a fake one by copying the JSOP_TRY location, as-if this
                // was an incoming edge of the catch block. This is needed
                // because we only report offsets of entry points which have
                // valid incoming edges.
                JSTryNote* tn = script->trynotes()->vector;
                JSTryNote* tnlimit = tn + script->trynotes()->length;
                for (; tn < tnlimit; tn++) {
                    uint32_t startOffset = script->mainOffset() + tn->start;
                    if (startOffset == r.frontOffset() + 1) {
                        uint32_t catchOffset = startOffset + tn->length;
                        if (tn->kind == JSTRY_CATCH || tn->kind == JSTRY_FINALLY)
                            addEdge(lineno, column, catchOffset);
                    }
                }
            }

            prevLineno = lineno;
            prevColumn = column;
            prevOp = op;
        }

        return true;
    }

  private:
    void addEdge(size_t sourceLineno, size_t sourceColumn, size_t targetOffset) {
        if (entries_[targetOffset].hasNoEdges())
            entries_[targetOffset] = Entry::createWithSingleEdge(sourceLineno, sourceColumn);
        else if (entries_[targetOffset].lineno() != sourceLineno)
            entries_[targetOffset] = Entry::createWithMultipleEdgesFromMultipleLines();
        else if (entries_[targetOffset].column() != sourceColumn)
            entries_[targetOffset] = Entry::createWithMultipleEdgesFromSingleLine(sourceLineno);
    }

    Vector<Entry> entries_;
};

} /* anonymous namespace */

class DebuggerScriptGetOffsetLocationMatcher
{
    JSContext* cx_;
    size_t offset_;
    MutableHandlePlainObject result_;

  public:
    explicit DebuggerScriptGetOffsetLocationMatcher(JSContext* cx, size_t offset,
                                                    MutableHandlePlainObject result)
      : cx_(cx), offset_(offset), result_(result) { }
    using ReturnType = bool;
    ReturnType match(HandleScript script) {
        if (!EnsureScriptOffsetIsValid(cx_, script, offset_))
            return false;

        FlowGraphSummary flowData(cx_);
        if (!flowData.populate(cx_, script))
            return false;

        result_.set(NewBuiltinClassInstance<PlainObject>(cx_));
        if (!result_)
            return false;

        BytecodeRangeWithPosition r(cx_, script);
        while (!r.empty() && r.frontOffset() < offset_)
            r.popFront();

        size_t offset = r.frontOffset();
        bool isEntryPoint = r.frontIsEntryPoint();

        // Line numbers are only correctly defined on entry points. Thus looks
        // either for the next valid offset in the flowData, being the last entry
        // point flowing into the current offset, or for the next valid entry point.
        while (!r.frontIsEntryPoint() && !flowData[r.frontOffset()].hasSingleEdge()) {
            r.popFront();
            MOZ_ASSERT(!r.empty());
        }

        // If this is an entry point, take the line number associated with the entry
        // point, otherwise settle on the next instruction and take the incoming
        // edge position.
        size_t lineno;
        size_t column;
        if (r.frontIsEntryPoint()) {
            lineno = r.frontLineNumber();
            column = r.frontColumnNumber();
        } else {
            MOZ_ASSERT(flowData[r.frontOffset()].hasSingleEdge());
            lineno = flowData[r.frontOffset()].lineno();
            column = flowData[r.frontOffset()].column();
        }

        RootedId id(cx_, NameToId(cx_->names().lineNumber));
        RootedValue value(cx_, NumberValue(lineno));
        if (!DefineDataProperty(cx_, result_, id, value))
            return false;

        value = NumberValue(column);
        if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value))
            return false;

        // The same entry point test that is used by getAllColumnOffsets.
        isEntryPoint = (isEntryPoint &&
                        !flowData[offset].hasNoEdges() &&
                        (flowData[offset].lineno() != r.frontLineNumber() ||
                         flowData[offset].column() != r.frontColumnNumber()));
        value.setBoolean(isEntryPoint);
        if (!DefineDataProperty(cx_, result_, cx_->names().isEntryPoint, value))
            return false;

        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> instance) {
        size_t lineno;
        size_t column;
        bool found;
        if (!instance->instance().debug().getOffsetLocation(cx_, offset_, &found, &lineno, &column))
            return false;

        if (!found) {
            JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_OFFSET);
            return false;
        }

        result_.set(NewBuiltinClassInstance<PlainObject>(cx_));
        if (!result_)
            return false;

        RootedId id(cx_, NameToId(cx_->names().lineNumber));
        RootedValue value(cx_, NumberValue(lineno));
        if (!DefineDataProperty(cx_, result_, id, value))
            return false;

        value = NumberValue(column);
        if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value))
            return false;

        value.setBoolean(true);
        if (!DefineDataProperty(cx_, result_, cx_->names().isEntryPoint, value))
            return false;

        return true;
    }
};

static bool
DebuggerScript_getOffsetLocation(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "getOffsetLocation", args, obj, referent);
    if (!args.requireAtLeast(cx, "Debugger.Script.getOffsetLocation", 1))
        return false;
    size_t offset;
    if (!ScriptOffset(cx, args[0], &offset))
        return false;

    RootedPlainObject result(cx);
    DebuggerScriptGetOffsetLocationMatcher matcher(cx, offset, &result);
    if (!referent.match(matcher))
        return false;

    args.rval().setObject(*result);
    return true;
}

static bool
DebuggerScript_getAllOffsets(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getAllOffsets", args, obj, script);

    /*
     * First pass: determine which offsets in this script are jump targets and
     * which line numbers jump to them.
     */
    FlowGraphSummary flowData(cx);
    if (!flowData.populate(cx, script))
        return false;

    /* Second pass: build the result array. */
    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
        if (!r.frontIsEntryPoint())
            continue;

        size_t offset = r.frontOffset();
        size_t lineno = r.frontLineNumber();

        /* Make a note, if the current instruction is an entry point for the current line. */
        if (!flowData[offset].hasNoEdges() && flowData[offset].lineno() != lineno) {
            /* Get the offsets array for this line. */
            RootedObject offsets(cx);
            RootedValue offsetsv(cx);

            RootedId id(cx, INT_TO_JSID(lineno));

            bool found;
            if (!HasOwnProperty(cx, result, id, &found))
                return false;
            if (found && !GetProperty(cx, result, result, id, &offsetsv))
                return false;

            if (offsetsv.isObject()) {
                offsets = &offsetsv.toObject();
            } else {
                MOZ_ASSERT(offsetsv.isUndefined());

                /*
                 * Create an empty offsets array for this line.
                 * Store it in the result array.
                 */
                RootedId id(cx);
                RootedValue v(cx, NumberValue(lineno));
                offsets = NewDenseEmptyArray(cx);
                if (!offsets ||
                    !ValueToId<CanGC>(cx, v, &id))
                {
                    return false;
                }

                RootedValue value(cx, ObjectValue(*offsets));
                if (!DefineDataProperty(cx, result, id, value))
                    return false;
            }

            /* Append the current offset to the offsets array. */
            if (!NewbornArrayPush(cx, offsets, NumberValue(offset)))
                return false;
        }
    }

    args.rval().setObject(*result);
    return true;
}

class DebuggerScriptGetAllColumnOffsetsMatcher
{
    JSContext* cx_;
    MutableHandleObject result_;

    bool appendColumnOffsetEntry(size_t lineno, size_t column, size_t offset) {
        RootedPlainObject entry(cx_, NewBuiltinClassInstance<PlainObject>(cx_));
        if (!entry)
            return false;

        RootedId id(cx_, NameToId(cx_->names().lineNumber));
        RootedValue value(cx_, NumberValue(lineno));
        if (!DefineDataProperty(cx_, entry, id, value))
            return false;

        value = NumberValue(column);
        if (!DefineDataProperty(cx_, entry, cx_->names().columnNumber, value))
            return false;

        id = NameToId(cx_->names().offset);
        value = NumberValue(offset);
        if (!DefineDataProperty(cx_, entry, id, value))
            return false;

        return NewbornArrayPush(cx_, result_, ObjectValue(*entry));
    }

  public:
    explicit DebuggerScriptGetAllColumnOffsetsMatcher(JSContext* cx, MutableHandleObject result)
      : cx_(cx), result_(result) { }
    using ReturnType = bool;
    ReturnType match(HandleScript script) {
        /*
         * First pass: determine which offsets in this script are jump targets
         * and which positions jump to them.
         */
        FlowGraphSummary flowData(cx_);
        if (!flowData.populate(cx_, script))
            return false;

        /* Second pass: build the result array. */
        result_.set(NewDenseEmptyArray(cx_));
        if (!result_)
            return false;

        for (BytecodeRangeWithPosition r(cx_, script); !r.empty(); r.popFront()) {
            size_t lineno = r.frontLineNumber();
            size_t column = r.frontColumnNumber();
            size_t offset = r.frontOffset();

            /*
             * Make a note, if the current instruction is an entry point for
             * the current position.
             */
            if (r.frontIsEntryPoint() &&
                !flowData[offset].hasNoEdges() &&
                (flowData[offset].lineno() != lineno ||
                 flowData[offset].column() != column)) {
                if (!appendColumnOffsetEntry(lineno, column, offset))
                    return false;
            }
        }
        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> instance) {
        Vector<wasm::ExprLoc> offsets(cx_);
        if (!instance->instance().debug().getAllColumnOffsets(cx_, &offsets))
            return false;

        result_.set(NewDenseEmptyArray(cx_));
        if (!result_)
            return false;

        for (uint32_t i = 0; i < offsets.length(); i++) {
            size_t lineno = offsets[i].lineno;
            size_t column = offsets[i].column;
            size_t offset = offsets[i].offset;
            if (!appendColumnOffsetEntry(lineno, column, offset))
                return false;
        }
        return true;
    }
};

static bool
DebuggerScript_getAllColumnOffsets(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "getAllColumnOffsets", args, obj, referent);

    RootedObject result(cx);
    DebuggerScriptGetAllColumnOffsetsMatcher matcher(cx, &result);
    if (!referent.match(matcher))
        return false;

    args.rval().setObject(*result);
    return true;
}

class DebuggerScriptGetLineOffsetsMatcher
{
    JSContext* cx_;
    size_t lineno_;
    MutableHandleObject result_;

  public:
    explicit DebuggerScriptGetLineOffsetsMatcher(JSContext* cx, size_t lineno, MutableHandleObject result)
      : cx_(cx), lineno_(lineno), result_(result) { }
    using ReturnType = bool;
    ReturnType match(HandleScript script) {
        /*
         * First pass: determine which offsets in this script are jump targets and
         * which line numbers jump to them.
         */
        FlowGraphSummary flowData(cx_);
        if (!flowData.populate(cx_, script))
            return false;

        result_.set(NewDenseEmptyArray(cx_));
        if (!result_)
            return false;

        /* Second pass: build the result array. */
        for (BytecodeRangeWithPosition r(cx_, script); !r.empty(); r.popFront()) {
            if (!r.frontIsEntryPoint())
                continue;

            size_t offset = r.frontOffset();

            /* If the op at offset is an entry point, append offset to result. */
            if (r.frontLineNumber() == lineno_ &&
                !flowData[offset].hasNoEdges() &&
                flowData[offset].lineno() != lineno_)
            {
                if (!NewbornArrayPush(cx_, result_, NumberValue(offset)))
                    return false;
            }
        }

        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> instance) {
        Vector<uint32_t> offsets(cx_);
        if (!instance->instance().debug().getLineOffsets(cx_, lineno_, &offsets))
            return false;

        result_.set(NewDenseEmptyArray(cx_));
        if (!result_)
            return false;

        for (uint32_t i = 0; i < offsets.length(); i++) {
            if (!NewbornArrayPush(cx_, result_, NumberValue(offsets[i])))
                return false;
        }
        return true;
    }
};

static bool
DebuggerScript_getLineOffsets(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "getLineOffsets", args, obj, referent);
    if (!args.requireAtLeast(cx, "Debugger.Script.getLineOffsets", 1))
        return false;

    /* Parse lineno argument. */
    RootedValue linenoValue(cx, args[0]);
    size_t lineno;
    if (!ToNumber(cx, &linenoValue))
        return false;
    {
        double d = linenoValue.toNumber();
        lineno = size_t(d);
        if (lineno != d) {
            JS_ReportErrorNumberASCII(cx,  GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_LINE);
            return false;
        }
    }

    RootedObject result(cx);
    DebuggerScriptGetLineOffsetsMatcher matcher(cx, lineno, &result);
    if (!referent.match(matcher))
        return false;

    args.rval().setObject(*result);
    return true;
}

bool
Debugger::observesFrame(AbstractFramePtr frame) const
{
    if (frame.isWasmDebugFrame())
        return observesWasm(frame.wasmInstance());

    return observesScript(frame.script());
}

bool
Debugger::observesFrame(const FrameIter& iter) const
{
    // Skip frames not yet fully initialized during their prologue.
    if (iter.isInterp() && iter.isFunctionFrame()) {
        const Value& thisVal = iter.interpFrame()->thisArgument();
        if (thisVal.isMagic() && thisVal.whyMagic() == JS_IS_CONSTRUCTING)
            return false;
    }
    if (iter.isWasm()) {
        // Skip frame of wasm instances we cannot observe.
        if (!iter.wasmDebugEnabled())
            return false;
        return observesWasm(iter.wasmInstance());
    }
    return observesScript(iter.script());
}

bool
Debugger::observesScript(JSScript* script) const
{
    if (!enabled)
        return false;
    // Don't ever observe self-hosted scripts: the Debugger API can break
    // self-hosted invariants.
    return observesGlobal(&script->global()) && !script->selfHosted();
}

bool
Debugger::observesWasm(wasm::Instance* instance) const
{
    if (!enabled || !instance->debugEnabled())
        return false;
    return observesGlobal(&instance->object()->global());
}

/* static */ bool
Debugger::replaceFrameGuts(JSContext* cx, AbstractFramePtr from, AbstractFramePtr to,
                           ScriptFrameIter& iter)
{
    auto removeFromDebuggerFramesOnExit = MakeScopeExit([&] {
        // Remove any remaining old entries on exit, as the 'from' frame will
        // be gone. This is only done in the failure case. On failure, the
        // removeToDebuggerFramesOnExit lambda below will rollback any frames
        // that were replaced, resulting in !frameMaps(to). On success, the
        // range will be empty, as all from Frame.Debugger instances will have
        // been removed.
        MOZ_ASSERT_IF(inFrameMaps(to), !inFrameMaps(from));
        removeFromFrameMapsAndClearBreakpointsIn(cx, from);

        // Rekey missingScopes to maintain Debugger.Environment identity and
        // forward liveScopes to point to the new frame.
        DebugEnvironments::forwardLiveFrame(cx, from, to);
    });

    // Forward live Debugger.Frame objects.
    Rooted<DebuggerFrameVector> frames(cx, DebuggerFrameVector(cx));
    if (!getDebuggerFrames(from, &frames)) {
        // An OOM here means that all Debuggers' frame maps still contain
        // entries for 'from' and no entries for 'to'. Since the 'from' frame
        // will be gone, they are removed by removeFromDebuggerFramesOnExit
        // above.
        return false;
    }

    // If during the loop below we hit an OOM, we must also rollback any of
    // the frames that were successfully replaced. For OSR frames, OOM here
    // means those frames will pop from the OSR trampoline, which does not
    // call Debugger::onLeaveFrame.
    auto removeToDebuggerFramesOnExit = MakeScopeExit([&] {
        removeFromFrameMapsAndClearBreakpointsIn(cx, to);
    });

    for (size_t i = 0; i < frames.length(); i++) {
        HandleDebuggerFrame frameobj = frames[i];
        Debugger* dbg = Debugger::fromChildJSObject(frameobj);

        // Update frame object's ScriptFrameIter::data pointer.
        DebuggerFrame_freeScriptFrameIterData(cx->runtime()->defaultFreeOp(), frameobj);
        ScriptFrameIter::Data* data = iter.copyData();
        if (!data) {
            // An OOM here means that some Debuggers' frame maps may still
            // contain entries for 'from' and some Debuggers' frame maps may
            // also contain entries for 'to'. Thus both
            // removeFromDebuggerFramesOnExit and
            // removeToDebuggerFramesOnExit must both run.
            //
            // The current frameobj in question is still in its Debugger's
            // frame map keyed by 'from', so it will be covered by
            // removeFromDebuggerFramesOnExit.
            return false;
        }
        frameobj->setPrivate(data);

        // Remove old frame.
        dbg->frames.remove(from);

        // Add the frame object with |to| as key.
        if (!dbg->frames.putNew(to, frameobj)) {
            // This OOM is subtle. At this point, both
            // removeFromDebuggerFramesOnExit and removeToDebuggerFramesOnExit
            // must both run for the same reason given above.
            //
            // The difference is that the current frameobj is no longer in its
            // Debugger's frame map, so it will not be cleaned up by neither
            // lambda. Manually clean it up here.
            FreeOp* fop = cx->runtime()->defaultFreeOp();
            DebuggerFrame_freeScriptFrameIterData(fop, frameobj);
            DebuggerFrame_maybeDecrementFrameScriptStepModeCount(fop, to, frameobj);

            ReportOutOfMemory(cx);
            return false;
        }
    }

    // All frames successfuly replaced, cancel the rollback.
    removeToDebuggerFramesOnExit.release();

    return true;
}

/* static */ bool
Debugger::inFrameMaps(AbstractFramePtr frame)
{
    bool foundAny = false;
    forEachDebuggerFrame(frame, [&](NativeObject* frameobj) { foundAny = true; });
    return foundAny;
}

/* static */ void
Debugger::removeFromFrameMapsAndClearBreakpointsIn(JSContext* cx, AbstractFramePtr frame)
{
    forEachDebuggerFrame(frame, [&](NativeObject* frameobj) {
        Debugger* dbg = Debugger::fromChildJSObject(frameobj);

        FreeOp* fop = cx->runtime()->defaultFreeOp();
        DebuggerFrame_freeScriptFrameIterData(fop, frameobj);
        DebuggerFrame_maybeDecrementFrameScriptStepModeCount(fop, frame, frameobj);

        dbg->frames.remove(frame);
    });

    /*
     * If this is an eval frame, then from the debugger's perspective the
     * script is about to be destroyed. Remove any breakpoints in it.
     */
    if (frame.isEvalFrame()) {
        RootedScript script(cx, frame.script());
        script->clearBreakpointsIn(cx->runtime()->defaultFreeOp(), nullptr, nullptr);
    }
}

/* static */ bool
Debugger::handleBaselineOsr(JSContext* cx, InterpreterFrame* from, jit::BaselineFrame* to)
{
    ScriptFrameIter iter(cx);
    MOZ_ASSERT(iter.abstractFramePtr() == to);
    return replaceFrameGuts(cx, from, to, iter);
}

/* static */ bool
Debugger::handleIonBailout(JSContext* cx, jit::RematerializedFrame* from, jit::BaselineFrame* to)
{
    // When we return to a bailed-out Ion real frame, we must update all
    // Debugger.Frames that refer to its inline frames. However, since we
    // can't pop individual inline frames off the stack (we can only pop the
    // real frame that contains them all, as a unit), we cannot assume that
    // the frame we're dealing with is the top frame. Advance the iterator
    // across any inlined frames younger than |to|, the baseline frame
    // reconstructed during bailout from the Ion frame corresponding to
    // |from|.
    ScriptFrameIter iter(cx);
    while (iter.abstractFramePtr() != to)
        ++iter;
    return replaceFrameGuts(cx, from, to, iter);
}

/* static */ void
Debugger::handleUnrecoverableIonBailoutError(JSContext* cx, jit::RematerializedFrame* frame)
{
    // Ion bailout can fail due to overrecursion. In such cases we cannot
    // honor any further Debugger hooks on the frame, and need to ensure that
    // its Debugger.Frame entry is cleaned up.
    removeFromFrameMapsAndClearBreakpointsIn(cx, frame);
}

/* static */ void
Debugger::propagateForcedReturn(JSContext* cx, AbstractFramePtr frame, HandleValue rval)
{
    // Invoking the interrupt handler is considered a step and invokes the
    // youngest frame's onStep handler, if any. However, we cannot handle
    // { return: ... } resumption values straightforwardly from the interrupt
    // handler. Instead, we set the intended return value in the frame's rval
    // slot and set the propagating-forced-return flag on the JSContext.
    //
    // The interrupt handler then returns false with no exception set,
    // signaling an uncatchable exception. In the exception handlers, we then
    // check for the special propagating-forced-return flag.
    MOZ_ASSERT(!cx->isExceptionPending());
    cx->setPropagatingForcedReturn();
    frame.setReturnValue(rval);
}

struct DebuggerScriptSetBreakpointMatcher
{
    JSContext* cx_;
    Debugger* dbg_;
    size_t offset_;
    RootedObject handler_;

  public:
    explicit DebuggerScriptSetBreakpointMatcher(JSContext* cx, Debugger* dbg, size_t offset, HandleObject handler)
      : cx_(cx), dbg_(dbg), offset_(offset), handler_(cx, handler)
    { }

    using ReturnType = bool;

    ReturnType match(HandleScript script) {
        if (!dbg_->observesScript(script)) {
            JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_DEBUGGING);
            return false;
        }

        if (!EnsureScriptOffsetIsValid(cx_, script, offset_))
            return false;

        // Ensure observability *before* setting the breakpoint. If the script is
        // not already a debuggee, trying to ensure observability after setting
        // the breakpoint (and thus marking the script as a debuggee) will skip
        // actually ensuring observability.
        if (!dbg_->ensureExecutionObservabilityOfScript(cx_, script))
            return false;

        jsbytecode* pc = script->offsetToPC(offset_);
        BreakpointSite* site = script->getOrCreateBreakpointSite(cx_, pc);
        if (!site)
            return false;
        site->inc(cx_->runtime()->defaultFreeOp());
        if (cx_->zone()->new_<Breakpoint>(dbg_, site, handler_))
            return true;
        site->dec(cx_->runtime()->defaultFreeOp());
        site->destroyIfEmpty(cx_->runtime()->defaultFreeOp());
        return false;
    }

    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        wasm::Instance& instance = wasmInstance->instance();
        if (!instance.debug().hasBreakpointTrapAtOffset(offset_)) {
            JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr, JSMSG_DEBUG_BAD_OFFSET);
            return false;
        }
        WasmBreakpointSite* site = instance.debug().getOrCreateBreakpointSite(cx_, offset_);
        if (!site)
            return false;
        site->inc(cx_->runtime()->defaultFreeOp());
        if (cx_->zone()->new_<WasmBreakpoint>(dbg_, site, handler_, instance.object()))
            return true;
        site->dec(cx_->runtime()->defaultFreeOp());
        site->destroyIfEmpty(cx_->runtime()->defaultFreeOp());
        return false;
    }
};

static bool
DebuggerScript_setBreakpoint(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "setBreakpoint", args, obj, referent);
    if (!args.requireAtLeast(cx, "Debugger.Script.setBreakpoint", 2))
        return false;
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    size_t offset;
    if (!ScriptOffset(cx, args[0], &offset))
        return false;

    RootedObject handler(cx, NonNullObject(cx, args[1]));
    if (!handler)
        return false;

    DebuggerScriptSetBreakpointMatcher matcher(cx, dbg, offset, handler);
    if (!referent.match(matcher))
        return false;
    args.rval().setUndefined();
    return true;
}

static bool
DebuggerScript_getBreakpoints(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getBreakpoints", args, obj, script);
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    jsbytecode* pc;
    if (args.length() > 0) {
        size_t offset;
        if (!ScriptOffset(cx, args[0], &offset) || !EnsureScriptOffsetIsValid(cx, script, offset))
            return false;
        pc = script->offsetToPC(offset);
    } else {
        pc = nullptr;
    }

    RootedObject arr(cx, NewDenseEmptyArray(cx));
    if (!arr)
        return false;

    for (unsigned i = 0; i < script->length(); i++) {
        BreakpointSite* site = script->getBreakpointSite(script->offsetToPC(i));
        if (!site)
            continue;
        MOZ_ASSERT(site->type() == BreakpointSite::Type::JS);
        if (!pc || site->asJS()->pc == pc) {
            for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = bp->nextInSite()) {
                if (bp->debugger == dbg &&
                    !NewbornArrayPush(cx, arr, ObjectValue(*bp->getHandler())))
                {
                    return false;
                }
            }
        }
    }
    args.rval().setObject(*arr);
    return true;
}

class DebuggerScriptClearBreakpointMatcher
{
    JSContext* cx_;
    Debugger* dbg_;
    JSObject* handler_;

  public:
    explicit DebuggerScriptClearBreakpointMatcher(JSContext* cx, Debugger* dbg, JSObject* handler) : cx_(cx), dbg_(dbg), handler_(handler) { }
    using ReturnType = bool;

    ReturnType match(HandleScript script) {
        script->clearBreakpointsIn(cx_->runtime()->defaultFreeOp(), dbg_, handler_);
        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> instance) {
        return instance->instance().debug().clearBreakpointsIn(cx_, instance, dbg_, handler_);
    }
};


static bool
DebuggerScript_clearBreakpoint(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "clearBreakpoint", args, obj, referent);
    if (!args.requireAtLeast(cx, "Debugger.Script.clearBreakpoint", 1))
        return false;
    Debugger* dbg = Debugger::fromChildJSObject(obj);

    JSObject* handler = NonNullObject(cx, args[0]);
    if (!handler)
        return false;

    DebuggerScriptClearBreakpointMatcher matcher(cx, dbg, handler);
    if (!referent.match(matcher))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
DebuggerScript_clearAllBreakpoints(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "clearAllBreakpoints", args, obj, referent);
    Debugger* dbg = Debugger::fromChildJSObject(obj);
    DebuggerScriptClearBreakpointMatcher matcher(cx, dbg, nullptr);
    if (!referent.match(matcher))
          return false;
    args.rval().setUndefined();
    return true;
}

class DebuggerScriptIsInCatchScopeMatcher
{
    JSContext* cx_;
    size_t offset_;
    bool isInCatch_;

  public:
    explicit DebuggerScriptIsInCatchScopeMatcher(JSContext* cx, size_t offset) : cx_(cx), offset_(offset) { }
    using ReturnType = bool;

    inline bool isInCatch() const { return isInCatch_; }

    ReturnType match(HandleScript script) {
        if (!EnsureScriptOffsetIsValid(cx_, script, offset_))
            return false;

        /*
         * Try note ranges are relative to the mainOffset of the script, so adjust
         * offset accordingly.
         */
        size_t offset = offset_ - script->mainOffset();

        if (script->hasTrynotes()) {
            JSTryNote* tnBegin = script->trynotes()->vector;
            JSTryNote* tnEnd = tnBegin + script->trynotes()->length;
            while (tnBegin != tnEnd) {
                if (tnBegin->start <= offset &&
                    offset <= tnBegin->start + tnBegin->length &&
                    tnBegin->kind == JSTRY_CATCH)
                {
                    isInCatch_ = true;
                    return true;
                }
                ++tnBegin;
            }
        }
        isInCatch_ = false;
        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> instance) {
        isInCatch_ = false;
        return true;
    }
};

static bool
DebuggerScript_isInCatchScope(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_REFERENT(cx, argc, vp, "isInCatchScope", args, obj, referent);
    if (!args.requireAtLeast(cx, "Debugger.Script.isInCatchScope", 1))
        return false;

    size_t offset;
    if (!ScriptOffset(cx, args[0], &offset))
        return false;

    DebuggerScriptIsInCatchScopeMatcher matcher(cx, offset);
    if (!referent.match(matcher))
        return false;
    args.rval().setBoolean(matcher.isInCatch());
    return true;
}

static bool
DebuggerScript_getOffsetsCoverage(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getOffsetsCoverage", args, obj, script);

    // If the script has no coverage information, then skip this and return null
    // instead.
    if (!script->hasScriptCounts()) {
        args.rval().setNull();
        return true;
    }

    ScriptCounts* sc = &script->getScriptCounts();

    // If the main ever got visited, then assume that any code before main got
    // visited once.
    uint64_t hits = 0;
    const PCCounts* counts = sc->maybeGetPCCounts(script->pcToOffset(script->main()));
    if (counts->numExec())
        hits = 1;

    // Build an array of objects which are composed of 4 properties:
    //  - offset          PC offset of the current opcode.
    //  - lineNumber      Line of the current opcode.
    //  - columnNumber    Column of the current opcode.
    //  - count           Number of times the instruction got executed.
    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;

    RootedId offsetId(cx, NameToId(cx->names().offset));
    RootedId lineNumberId(cx, NameToId(cx->names().lineNumber));
    RootedId columnNumberId(cx, NameToId(cx->names().columnNumber));
    RootedId countId(cx, NameToId(cx->names().count));

    RootedObject item(cx);
    RootedValue offsetValue(cx);
    RootedValue lineNumberValue(cx);
    RootedValue columnNumberValue(cx);
    RootedValue countValue(cx);

    // Iterate linearly over the bytecode.
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
        size_t offset = r.frontOffset();

        // The beginning of each non-branching sequences of instruction set the
        // number of execution of the current instruction and any following
        // instruction.
        counts = sc->maybeGetPCCounts(offset);
        if (counts)
            hits = counts->numExec();

        offsetValue.setNumber(double(offset));
        lineNumberValue.setNumber(double(r.frontLineNumber()));
        columnNumberValue.setNumber(double(r.frontColumnNumber()));
        countValue.setNumber(double(hits));

        // Create a new object with the offset, line number, column number, the
        // number of hit counts, and append it to the array.
        item = NewObjectWithGivenProto<PlainObject>(cx, nullptr);
        if (!item ||
            !DefineDataProperty(cx, item, offsetId, offsetValue) ||
            !DefineDataProperty(cx, item, lineNumberId, lineNumberValue) ||
            !DefineDataProperty(cx, item, columnNumberId, columnNumberValue) ||
            !DefineDataProperty(cx, item, countId, countValue) ||
            !NewbornArrayPush(cx, result, ObjectValue(*item)))
        {
            return false;
        }

        // If the current instruction has thrown, then decrement the hit counts
        // with the number of throws.
        counts = sc->maybeGetThrowCounts(offset);
        if (counts)
            hits -= counts->numExec();
    }

    args.rval().setObject(*result);
    return true;
}

static bool
DebuggerScript_construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                              "Debugger.Script");
    return false;
}

static const JSPropertySpec DebuggerScript_properties[] = {
    JS_PSG("isGeneratorFunction", DebuggerScript_getIsGeneratorFunction, 0),
    JS_PSG("isAsyncFunction", DebuggerScript_getIsAsyncFunction, 0),
    JS_PSG("displayName", DebuggerScript_getDisplayName, 0),
    JS_PSG("url", DebuggerScript_getUrl, 0),
    JS_PSG("startLine", DebuggerScript_getStartLine, 0),
    JS_PSG("lineCount", DebuggerScript_getLineCount, 0),
    JS_PSG("source", DebuggerScript_getSource, 0),
    JS_PSG("sourceStart", DebuggerScript_getSourceStart, 0),
    JS_PSG("sourceLength", DebuggerScript_getSourceLength, 0),
    JS_PSG("global", DebuggerScript_getGlobal, 0),
    JS_PSG("format", DebuggerScript_getFormat, 0),
    JS_PS_END
};

static const JSFunctionSpec DebuggerScript_methods[] = {
    JS_FN("getChildScripts", DebuggerScript_getChildScripts, 0, 0),
    JS_FN("getAllOffsets", DebuggerScript_getAllOffsets, 0, 0),
    JS_FN("getAllColumnOffsets", DebuggerScript_getAllColumnOffsets, 0, 0),
    JS_FN("getLineOffsets", DebuggerScript_getLineOffsets, 1, 0),
    JS_FN("getOffsetLocation", DebuggerScript_getOffsetLocation, 0, 0),
    JS_FN("setBreakpoint", DebuggerScript_setBreakpoint, 2, 0),
    JS_FN("getBreakpoints", DebuggerScript_getBreakpoints, 1, 0),
    JS_FN("clearBreakpoint", DebuggerScript_clearBreakpoint, 1, 0),
    JS_FN("clearAllBreakpoints", DebuggerScript_clearAllBreakpoints, 0, 0),
    JS_FN("isInCatchScope", DebuggerScript_isInCatchScope, 1, 0),
    JS_FN("getOffsetsCoverage", DebuggerScript_getOffsetsCoverage, 0, 0),
    JS_FS_END
};


/*** Debugger.Source *****************************************************************************/

// For internal use only.
static inline NativeObject*
GetSourceReferentRawObject(JSObject* obj)
{
    MOZ_ASSERT(obj->getClass() == &DebuggerSource_class);
    return static_cast<NativeObject*>(obj->as<NativeObject>().getPrivate());
}

static inline DebuggerSourceReferent
GetSourceReferent(JSObject* obj)
{
    if (NativeObject* referent = GetSourceReferentRawObject(obj)) {
        if (referent->is<ScriptSourceObject>())
            return AsVariant(&referent->as<ScriptSourceObject>());
        return AsVariant(&referent->as<WasmInstanceObject>());
    }
    return AsVariant(static_cast<ScriptSourceObject*>(nullptr));
}

void
DebuggerSource_trace(JSTracer* trc, JSObject* obj)
{
    /*
     * There is a barrier on private pointers, so the Unbarriered marking
     * is okay.
     */
    if (JSObject *referent = GetSourceReferentRawObject(obj)) {
        TraceManuallyBarrieredCrossCompartmentEdge(trc, obj, &referent,
                                                   "Debugger.Source referent");
        obj->as<NativeObject>().setPrivateUnbarriered(referent);
    }
}

class SetDebuggerSourcePrivateMatcher
{
    NativeObject* obj_;
  public:
    explicit SetDebuggerSourcePrivateMatcher(NativeObject* obj) : obj_(obj) { }
    using ReturnType = void;
    ReturnType match(HandleScriptSource source) { obj_->setPrivateGCThing(source); }
    ReturnType match(Handle<WasmInstanceObject*> instance) { obj_->setPrivateGCThing(instance); }
};

NativeObject*
Debugger::newDebuggerSource(JSContext* cx, Handle<DebuggerSourceReferent> referent)
{
    assertSameCompartment(cx, object.get());

    RootedObject proto(cx, &object->getReservedSlot(JSSLOT_DEBUG_SOURCE_PROTO).toObject());
    MOZ_ASSERT(proto);
    NativeObject* sourceobj = NewNativeObjectWithGivenProto(cx, &DebuggerSource_class,
                                                            proto, TenuredObject);
    if (!sourceobj)
        return nullptr;
    sourceobj->setReservedSlot(JSSLOT_DEBUGSOURCE_OWNER, ObjectValue(*object));
    SetDebuggerSourcePrivateMatcher matcher(sourceobj);
    referent.match(matcher);

    return sourceobj;
}

JSObject*
Debugger::wrapVariantReferent(JSContext* cx, Handle<DebuggerSourceReferent> referent)
{
    JSObject* obj;
    if (referent.is<ScriptSourceObject*>()) {
        Handle<ScriptSourceObject*> untaggedReferent = referent.template as<ScriptSourceObject*>();
        Rooted<CrossCompartmentKey> key(cx, CrossCompartmentKey(object, untaggedReferent,
                                    CrossCompartmentKey::DebuggerObjectKind::DebuggerSource));
        obj = wrapVariantReferent<DebuggerSourceReferent, ScriptSourceObject*, SourceWeakMap>(
            cx, sources, key, referent);
    } else {
        Handle<WasmInstanceObject*> untaggedReferent = referent.template as<WasmInstanceObject*>();
        Rooted<CrossCompartmentKey> key(cx, CrossCompartmentKey(object, untaggedReferent,
                                    CrossCompartmentKey::DebuggerObjectKind::DebuggerWasmSource));
        obj = wrapVariantReferent<DebuggerSourceReferent, WasmInstanceObject*, WasmInstanceWeakMap>(
            cx, wasmInstanceSources, key, referent);
    }
    MOZ_ASSERT_IF(obj, GetSourceReferent(obj) == referent);
    return obj;
}

JSObject*
Debugger::wrapSource(JSContext* cx, HandleScriptSource source)
{
    Rooted<DebuggerSourceReferent> referent(cx, source.get());
    return wrapVariantReferent(cx, referent);
}

JSObject*
Debugger::wrapWasmSource(JSContext* cx, Handle<WasmInstanceObject*> wasmInstance)
{
    Rooted<DebuggerSourceReferent> referent(cx, wasmInstance.get());
    return wrapVariantReferent(cx, referent);
}

static bool
DebuggerSource_construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                              "Debugger.Source");
    return false;
}

static NativeObject*
DebuggerSource_check(JSContext* cx, HandleValue thisv, const char* fnname)
{
    JSObject* thisobj = NonNullObject(cx, thisv);
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &DebuggerSource_class) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Source", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    NativeObject* nthisobj = &thisobj->as<NativeObject>();

    if (!GetSourceReferentRawObject(thisobj)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Source", fnname, "prototype object");
        return nullptr;
    }

    return nthisobj;
}

template <typename ReferentT>
static NativeObject*
DebuggerSource_checkThis(JSContext* cx, const CallArgs& args, const char* fnname,
                         const char* refname)
{
    NativeObject* thisobj = DebuggerSource_check(cx, args.thisv(), fnname);
    if (!thisobj)
        return nullptr;

    if (!GetSourceReferent(thisobj).is<ReferentT>()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                              JSDVG_SEARCH_STACK, args.thisv(), nullptr,
                              refname, nullptr);
        return nullptr;
    }

    return thisobj;
}

#define THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, fnname, args, obj, referent)        \
    CallArgs args = CallArgsFromVp(argc, vp);                                       \
    RootedNativeObject obj(cx, DebuggerSource_check(cx, args.thisv(), fnname));     \
    if (!obj)                                                                       \
        return false;                                                               \
    Rooted<DebuggerSourceReferent> referent(cx, GetSourceReferent(obj))

#define THIS_DEBUGSOURCE_SOURCE(cx, argc, vp, fnname, args, obj, sourceObject)      \
    CallArgs args = CallArgsFromVp(argc, vp);                                       \
    RootedNativeObject obj(cx,                                                      \
        DebuggerSource_checkThis<ScriptSourceObject*>(cx, args, fnname,             \
                                                      "a JS source"));              \
    if (!obj)                                                                       \
        return false;                                                               \
    RootedScriptSource sourceObject(cx, GetSourceReferent(obj).as<ScriptSourceObject*>())

class DebuggerSourceGetTextMatcher
{
    JSContext* cx_;

  public:
    explicit DebuggerSourceGetTextMatcher(JSContext* cx) : cx_(cx) { }

    using ReturnType = JSString*;

    ReturnType match(HandleScriptSource sourceObject) {
        ScriptSource* ss = sourceObject->source();
        bool hasSourceData = ss->hasSourceData();
        if (!ss->hasSourceData() && !JSScript::loadSource(cx_, ss, &hasSourceData))
            return nullptr;
        if (!hasSourceData)
            return NewStringCopyZ<CanGC>(cx_, "[no source]");

        if (ss->isFunctionBody())
            return ss->functionBodyString(cx_);

        return ss->substring(cx_, 0, ss->length());
    }

    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        if (wasmInstance->instance().debug().maybeBytecode() &&
            wasmInstance->instance().debug().binarySource())
        {
            return NewStringCopyZ<CanGC>(cx_, "[wasm]");
        }
        return wasmInstance->instance().debug().createText(cx_);
    }
};

static bool
DebuggerSource_getText(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get text)", args, obj, referent);
    Value textv = obj->getReservedSlot(JSSLOT_DEBUGSOURCE_TEXT);
    if (!textv.isUndefined()) {
        MOZ_ASSERT(textv.isString());
        args.rval().set(textv);
        return true;
    }

    DebuggerSourceGetTextMatcher matcher(cx);
    JSString* str = referent.match(matcher);
    if (!str)
        return false;

    args.rval().setString(str);
    obj->setReservedSlot(JSSLOT_DEBUGSOURCE_TEXT, args.rval());
    return true;
}

static bool
DebuggerSource_getBinary(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get binary)", args, obj, referent);

    if (!referent.is<WasmInstanceObject*>()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                              JSDVG_SEARCH_STACK, args.thisv(), nullptr,
                              "a wasm source", nullptr);
        return false;
    }

    RootedWasmInstanceObject wasmInstance(cx, referent.as<WasmInstanceObject*>());
    if (!wasmInstance->instance().debug().binarySource()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DEBUG_NO_BINARY_SOURCE);
        return false;
    }

    auto bytecode = wasmInstance->instance().debug().maybeBytecode();
    size_t arrLength = bytecode ? bytecode->length() : 0;
    RootedObject arr(cx, JS_NewUint8Array(cx, arrLength));
    if (!arr)
        return false;
    if (bytecode)
        memcpy(arr->as<TypedArrayObject>().viewDataUnshared(), bytecode->begin(), arrLength);

    args.rval().setObject(*arr);
    return true;
}

class DebuggerSourceGetURLMatcher
{
    JSContext* cx_;

  public:
    explicit DebuggerSourceGetURLMatcher(JSContext* cx) : cx_(cx) { }

    using ReturnType = Maybe<JSString*>;

    ReturnType match(HandleScriptSource sourceObject) {
        ScriptSource* ss = sourceObject->source();
        MOZ_ASSERT(ss);
        if (ss->filename()) {
            JSString* str = NewStringCopyZ<CanGC>(cx_, ss->filename());
            return Some(str);
        }
        return Nothing();
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        if (wasmInstance->instance().metadata().baseURL) {
            JSString* str = NewStringCopyZ<CanGC>(cx_, wasmInstance->instance().metadata().baseURL.get());
            if (!str)
                return Nothing();
            return Some(str);
        }
        if (JSString* str = wasmInstance->instance().debug().debugDisplayURL(cx_))
            return Some(str);
        return Nothing();
    }
};

static bool
DebuggerSource_getURL(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get url)", args, obj, referent);

    DebuggerSourceGetURLMatcher matcher(cx);
    Maybe<JSString*> str = referent.match(matcher);
    if (str.isSome()) {
        if (!*str)
            return false;
        args.rval().setString(*str);
    } else {
        args.rval().setNull();
    }
    return true;
}

struct DebuggerSourceGetDisplayURLMatcher
{
    using ReturnType = const char16_t*;
    ReturnType match(HandleScriptSource sourceObject) {
        ScriptSource* ss = sourceObject->source();
        MOZ_ASSERT(ss);
        return ss->hasDisplayURL() ? ss->displayURL() : nullptr;
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return wasmInstance->instance().metadata().displayURL();
    }
};

static bool
DebuggerSource_getDisplayURL(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get url)", args, obj, referent);

    DebuggerSourceGetDisplayURLMatcher matcher;
    if (const char16_t* displayURL = referent.match(matcher)) {
        JSString* str = JS_NewUCStringCopyZ(cx, displayURL);
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setNull();
    }
    return true;
}

struct DebuggerSourceGetElementMatcher
{
    using ReturnType = JSObject*;
    ReturnType match(HandleScriptSource sourceObject) {
        return sourceObject->element();
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return nullptr;
    }
};

static bool
DebuggerSource_getElement(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get element)", args, obj, referent);

    DebuggerSourceGetElementMatcher matcher;
    if (JSObject* element = referent.match(matcher)) {
        args.rval().setObjectOrNull(element);
        if (!Debugger::fromChildJSObject(obj)->wrapDebuggeeValue(cx, args.rval()))
            return false;
    } else {
        args.rval().setUndefined();
    }
    return true;
}

struct DebuggerSourceGetElementPropertyMatcher
{
    using ReturnType = Value;
    ReturnType match(HandleScriptSource sourceObject) {
        return sourceObject->elementAttributeName();
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return UndefinedValue();
    }
};

static bool
DebuggerSource_getElementProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get elementAttributeName)", args, obj, referent);
    DebuggerSourceGetElementPropertyMatcher matcher;
    args.rval().set(referent.match(matcher));
    return Debugger::fromChildJSObject(obj)->wrapDebuggeeValue(cx, args.rval());
}

class DebuggerSourceGetIntroductionScriptMatcher
{
    JSContext* cx_;
    Debugger* dbg_;
    MutableHandleValue rval_;

  public:
    DebuggerSourceGetIntroductionScriptMatcher(JSContext* cx, Debugger* dbg,
                                               MutableHandleValue rval)
      : cx_(cx),
        dbg_(dbg),
        rval_(rval)
    { }

    using ReturnType = bool;

    ReturnType match(HandleScriptSource sourceObject) {
        RootedScript script(cx_, sourceObject->introductionScript());
        if (script) {
            RootedObject scriptDO(cx_, dbg_->wrapScript(cx_, script));
            if (!scriptDO)
                return false;
            rval_.setObject(*scriptDO);
        } else {
            rval_.setUndefined();
        }
        return true;
    }

    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        RootedObject ds(cx_, dbg_->wrapWasmScript(cx_, wasmInstance));
        if (!ds)
            return false;
        rval_.setObject(*ds);
        return true;
    }
};

static bool
DebuggerSource_getIntroductionScript(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get introductionScript)", args, obj, referent);
    Debugger* dbg = Debugger::fromChildJSObject(obj);
    DebuggerSourceGetIntroductionScriptMatcher matcher(cx, dbg, args.rval());
    return referent.match(matcher);
}

struct DebuggerGetIntroductionOffsetMatcher
{
    using ReturnType = Value;
    ReturnType match(HandleScriptSource sourceObject) {
        // Regardless of what's recorded in the ScriptSourceObject and
        // ScriptSource, only hand out the introduction offset if we also have
        // the script within which it applies.
        ScriptSource* ss = sourceObject->source();
        if (ss->hasIntroductionOffset() && sourceObject->introductionScript())
            return Int32Value(ss->introductionOffset());
        return UndefinedValue();
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return UndefinedValue();
    }
};

static bool
DebuggerSource_getIntroductionOffset(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get introductionOffset)", args, obj, referent);
    DebuggerGetIntroductionOffsetMatcher matcher;
    args.rval().set(referent.match(matcher));
    return true;
}

struct DebuggerSourceGetIntroductionTypeMatcher
{
    using ReturnType = const char*;
    ReturnType match(HandleScriptSource sourceObject) {
        ScriptSource* ss = sourceObject->source();
        MOZ_ASSERT(ss);
        return ss->hasIntroductionType() ? ss->introductionType() : nullptr;
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        return "wasm";
    }
};

static bool
DebuggerSource_getIntroductionType(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get introductionType)", args, obj, referent);

    DebuggerSourceGetIntroductionTypeMatcher matcher;
    if (const char* introductionType = referent.match(matcher)) {
        JSString* str = NewStringCopyZ<CanGC>(cx, introductionType);
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setUndefined();
    }

    return true;
}

static bool
DebuggerSource_setSourceMapURL(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_SOURCE(cx, argc, vp, "set sourceMapURL", args, obj, sourceObject);
    ScriptSource* ss = sourceObject->source();
    MOZ_ASSERT(ss);
    if (!args.requireAtLeast(cx, "set sourceMapURL", 1))
        return false;

    JSString* str = ToString<CanGC>(cx, args[0]);
    if (!str)
        return false;

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, str))
        return false;

    if (!ss->setSourceMapURL(cx, stableChars.twoByteChars()))
        return false;

    args.rval().setUndefined();
    return true;
}

class DebuggerSourceGetSourceMapURLMatcher
{
    JSContext* cx_;
    MutableHandleString result_;

  public:
    explicit DebuggerSourceGetSourceMapURLMatcher(JSContext* cx, MutableHandleString result)
      : cx_(cx),
        result_(result)
    { }

    using ReturnType = bool;
    ReturnType match(HandleScriptSource sourceObject) {
        ScriptSource* ss = sourceObject->source();
        MOZ_ASSERT(ss);
        if (!ss->hasSourceMapURL()) {
            result_.set(nullptr);
            return true;
        }
        JSString* str = JS_NewUCStringCopyZ(cx_, ss->sourceMapURL());
        if (!str)
            return false;
        result_.set(str);
        return true;
    }
    ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
        // sourceMapURL is not available if debugger was not in
        // allowWasmBinarySource mode.
        if (!wasmInstance->instance().debug().binarySource()) {
            result_.set(nullptr);
            return true;
        }
        RootedString str(cx_);
        if (!wasmInstance->instance().debug().getSourceMappingURL(cx_, &str))
            return false;
        result_.set(str);
        return true;
    }
};

static bool
DebuggerSource_getSourceMapURL(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGSOURCE_REFERENT(cx, argc, vp, "(get sourceMapURL)", args, obj, referent);

    RootedString result(cx);
    DebuggerSourceGetSourceMapURLMatcher matcher(cx, &result);
    if (!referent.match(matcher))
        return false;
    if (result)
        args.rval().setString(result);
    else
        args.rval().setNull();
    return true;
}

static const JSPropertySpec DebuggerSource_properties[] = {
    JS_PSG("text", DebuggerSource_getText, 0),
    JS_PSG("binary", DebuggerSource_getBinary, 0),
    JS_PSG("url", DebuggerSource_getURL, 0),
    JS_PSG("element", DebuggerSource_getElement, 0),
    JS_PSG("displayURL", DebuggerSource_getDisplayURL, 0),
    JS_PSG("introductionScript", DebuggerSource_getIntroductionScript, 0),
    JS_PSG("introductionOffset", DebuggerSource_getIntroductionOffset, 0),
    JS_PSG("introductionType", DebuggerSource_getIntroductionType, 0),
    JS_PSG("elementAttributeName", DebuggerSource_getElementProperty, 0),
    JS_PSGS("sourceMapURL", DebuggerSource_getSourceMapURL, DebuggerSource_setSourceMapURL, 0),
    JS_PS_END
};

static const JSFunctionSpec DebuggerSource_methods[] = {
    JS_FS_END
};


/*** Debugger.Frame ******************************************************************************/

ScriptedOnStepHandler::ScriptedOnStepHandler(JSObject* object)
  : object_(object)
{
    MOZ_ASSERT(object_->isCallable());
}

JSObject*
ScriptedOnStepHandler::object() const
{
    return object_;
}

void
ScriptedOnStepHandler::drop()
{
    this->~ScriptedOnStepHandler();
    js_free(this);
}

void
ScriptedOnStepHandler::trace(JSTracer* tracer)
{
    TraceEdge(tracer, &object_, "OnStepHandlerFunction.object");
}

bool
ScriptedOnStepHandler::onStep(JSContext* cx, HandleDebuggerFrame frame, JSTrapStatus& statusp,
                              MutableHandleValue vp)
{
    RootedValue fval(cx, ObjectValue(*object_));
    RootedValue rval(cx);
    if (!js::Call(cx, fval, frame, &rval))
        return false;

    return ParseResumptionValue(cx, rval, statusp, vp);
};

ScriptedOnPopHandler::ScriptedOnPopHandler(JSObject* object)
  : object_(object)
{
    MOZ_ASSERT(object->isCallable());
}

JSObject*
ScriptedOnPopHandler::object() const
{
    return object_;
}

void
ScriptedOnPopHandler::drop()
{
    this->~ScriptedOnPopHandler();
    js_free(this);
}

void
ScriptedOnPopHandler::trace(JSTracer* tracer)
{
    TraceEdge(tracer, &object_, "OnStepHandlerFunction.object");
}

bool
ScriptedOnPopHandler::onPop(JSContext* cx, HandleDebuggerFrame frame, JSTrapStatus& statusp,
                            MutableHandleValue vp)
{
    Debugger *dbg = frame->owner();

    RootedValue completion(cx);
    if (!dbg->newCompletionValue(cx, statusp, vp, &completion))
        return false;

    RootedValue fval(cx, ObjectValue(*object_));
    RootedValue rval(cx);
    if (!js::Call(cx, fval, frame, completion, &rval))
        return false;

    return ParseResumptionValue(cx, rval, statusp, vp);
};

/* static */ NativeObject*
DebuggerFrame::initClass(JSContext* cx, HandleObject dbgCtor, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));

    return InitClass(cx, dbgCtor, objProto, &class_, construct, 0, properties_,
                     methods_, nullptr, nullptr);
}

/* static */ DebuggerFrame*
DebuggerFrame::create(JSContext* cx, HandleObject proto, AbstractFramePtr referent,
                      const FrameIter* maybeIter, HandleNativeObject debugger)
{
  JSObject* obj = NewObjectWithGivenProto(cx, &DebuggerFrame::class_, proto);
  if (!obj)
      return nullptr;

  DebuggerFrame& frame = obj->as<DebuggerFrame>();

  // Eagerly copy FrameIter data if we've already walked the stack.
  if (maybeIter) {
      AbstractFramePtr data = maybeIter->copyDataAsAbstractFramePtr();
      if (!data)
          return nullptr;
      frame.setPrivate(data.raw());
  } else {
      frame.setPrivate(referent.raw());
  }

  frame.setReservedSlot(JSSLOT_DEBUGFRAME_OWNER, ObjectValue(*debugger));

  return &frame;
}

/* static */ bool
DebuggerFrame::getCallee(JSContext* cx, HandleDebuggerFrame frame,
                         MutableHandleDebuggerObject result)
{
    MOZ_ASSERT(frame->isLive());

    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);
    if (!referent.isFunctionFrame()) {
        result.set(nullptr);
        return true;
    }

    Debugger* dbg = frame->owner();

    RootedObject callee(cx, referent.callee());
    return dbg->wrapDebuggeeObject(cx, callee, result);
}

/* static */ bool
DebuggerFrame::getIsConstructing(JSContext* cx, HandleDebuggerFrame frame, bool& result)
{
    MOZ_ASSERT(frame->isLive());

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    result = iter.isFunctionFrame() && iter.isConstructing();
    return true;
}

static void
UpdateFrameIterPc(FrameIter& iter)
{
    if (iter.abstractFramePtr().isWasmDebugFrame()) {
        // Wasm debug frames don't need their pc updated -- it's null.
        return;
    }

    if (iter.abstractFramePtr().isRematerializedFrame()) {
#ifdef DEBUG
        // Rematerialized frames don't need their pc updated. The reason we
        // need to update pc is because we might get the same Debugger.Frame
        // object for multiple re-entries into debugger code from debuggee
        // code. This reentrancy is not possible with rematerialized frames,
        // because when returning to debuggee code, we would have bailed out
        // to baseline.
        //
        // We walk the stack to assert that it doesn't need updating.
        jit::RematerializedFrame* frame = iter.abstractFramePtr().asRematerializedFrame();
        jit::JitFrameLayout* jsFrame = (jit::JitFrameLayout*)frame->top();
        jit::JitActivation* activation = iter.activation()->asJit();

        JSContext* cx = TlsContext.get();
        MOZ_ASSERT(cx == activation->cx());

        ActivationIterator activationIter(cx);
        while (activationIter.activation() != activation)
            ++activationIter;

        OnlyJSJitFrameIter jitIter(activationIter);
        while (!jitIter.frame().isIonJS() || jitIter.frame().jsFrame() != jsFrame)
            ++jitIter;

        jit::InlineFrameIterator ionInlineIter(cx, &jitIter.frame());
        while (ionInlineIter.frameNo() != frame->frameNo())
            ++ionInlineIter;

        MOZ_ASSERT(ionInlineIter.pc() == iter.pc());
#endif
        return;
    }

    iter.updatePcQuadratic();
}

/* static */ bool
DebuggerFrame::getEnvironment(JSContext* cx, HandleDebuggerFrame frame,
                              MutableHandleDebuggerEnvironment result)
{
    MOZ_ASSERT(frame->isLive());

    Debugger* dbg = frame->owner();

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    Rooted<Env*> env(cx);
    {
        AutoCompartment ac(cx, iter.abstractFramePtr().environmentChain());
        UpdateFrameIterPc(iter);
        env = GetDebugEnvironmentForFrame(cx, iter.abstractFramePtr(), iter.pc());
        if (!env)
            return false;
    }

    return dbg->wrapEnvironment(cx, env, result);
}

/* static */ bool
DebuggerFrame::getIsGenerator(HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);
    return referent.hasScript() && referent.script()->isGenerator();
}

/* static */ bool
DebuggerFrame::getOffset(JSContext* cx, HandleDebuggerFrame frame, size_t& result)
{
    MOZ_ASSERT(frame->isLive());

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);
    if (referent.isWasmDebugFrame()) {
        iter.wasmUpdateBytecodeOffset();
        result = iter.wasmBytecodeOffset();
    } else {
        JSScript* script = iter.script();
        UpdateFrameIterPc(iter);
        jsbytecode* pc = iter.pc();
        result = script->pcToOffset(pc);
    }
    return true;
}

/* static */ bool
DebuggerFrame::getOlder(JSContext* cx, HandleDebuggerFrame frame,
                        MutableHandleDebuggerFrame result)
{
    MOZ_ASSERT(frame->isLive());

    Debugger* dbg = frame->owner();

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    for (++iter; !iter.done(); ++iter) {
        if (dbg->observesFrame(iter)) {
            if (iter.isIon() && !iter.ensureHasRematerializedFrame(cx))
                return false;
            return dbg->getScriptFrame(cx, iter, result);
        }
    }

    result.set(nullptr);
    return true;
}

/* static */ bool
DebuggerFrame::getThis(JSContext* cx, HandleDebuggerFrame frame, MutableHandleValue result)
{
    MOZ_ASSERT(frame->isLive());

    if (!requireScriptReferent(cx, frame))
        return false;

    Debugger* dbg = frame->owner();

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    {
        AbstractFramePtr frame = iter.abstractFramePtr();
        AutoCompartment ac(cx, frame.environmentChain());

        UpdateFrameIterPc(iter);

        if (!GetThisValueForDebuggerMaybeOptimizedOut(cx, frame, iter.pc(), result))
            return false;
    }

    return dbg->wrapDebuggeeValue(cx, result);
}

/* static */ DebuggerFrameType
DebuggerFrame::getType(HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);

    /*
     * Indirect eval frames are both isGlobalFrame() and isEvalFrame(), so the
     * order of checks here is significant.
     */
    if (referent.isEvalFrame())
        return DebuggerFrameType::Eval;
    else if (referent.isGlobalFrame())
        return DebuggerFrameType::Global;
    else if (referent.isFunctionFrame())
        return DebuggerFrameType::Call;
    else if (referent.isModuleFrame())
        return DebuggerFrameType::Module;
    else if (referent.isWasmDebugFrame())
        return DebuggerFrameType::WasmCall;
    MOZ_CRASH("Unknown frame type");
}

/* static */ DebuggerFrameImplementation
DebuggerFrame::getImplementation(HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);

    if (referent.isBaselineFrame())
        return DebuggerFrameImplementation::Baseline;
    else if (referent.isRematerializedFrame())
        return DebuggerFrameImplementation::Ion;
    else if (referent.isWasmDebugFrame())
        return DebuggerFrameImplementation::Wasm;
    return DebuggerFrameImplementation::Interpreter;
}

/*
 * If succesful, transfers the ownership of the given `handler` to this
 * Debugger.Frame. Note that on failure, the ownership of `handler` is not
 * transferred, and the caller is responsible for cleaning it up.
 */
/* static */ bool
DebuggerFrame::setOnStepHandler(JSContext* cx, HandleDebuggerFrame frame, OnStepHandler* handler)
{
    MOZ_ASSERT(frame->isLive());

    OnStepHandler* prior = frame->onStepHandler();
    if (prior && handler != prior) {
        prior->drop();
    }

    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);
    if (referent.isWasmDebugFrame()) {
        wasm::Instance* instance = referent.asWasmDebugFrame()->instance();
        wasm::DebugFrame* wasmFrame = referent.asWasmDebugFrame();
        if (handler && !prior) {
            // Single stepping toggled off->on.
            if (!instance->debug().incrementStepModeCount(cx, wasmFrame->funcIndex()))
                return false;
        } else if (!handler && prior) {
            // Single stepping toggled on->off.
            FreeOp* fop = cx->runtime()->defaultFreeOp();
            if (!instance->debug().decrementStepModeCount(fop, wasmFrame->funcIndex()))
                return false;
        }
    } else {
        if (handler && !prior) {
            // Single stepping toggled off->on.
            AutoCompartment ac(cx, referent.environmentChain());
            // Ensure observability *before* incrementing the step mode count.
            // Calling this function after calling incrementStepModeCount
            // will make it a no-op.
            Debugger* dbg = frame->owner();
            if (!dbg->ensureExecutionObservabilityOfScript(cx, referent.script()))
                return false;
            if (!referent.script()->incrementStepModeCount(cx))
                return false;
        } else if (!handler && prior) {
            // Single stepping toggled on->off.
            referent.script()->decrementStepModeCount(cx->runtime()->defaultFreeOp());
        }
    }

    /* Now that the step mode switch has succeeded, we can install the handler. */
    frame->setReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER,
                           handler ? PrivateValue(handler) : UndefinedValue());
    return true;
}

/* static */ bool
DebuggerFrame::getArguments(JSContext *cx, HandleDebuggerFrame frame,
                            MutableHandleDebuggerArguments result)
{
    Value argumentsv = frame->getReservedSlot(JSSLOT_DEBUGFRAME_ARGUMENTS);
    if (!argumentsv.isUndefined()) {
        result.set(argumentsv.isObject() ? &argumentsv.toObject().as<DebuggerArguments>() : nullptr);
        return true;
    }

    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);

    RootedDebuggerArguments arguments(cx);
    if (referent.hasArgs()) {
        Rooted<GlobalObject*> global(cx, &frame->global());
        RootedObject proto(cx, GlobalObject::getOrCreateArrayPrototype(cx, global));
        if (!proto)
            return false;
        arguments = DebuggerArguments::create(cx, proto, frame);
        if (!arguments)
            return false;
    } else {
        arguments = nullptr;
    }

    result.set(arguments);
    frame->setReservedSlot(JSSLOT_DEBUGFRAME_ARGUMENTS, ObjectOrNullValue(result));
    return true;
}

/*
 * Evaluate |chars[0..length-1]| in the environment |env|, treating that
 * source as appearing starting at |lineno| in |filename|. Store the return
 * value in |*rval|. Use |thisv| as the 'this' value.
 *
 * If |frame| is non-nullptr, evaluate as for a direct eval in that frame; |env|
 * must be either |frame|'s DebugScopeObject, or some extension of that
 * environment; either way, |frame|'s scope is where newly declared variables
 * go. In this case, |frame| must have a computed 'this' value, equal to |thisv|.
 */
static bool
EvaluateInEnv(JSContext* cx, Handle<Env*> env, AbstractFramePtr frame,
              mozilla::Range<const char16_t> chars, const char* filename,
              unsigned lineno, MutableHandleValue rval)
{
    assertSameCompartment(cx, env, frame);

    CompileOptions options(cx);
    options.setIsRunOnce(true)
           .setNoScriptRval(false)
           .setFileAndLine(filename, lineno)
           .setCanLazilyParse(false)
           .setIntroductionType("debugger eval")
           .maybeMakeStrictMode(frame && frame.hasScript() ? frame.script()->strict() : false);
    RootedScript callerScript(cx, frame && frame.hasScript() ? frame.script() : nullptr);
    SourceBufferHolder srcBuf(chars.begin().get(), chars.length(), SourceBufferHolder::NoOwnership);
    RootedScript script(cx);

    ScopeKind scopeKind;
    if (IsGlobalLexicalEnvironment(env))
        scopeKind = ScopeKind::Global;
    else
        scopeKind = ScopeKind::NonSyntactic;

    if (frame) {
        MOZ_ASSERT(scopeKind == ScopeKind::NonSyntactic);
        RootedScope scope(cx, GlobalScope::createEmpty(cx, ScopeKind::NonSyntactic));
        if (!scope)
            return false;
        script = frontend::CompileEvalScript(cx, cx->tempLifoAlloc(), env, scope,
                                             options, srcBuf);
        if (script)
            script->setActiveEval();
    } else {
        // Do not consider executeInGlobal{WithBindings} as an eval, but instead
        // as executing a series of statements at the global level. This is to
        // circumvent the fresh lexical scope that all eval have, so that the
        // users of executeInGlobal, like the web console, may add new bindings to
        // the global scope.
        script = frontend::CompileGlobalScript(cx, cx->tempLifoAlloc(), scopeKind, options,
                                               srcBuf);
    }

    if (!script)
        return false;

    return ExecuteKernel(cx, script, *env, NullValue(), frame, rval.address());
}

static bool
DebuggerGenericEval(JSContext* cx, const mozilla::Range<const char16_t> chars,
                    HandleObject bindings, const EvalOptions& options,
                    JSTrapStatus& status, MutableHandleValue value,
                    Debugger* dbg, HandleObject envArg, FrameIter* iter)
{
    /* Either we're specifying the frame, or a global. */
    MOZ_ASSERT_IF(iter, !envArg);
    MOZ_ASSERT_IF(!iter, envArg && IsGlobalLexicalEnvironment(envArg));

    /*
     * Gather keys and values of bindings, if any. This must be done in the
     * debugger compartment, since that is where any exceptions must be
     * thrown.
     */
    AutoIdVector keys(cx);
    AutoValueVector values(cx);
    if (bindings) {
        if (!GetPropertyKeys(cx, bindings, JSITER_OWNONLY, &keys) ||
            !values.growBy(keys.length()))
        {
            return false;
        }
        for (size_t i = 0; i < keys.length(); i++) {
            MutableHandleValue valp = values[i];
            if (!GetProperty(cx, bindings, bindings, keys[i], valp) ||
                !dbg->unwrapDebuggeeValue(cx, valp))
            {
                return false;
            }
        }
    }

    Maybe<AutoCompartment> ac;
    if (iter)
        ac.emplace(cx, iter->environmentChain(cx));
    else
        ac.emplace(cx, envArg);

    Rooted<Env*> env(cx);
    if (iter) {
        env = GetDebugEnvironmentForFrame(cx, iter->abstractFramePtr(), iter->pc());
        if (!env)
            return false;
    } else {
        env = envArg;
    }

    /* If evalWithBindings, create the inner environment. */
    if (bindings) {
        RootedPlainObject nenv(cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr));
        if (!nenv)
            return false;
        RootedId id(cx);
        for (size_t i = 0; i < keys.length(); i++) {
            id = keys[i];
            cx->markId(id);
            MutableHandleValue val = values[i];
            if (!cx->compartment()->wrap(cx, val) ||
                !NativeDefineDataProperty(cx, nenv, id, val, 0))
            {
                return false;
            }
        }

        AutoObjectVector envChain(cx);
        if (!envChain.append(nenv))
            return false;

        RootedObject newEnv(cx);
        if (!CreateObjectsForEnvironmentChain(cx, envChain, env, &newEnv))
            return false;

        env = newEnv;
    }

    /* Run the code and produce the completion value. */
    LeaveDebuggeeNoExecute nnx(cx);
    RootedValue rval(cx);
    AbstractFramePtr frame = iter ? iter->abstractFramePtr() : NullFramePtr();

    bool ok = EvaluateInEnv(cx, env, frame, chars,
                            options.filename() ? options.filename() : "debugger eval code",
                            options.lineno(), &rval);
    Debugger::resultToCompletion(cx, ok, rval, &status, value);
    ac.reset();
    return dbg->wrapDebuggeeValue(cx, value);
}

/* static */ bool
DebuggerFrame::eval(JSContext* cx, HandleDebuggerFrame frame, mozilla::Range<const char16_t> chars,
                    HandleObject bindings, const EvalOptions& options, JSTrapStatus& status,
                    MutableHandleValue value)
{
    MOZ_ASSERT(frame->isLive());

    Debugger* dbg = frame->owner();

    Maybe<FrameIter> maybeIter;
    if (!DebuggerFrame::getFrameIter(cx, frame, maybeIter))
        return false;
    FrameIter& iter = *maybeIter;

    UpdateFrameIterPc(iter);

    return DebuggerGenericEval(cx, chars, bindings, options, status, value, dbg, nullptr, &iter);
}

/* statuc */ bool
DebuggerFrame::isLive() const
{
    return !!getPrivate();
}

OnStepHandler*
DebuggerFrame::onStepHandler() const
{
    Value value = getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER);
    return value.isUndefined() ? nullptr : static_cast<OnStepHandler*>(value.toPrivate());
}

OnPopHandler*
DebuggerFrame::onPopHandler() const
{
    Value value = getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER);
    return value.isUndefined() ? nullptr : static_cast<OnPopHandler*>(value.toPrivate());
}

void
DebuggerFrame::setOnPopHandler(OnPopHandler* handler)
{
    MOZ_ASSERT(isLive());

    OnPopHandler* prior = onPopHandler();
    if (prior && prior != handler)
        prior->drop();

    setReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER,
                    handler ? PrivateValue(handler) : UndefinedValue());
}

static bool
DebuggerFrame_requireLive(JSContext* cx, HandleDebuggerFrame frame)
{
    if (!frame->isLive()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_LIVE,
                                  "Debugger.Frame");
        return false;
    }

    return true;
}

/* static */ AbstractFramePtr
DebuggerFrame::getReferent(HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = AbstractFramePtr::FromRaw(frame->getPrivate());
    if (referent.isScriptFrameIterData()) {
        FrameIter iter(*(FrameIter::Data*)(referent.raw()));
        referent = iter.abstractFramePtr();
    }
    return referent;
}

/* static */ bool
DebuggerFrame::getFrameIter(JSContext* cx, HandleDebuggerFrame frame,
                            Maybe<FrameIter>& result)
{
    AbstractFramePtr referent = AbstractFramePtr::FromRaw(frame->getPrivate());
    if (referent.isScriptFrameIterData()) {
        result.emplace(*reinterpret_cast<FrameIter::Data*>(referent.raw()));
    } else {
        result.emplace(cx, FrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK);
        FrameIter& iter = *result;
        while (!iter.hasUsableAbstractFramePtr() || iter.abstractFramePtr() != referent)
            ++iter;
        AbstractFramePtr data = iter.copyDataAsAbstractFramePtr();
        if (!data)
            return false;
        frame->setPrivate(data.raw());
    }
    return true;
}

/* static */ bool
DebuggerFrame::requireScriptReferent(JSContext* cx, HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);
    if (!referent.hasScript()) {
        RootedValue frameobj(cx, ObjectValue(*frame));
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                              JSDVG_SEARCH_STACK, frameobj, nullptr,
                              "a script frame", nullptr);
        return false;
    }
    return true;
}

static void
DebuggerFrame_freeScriptFrameIterData(FreeOp* fop, JSObject* obj)
{
    AbstractFramePtr frame = AbstractFramePtr::FromRaw(obj->as<NativeObject>().getPrivate());
    if (frame.isScriptFrameIterData())
        fop->delete_((FrameIter::Data*) frame.raw());
    obj->as<NativeObject>().setPrivate(nullptr);
}

static void
DebuggerFrame_maybeDecrementFrameScriptStepModeCount(FreeOp* fop, AbstractFramePtr frame,
                                                     NativeObject* frameobj)
{
    /* If this frame has an onStep handler, decrement the script's count. */
    if (frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined())
        return;
    if (frame.isWasmDebugFrame()) {
        wasm::Instance* instance = frame.wasmInstance();
        instance->debug().decrementStepModeCount(fop, frame.asWasmDebugFrame()->funcIndex());
    } else {
        frame.script()->decrementStepModeCount(fop);
    }
}

static void
DebuggerFrame_finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->maybeOnHelperThread());
    DebuggerFrame_freeScriptFrameIterData(fop, obj);
    OnStepHandler* onStepHandler = obj->as<DebuggerFrame>().onStepHandler();
    if (onStepHandler)
       onStepHandler->drop();
    OnPopHandler* onPopHandler = obj->as<DebuggerFrame>().onPopHandler();
    if (onPopHandler)
       onPopHandler->drop();
}

static void
DebuggerFrame_trace(JSTracer* trc, JSObject* obj)
{
    OnStepHandler* onStepHandler = obj->as<DebuggerFrame>().onStepHandler();
    if (onStepHandler)
        onStepHandler->trace(trc);
    OnPopHandler* onPopHandler = obj->as<DebuggerFrame>().onPopHandler();
    if (onPopHandler)
        onPopHandler->trace(trc);
}

static DebuggerFrame*
DebuggerFrame_checkThis(JSContext* cx, const CallArgs& args, const char* fnname, bool checkLive)
{
    JSObject* thisobj = NonNullObject(cx, args.thisv());
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &DebuggerFrame::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Frame", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    RootedDebuggerFrame frame(cx, &thisobj->as<DebuggerFrame>());

    /*
     * Forbid Debugger.Frame.prototype, which is of class DebuggerFrame::class_
     * but isn't really a working Debugger.Frame object. The prototype object
     * is distinguished by having a nullptr private value. Also, forbid popped
     * frames.
     */
    if (!frame->getPrivate() &&
        frame->getReservedSlot(JSSLOT_DEBUGFRAME_OWNER).isUndefined())
    {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Frame", fnname, "prototype object");
        return nullptr;
    }

    if (checkLive) {
        if (!DebuggerFrame_requireLive(cx, frame))
            return nullptr;
    }

    return frame;
}

/*
 * To make frequently fired hooks like onEnterFrame more performant,
 * Debugger.Frame methods should not create a FrameIter unless it
 * absolutely needs to. That is, unless the method has to call a method on
 * FrameIter that's otherwise not available on AbstractFramePtr.
 *
 * When a Debugger.Frame is first created, its private slot is set to the
 * AbstractFramePtr itself. The first time the users asks for a
 * FrameIter, we construct one, have it settle on the frame pointed to
 * by the AbstractFramePtr and cache its internal Data in the Debugger.Frame
 * object's private slot. Subsequent uses of the Debugger.Frame object will
 * always create a FrameIter from the cached Data.
 *
 * Methods that only need the AbstractFramePtr should use THIS_FRAME.
 * Methods that need a FrameIterator should use THIS_FRAME_ITER.
 */

#define THIS_DEBUGGER_FRAME(cx, argc, vp, fnname, args, frame)                          \
    CallArgs args = CallArgsFromVp(argc, vp);                                           \
    RootedDebuggerFrame frame(cx, DebuggerFrame_checkThis(cx, args, fnname, true));     \
    if (!frame)                                                                         \
        return false;

#define THIS_FRAME_THISOBJ(cx, argc, vp, fnname, args, thisobj)                       \
    CallArgs args = CallArgsFromVp(argc, vp);                                         \
    RootedNativeObject thisobj(cx, DebuggerFrame_checkThis(cx, args, fnname, true));  \
    if (!thisobj)                                                                     \
        return false

#define THIS_FRAME(cx, argc, vp, fnname, args, thisobj, frame)                 \
    THIS_FRAME_THISOBJ(cx, argc, vp, fnname, args, thisobj);                   \
    AbstractFramePtr frame = AbstractFramePtr::FromRaw(thisobj->getPrivate()); \
    if (frame.isScriptFrameIterData()) {                                       \
        FrameIter iter(*(FrameIter::Data*)(frame.raw()));                      \
        frame = iter.abstractFramePtr();                                       \
    }

#define THIS_FRAME_ITER(cx, argc, vp, fnname, args, thisobj, maybeIter, iter)  \
    THIS_FRAME_THISOBJ(cx, argc, vp, fnname, args, thisobj);                   \
    Maybe<FrameIter> maybeIter;                                                \
    {                                                                          \
        AbstractFramePtr f = AbstractFramePtr::FromRaw(thisobj->getPrivate()); \
        if (f.isScriptFrameIterData()) {                                       \
            maybeIter.emplace(*(FrameIter::Data*)(f.raw()));                   \
        } else {                                                               \
            maybeIter.emplace(cx, FrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK);  \
            FrameIter& iter = *maybeIter;                                      \
            while (!iter.hasUsableAbstractFramePtr() || iter.abstractFramePtr() != f) \
                ++iter;                                                        \
            AbstractFramePtr data = iter.copyDataAsAbstractFramePtr();         \
            if (!data)                                                         \
                return false;                                                  \
            thisobj->setPrivate(data.raw());                                   \
        }                                                                      \
    }                                                                          \
    FrameIter& iter = *maybeIter

#define THIS_FRAME_OWNER(cx, argc, vp, fnname, args, thisobj, frame, dbg)      \
    THIS_FRAME(cx, argc, vp, fnname, args, thisobj, frame);                    \
    Debugger* dbg = Debugger::fromChildJSObject(thisobj)

#define THIS_FRAME_OWNER_ITER(cx, argc, vp, fnname, args, thisobj, maybeIter, iter, dbg) \
    THIS_FRAME_ITER(cx, argc, vp, fnname, args, thisobj, maybeIter, iter);               \
    Debugger* dbg = Debugger::fromChildJSObject(thisobj)

/* static */ bool
DebuggerFrame::typeGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get type", args, frame);

    DebuggerFrameType type = DebuggerFrame::getType(frame);

    JSString* str;
    switch (type) {
      case DebuggerFrameType::Eval:
        str = cx->names().eval;
        break;
      case DebuggerFrameType::Global:
        str = cx->names().global;
        break;
      case DebuggerFrameType::Call:
        str = cx->names().call;
        break;
      case DebuggerFrameType::Module:
        str = cx->names().module;
        break;
      case DebuggerFrameType::WasmCall:
        str = cx->names().wasmcall;
        break;
      default:
        MOZ_CRASH("bad DebuggerFrameType value");
    }

    args.rval().setString(str);
    return true;
}

/* static */ bool
DebuggerFrame::implementationGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get implementation", args, frame);

    DebuggerFrameImplementation implementation = DebuggerFrame::getImplementation(frame);

    const char* s;
    switch (implementation) {
      case DebuggerFrameImplementation::Baseline:
        s = "baseline";
        break;
      case DebuggerFrameImplementation::Ion:
        s = "ion";
        break;
      case DebuggerFrameImplementation::Interpreter:
        s = "interpreter";
        break;
      case DebuggerFrameImplementation::Wasm:
        s = "wasm";
        break;
      default:
        MOZ_CRASH("bad DebuggerFrameImplementation value");
    }

    JSAtom* str = Atomize(cx, s, strlen(s));
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

/* static */ bool
DebuggerFrame::environmentGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get environment", args, frame);

    RootedDebuggerEnvironment result(cx);
    if (!DebuggerFrame::getEnvironment(cx, frame, &result))
        return false;

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerFrame::calleeGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get callee", args, frame);

    RootedDebuggerObject result(cx);
    if (!DebuggerFrame::getCallee(cx, frame, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerFrame::generatorGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get callee", args, frame);

    args.rval().setBoolean(DebuggerFrame::getIsGenerator(frame));
    return true;
}

/* static */ bool
DebuggerFrame::constructingGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get callee", args, frame);

    bool result;
    if (!DebuggerFrame::getIsConstructing(cx, frame, result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

/* static */ bool
DebuggerFrame::thisGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get this", args, frame);

    return DebuggerFrame::getThis(cx, frame, args.rval());
}

/* static */ bool
DebuggerFrame::olderGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get older", args, frame);

    RootedDebuggerFrame result(cx);
    if (!DebuggerFrame::getOlder(cx, frame, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* The getter used for each element of frame.arguments. See DebuggerFrame_getArguments. */
static bool
DebuggerArguments_getArg(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    int32_t i = args.callee().as<JSFunction>().getExtendedSlot(0).toInt32();

    /* Check that the this value is an Arguments object. */
    RootedObject argsobj(cx, NonNullObject(cx, args.thisv()));
    if (!argsobj)
        return false;
    if (argsobj->getClass() != &DebuggerArguments::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Arguments", "getArgument", argsobj->getClass()->name);
        return false;
    }

    /*
     * Put the Debugger.Frame into the this-value slot, then use THIS_FRAME
     * to check that it is still live and get the fp.
     */
    args.setThis(argsobj->as<NativeObject>().getReservedSlot(JSSLOT_DEBUGARGUMENTS_FRAME));
    THIS_FRAME(cx, argc, vp, "get argument", ca2, thisobj, frame);

    // TODO handle wasm frame arguments -- they are not yet reflectable.
    MOZ_ASSERT(!frame.isWasmDebugFrame(), "a wasm frame args");

    /*
     * Since getters can be extracted and applied to other objects,
     * there is no guarantee this object has an ith argument.
     */
    MOZ_ASSERT(i >= 0);
    RootedValue arg(cx);
    RootedScript script(cx);
    if (unsigned(i) < frame.numActualArgs()) {
        script = frame.script();
        {
            AutoCompartment ac(cx, script);
            if (!script->ensureHasAnalyzedArgsUsage(cx))
                return false;
        }
        if (unsigned(i) < frame.numFormalArgs()) {
            for (PositionalFormalParameterIter fi(script); fi; fi++) {
                if (fi.argumentSlot() == unsigned(i)) {
                    // We might've been called before the CallObject was
                    // created.
                    if (fi.closedOver() && frame.hasInitialEnvironment())
                        arg = frame.callObj().aliasedBinding(fi);
                    else
                        arg = frame.unaliasedActual(i, DONT_CHECK_ALIASING);
                    break;
                }
            }
        } else if (script->argsObjAliasesFormals() && frame.hasArgsObj()) {
            arg = frame.argsObj().arg(i);
        } else {
            arg = frame.unaliasedActual(i, DONT_CHECK_ALIASING);
        }
    } else {
        arg.setUndefined();
    }

    if (!Debugger::fromChildJSObject(thisobj)->wrapDebuggeeValue(cx, &arg))
        return false;
    args.rval().set(arg);
    return true;
}

/* static */ DebuggerArguments*
DebuggerArguments::create(JSContext* cx, HandleObject proto, HandleDebuggerFrame frame)
{
    AbstractFramePtr referent = DebuggerFrame::getReferent(frame);

    RootedNativeObject obj(cx, NewNativeObjectWithGivenProto(cx, &DebuggerArguments::class_, proto));
    if (!obj)
        return nullptr;

    SetReservedSlot(obj, FRAME_SLOT, ObjectValue(*frame));

    MOZ_ASSERT(referent.numActualArgs() <= 0x7fffffff);
    unsigned fargc = referent.numActualArgs();
    RootedValue fargcVal(cx, Int32Value(fargc));
    if (!NativeDefineDataProperty(cx, obj, cx->names().length, fargcVal,
                                  JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return nullptr;
    }

    Rooted<jsid> id(cx);
    for (unsigned i = 0; i < fargc; i++) {
        RootedFunction getobj(cx);
        getobj = NewNativeFunction(cx, DebuggerArguments_getArg, 0, nullptr,
                                   gc::AllocKind::FUNCTION_EXTENDED);
        if (!getobj)
            return nullptr;
        id = INT_TO_JSID(i);
        if (!getobj ||
            !NativeDefineAccessorProperty(cx, obj, id,
                                          JS_DATA_TO_FUNC_PTR(GetterOp, getobj.get()), nullptr,
                                          JSPROP_ENUMERATE | JSPROP_GETTER))
        {
            return nullptr;
        }
        getobj->setExtendedSlot(0, Int32Value(i));
    }

    return &obj->as<DebuggerArguments>();
}

/* static */ bool
DebuggerFrame::argumentsGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get arguments", args, frame);

    RootedDebuggerArguments result(cx);
    if (!DebuggerFrame::getArguments(cx, frame, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

static bool
DebuggerFrame_getScript(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_FRAME(cx, argc, vp, "get script", args, thisobj, frame);
    Debugger* debug = Debugger::fromChildJSObject(thisobj);

    RootedObject scriptObject(cx);
    if (frame.isFunctionFrame()) {
        RootedFunction callee(cx, frame.callee());
        MOZ_ASSERT(callee->isInterpreted());
        RootedScript script(cx, callee->nonLazyScript());
        scriptObject = debug->wrapScript(cx, script);
        if (!scriptObject)
            return false;
    } else if (frame.isWasmDebugFrame()) {
        RootedWasmInstanceObject instance(cx, frame.wasmInstance()->object());
        scriptObject = debug->wrapWasmScript(cx, instance);
        if (!scriptObject)
            return false;
    } else {
        /*
         * We got eval, JS_Evaluate*, or JS_ExecuteScript non-function script
         * frames.
         */
        RootedScript script(cx, frame.script());
        scriptObject = debug->wrapScript(cx, script);
        if (!scriptObject)
            return false;
    }

    MOZ_ASSERT(scriptObject);
    args.rval().setObject(*scriptObject);
    return true;
}

/* static */ bool
DebuggerFrame::offsetGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get offset", args, frame);

    size_t result;
    if (!DebuggerFrame::getOffset(cx, frame, result))
        return false;

    args.rval().setNumber(double(result));
    return true;
}

/* static */ bool
DebuggerFrame::liveGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedDebuggerFrame frame(cx, DebuggerFrame_checkThis(cx, args, "get live", false));
    if (!frame)
        return false;

    args.rval().setBoolean(frame->isLive());
    return true;
}

static bool
IsValidHook(const Value& v)
{
    return v.isUndefined() || (v.isObject() && v.toObject().isCallable());
}

/* static */ bool
DebuggerFrame::onStepGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get onStep", args, frame);

    OnStepHandler* handler = frame->onStepHandler();
    RootedValue value(cx, handler ? ObjectOrNullValue(handler->object()) : UndefinedValue());
    MOZ_ASSERT(IsValidHook(value));
    args.rval().set(value);
    return true;
}

/* static */ bool
DebuggerFrame::onStepSetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "set onStep", args, frame);
    if (!args.requireAtLeast(cx, "Debugger.Frame.set onStep", 1))
        return false;
    if (!IsValidHook(args[0])) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }

    ScriptedOnStepHandler* handler = nullptr;
    if (!args[0].isUndefined()) {
        handler = cx->new_<ScriptedOnStepHandler>(&args[0].toObject());
        if (!handler)
            return false;
    }

    if (!DebuggerFrame::setOnStepHandler(cx, frame, handler)) {
        handler->drop();
        return false;
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerFrame::onPopGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "get onPop", args, frame);

    OnPopHandler* handler = frame->onPopHandler();
    RootedValue value(cx, handler ? ObjectValue(*handler->object()) : UndefinedValue());
    MOZ_ASSERT(IsValidHook(value));
    args.rval().set(value);
    return true;
}

/* static */ bool
DebuggerFrame::onPopSetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "set onPop", args, frame);
    if (!args.requireAtLeast(cx, "Debugger.Frame.set onPop", 1))
        return false;
    if (!IsValidHook(args[0])) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }

    ScriptedOnPopHandler* handler = nullptr;
    if (!args[0].isUndefined()) {
        handler = cx->new_<ScriptedOnPopHandler>(&args[0].toObject());
        if (!handler)
            return false;
    }

    frame->setOnPopHandler(handler);

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerFrame::evalMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "eval", args, frame);
    if (!args.requireAtLeast(cx, "Debugger.Frame.prototype.eval", 1))
        return false;

    AutoStableStringChars stableChars(cx);
    if (!ValueToStableChars(cx, "Debugger.Frame.prototype.eval", args[0], stableChars))
        return false;
    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

    EvalOptions options;
   if (!ParseEvalOptions(cx, args.get(1), options))
        return false;

    JSTrapStatus status;
    RootedValue value(cx);
    if (!DebuggerFrame::eval(cx, frame, chars, nullptr, options, status, &value))
        return false;

    return frame->owner()->newCompletionValue(cx, status, value, args.rval());
}

/* static */ bool
DebuggerFrame::evalWithBindingsMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_FRAME(cx, argc, vp, "evalWithBindings", args, frame);
    if (!args.requireAtLeast(cx, "Debugger.Frame.prototype.evalWithBindings", 2))
        return false;

    AutoStableStringChars stableChars(cx);
    if (!ValueToStableChars(cx, "Debugger.Frame.prototype.evalWithBindings", args[0],
                            stableChars))
    {
        return false;
    }
    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

    RootedObject bindings(cx, NonNullObject(cx, args[1]));
    if (!bindings)
        return false;

    EvalOptions options;
    if (!ParseEvalOptions(cx, args.get(2), options))
        return false;

    JSTrapStatus status;
    RootedValue value(cx);
    if (!DebuggerFrame::eval(cx, frame, chars, bindings, options, status, &value))
        return false;

    return frame->owner()->newCompletionValue(cx, status, value, args.rval());
}

/* static */ bool
DebuggerFrame::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                              "Debugger.Frame");
    return false;
}

const JSPropertySpec DebuggerFrame::properties_[] = {
    JS_PSG("arguments", DebuggerFrame::argumentsGetter, 0),
    JS_PSG("callee", DebuggerFrame::calleeGetter, 0),
    JS_PSG("constructing", DebuggerFrame::constructingGetter, 0),
    JS_PSG("environment", DebuggerFrame::environmentGetter, 0),
    JS_PSG("generator", DebuggerFrame::generatorGetter, 0),
    JS_PSG("live", DebuggerFrame::liveGetter, 0),
    JS_PSG("offset", DebuggerFrame::offsetGetter, 0),
    JS_PSG("older", DebuggerFrame::olderGetter, 0),
    JS_PSG("script", DebuggerFrame_getScript, 0),
    JS_PSG("this", DebuggerFrame::thisGetter, 0),
    JS_PSG("type", DebuggerFrame::typeGetter, 0),
    JS_PSG("implementation", DebuggerFrame::implementationGetter, 0),
    JS_PSGS("onStep", DebuggerFrame::onStepGetter, DebuggerFrame::onStepSetter, 0),
    JS_PSGS("onPop", DebuggerFrame::onPopGetter, DebuggerFrame::onPopSetter, 0),
    JS_PS_END
};

const JSFunctionSpec DebuggerFrame::methods_[] = {
    JS_FN("eval", DebuggerFrame::evalMethod, 1, 0),
    JS_FN("evalWithBindings", DebuggerFrame::evalWithBindingsMethod, 1, 0),
    JS_FS_END
};


/*** Debugger.Object *****************************************************************************/

void
DebuggerObject_trace(JSTracer* trc, JSObject* obj)
{
    /*
     * There is a barrier on private pointers, so the Unbarriered marking
     * is okay.
     */
    if (JSObject* referent = (JSObject*) obj->as<NativeObject>().getPrivate()) {
        TraceManuallyBarrieredCrossCompartmentEdge(trc, obj, &referent,
                                                   "Debugger.Object referent");
        obj->as<NativeObject>().setPrivateUnbarriered(referent);
    }
}

static DebuggerObject*
DebuggerObject_checkThis(JSContext* cx, const CallArgs& args, const char* fnname)
{
    JSObject* thisobj = NonNullObject(cx, args.thisv());
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &DebuggerObject::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Object", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    /*
     * Forbid Debugger.Object.prototype, which is of class DebuggerObject::class_
     * but isn't a real working Debugger.Object. The prototype object is
     * distinguished by having no referent.
     */
    DebuggerObject* nthisobj = &thisobj->as<DebuggerObject>();
    if (!nthisobj->getPrivate()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Object", fnname, "prototype object");
        return nullptr;
    }
    return nthisobj;
}

#define THIS_DEBUGOBJECT(cx, argc, vp, fnname, args, object)                         \
    CallArgs args = CallArgsFromVp(argc, vp);                                        \
    RootedDebuggerObject object(cx, DebuggerObject_checkThis(cx, args, fnname));     \
    if (!object)                                                                     \
        return false;                                                                \

#define THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, fnname, args, obj)     \
    CallArgs args = CallArgsFromVp(argc, vp);                          \
    RootedObject obj(cx, DebuggerObject_checkThis(cx, args, fnname));  \
    if (!obj)                                                          \
        return false;                                                  \
    obj = (JSObject*) obj->as<NativeObject>().getPrivate();            \
    MOZ_ASSERT(obj)

#define THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, fnname, args, dbg, obj) \
    CallArgs args = CallArgsFromVp(argc, vp);                                 \
    RootedObject obj(cx, DebuggerObject_checkThis(cx, args, fnname));         \
    if (!obj)                                                                 \
        return false;                                                         \
    Debugger* dbg = Debugger::fromChildJSObject(obj);                         \
    obj = (JSObject*) obj->as<NativeObject>().getPrivate();                   \
    MOZ_ASSERT(obj)

#define THIS_DEBUGOBJECT_PROMISE(cx, argc, vp, fnname, args, obj)                   \
   THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, fnname, args, obj);                      \
   obj = CheckedUnwrap(obj);                                                        \
   if (!obj) {                                                                      \
       ReportAccessDenied(cx);                                                      \
       return false;                                                                \
   }                                                                                \
   if (!obj->is<PromiseObject>()) {                                                 \
       JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,\
                                 "Debugger", "Promise", obj->getClass()->name);     \
       return false;                                                                \
   }                                                                                \
   Rooted<PromiseObject*> promise(cx, &obj->as<PromiseObject>());

#define THIS_DEBUGOBJECT_OWNER_PROMISE(cx, argc, vp, fnname, args, dbg, obj)        \
   THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, fnname, args, dbg, obj);           \
   obj = CheckedUnwrap(obj);                                                        \
   if (!obj) {                                                                      \
       ReportAccessDenied(cx);                                                      \
       return false;                                                                \
   }                                                                                \
   if (!obj->is<PromiseObject>()) {                                                 \
       JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,\
                                 "Debugger", "Promise", obj->getClass()->name);     \
       return false;                                                                \
   }                                                                                \
   Rooted<PromiseObject*> promise(cx, &obj->as<PromiseObject>());

/* static */ bool
DebuggerObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                              "Debugger.Object");
    return false;
}

/* static */ bool
DebuggerObject::callableGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get callable", args, object)

    args.rval().setBoolean(object->isCallable());
    return true;
}

/* static */ bool
DebuggerObject::isBoundFunctionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isBoundFunction", args, object)

    if (!object->isDebuggeeFunction()) {
        args.rval().setUndefined();
        return true;
    }

    args.rval().setBoolean(object->isBoundFunction());
    return true;
}

/* static */ bool
DebuggerObject::isArrowFunctionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isArrowFunction", args, object)

    if (!object->isDebuggeeFunction()) {
        args.rval().setUndefined();
        return true;
    }

    args.rval().setBoolean(object->isArrowFunction());
    return true;
}

/* static */ bool
DebuggerObject::isAsyncFunctionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isAsyncFunction", args, object)

    if (!object->isDebuggeeFunction()) {
        args.rval().setUndefined();
        return true;
    }

    args.rval().setBoolean(object->isAsyncFunction());
    return true;
}

/* static */ bool
DebuggerObject::isGeneratorFunctionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isGeneratorFunction", args, object)

    if (!object->isDebuggeeFunction()) {
        args.rval().setUndefined();
        return true;
    }

    args.rval().setBoolean(object->isGeneratorFunction());
    return true;
}

/* static */ bool
DebuggerObject::protoGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get proto", args, object)

    RootedDebuggerObject result(cx);
    if (!DebuggerObject::getPrototypeOf(cx, object, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerObject::classGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get class", args, object)

    RootedString result(cx);
    if (!DebuggerObject::getClassName(cx, object, &result))
        return false;

    args.rval().setString(result);
    return true;
}

/* static */ bool
DebuggerObject::nameGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get name", args, object)

    if (!object->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    RootedString result(cx, object->name(cx));
    if (result)
        args.rval().setString(result);
    else
        args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::displayNameGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get displayName", args, object)

    if (!object->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    RootedString result(cx, object->displayName(cx));
    if (result)
        args.rval().setString(result);
    else
        args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::parameterNamesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get parameterNames", args, object)

    if (!object->isDebuggeeFunction()) {
        args.rval().setUndefined();
        return true;
    }

    Rooted<StringVector> names(cx, StringVector(cx));
    if (!DebuggerObject::getParameterNames(cx, object, &names))
        return false;

    RootedArrayObject obj(cx, NewDenseFullyAllocatedArray(cx, names.length()));
    if (!obj)
        return false;

    obj->ensureDenseInitializedLength(cx, 0, names.length());
    for (size_t i = 0; i < names.length(); ++i) {
        Value v;
        if (names[i])
            v = StringValue(names[i]);
        else
            v = UndefinedValue();
        obj->setDenseElement(i, v);
    }

    args.rval().setObject(*obj);
    return true;
}

/* static */ bool
DebuggerObject::scriptGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get script", args, dbg, obj);

    if (!obj->is<JSFunction>()) {
        args.rval().setUndefined();
        return true;
    }

    RootedFunction fun(cx, RemoveAsyncWrapper(&obj->as<JSFunction>()));
    if (!fun->isInterpreted()) {
        args.rval().setUndefined();
        return true;
    }

    RootedScript script(cx, GetOrCreateFunctionScript(cx, fun));
    if (!script)
        return false;

    /* Only hand out debuggee scripts. */
    if (!dbg->observesScript(script)) {
        args.rval().setNull();
        return true;
    }

    RootedObject scriptObject(cx, dbg->wrapScript(cx, script));
    if (!scriptObject)
        return false;

    args.rval().setObject(*scriptObject);
    return true;
}

/* static */ bool
DebuggerObject::environmentGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get environment", args, dbg, obj);

    /* Don't bother switching compartments just to check obj's type and get its env. */
    if (!obj->is<JSFunction>()) {
        args.rval().setUndefined();
        return true;
    }

    RootedFunction fun(cx, RemoveAsyncWrapper(&obj->as<JSFunction>()));
    if (!fun->isInterpreted()) {
        args.rval().setUndefined();
        return true;
    }

    /* Only hand out environments of debuggee functions. */
    if (!dbg->observesGlobal(&fun->global())) {
        args.rval().setNull();
        return true;
    }

    Rooted<Env*> env(cx);
    {
        AutoCompartment ac(cx, fun);
        env = GetDebugEnvironmentForFunction(cx, fun);
        if (!env)
            return false;
    }

    return dbg->wrapEnvironment(cx, env, args.rval());
}

/* static */ bool
DebuggerObject::boundTargetFunctionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get boundTargetFunction", args, object)

    if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
        args.rval().setUndefined();
        return true;
    }

    RootedDebuggerObject result(cx);
    if (!DebuggerObject::getBoundTargetFunction(cx, object, &result))
        return false;

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerObject::boundThisGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get boundThis", args, object)

    if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
        args.rval().setUndefined();
        return true;
    }

    return DebuggerObject::getBoundThis(cx, object, args.rval());
}

/* static */ bool
DebuggerObject::boundArgumentsGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get boundArguments", args, object)

    if (!object->isDebuggeeFunction() || !object->isBoundFunction()) {
        args.rval().setUndefined();
        return true;
    }

    Rooted<ValueVector> result(cx, ValueVector(cx));
    if (!DebuggerObject::getBoundArguments(cx, object, &result))
        return false;

    RootedObject obj(cx, NewDenseCopiedArray(cx, result.length(), result.begin()));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* static */ bool
DebuggerObject::globalGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get global", args, object)

    RootedDebuggerObject result(cx);
    if (!DebuggerObject::getGlobal(cx, object, &result))
        return false;

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerObject::allocationSiteGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get allocationSite", args, object)

    RootedObject result(cx);
    if (!DebuggerObject::getAllocationSite(cx, object, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

// Returns the "name" field (see js.msg), which may be used as a unique
// identifier, for any error object with a JSErrorReport or undefined
// if the object has no JSErrorReport.
/* static */ bool
DebuggerObject::errorMessageNameGetter(JSContext *cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get errorMessageName", args, object)

    RootedString result(cx);
    if (!DebuggerObject::getErrorMessageName(cx, object, &result))
        return false;

    if (result)
        args.rval().setString(result);
    else
        args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::errorNotesGetter(JSContext *cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get errorNotes", args, object)

    return DebuggerObject::getErrorNotes(cx, object, args.rval());
}

/* static */ bool
DebuggerObject::errorLineNumberGetter(JSContext *cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get errorLineNumber", args, object)

    return DebuggerObject::getErrorLineNumber(cx, object, args.rval());
}

/* static */ bool
DebuggerObject::errorColumnNumberGetter(JSContext *cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get errorColumnNumber", args, object)

    return DebuggerObject::getErrorColumnNumber(cx, object, args.rval());
}

/* static */ bool
DebuggerObject::isProxyGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isProxy", args, object)

    args.rval().setBoolean(object->isScriptedProxy());
    return true;
}

/* static */ bool
DebuggerObject::proxyTargetGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get proxyTarget", args, object)

    if (!object->isScriptedProxy()) {
        args.rval().setUndefined();
        return true;
    }

    Rooted<DebuggerObject*> result(cx);
    if (!DebuggerObject::getScriptedProxyTarget(cx, object, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerObject::proxyHandlerGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get proxyHandler", args, object)

    if (!object->isScriptedProxy()) {
        args.rval().setUndefined();
        return true;
    }
    Rooted<DebuggerObject*> result(cx);
    if (!DebuggerObject::getScriptedProxyHandler(cx, object, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerObject::isPromiseGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get isPromise", args, object)

    args.rval().setBoolean(object->isPromise());
    return true;
}

/* static */ bool
DebuggerObject::promiseStateGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get promiseState", args, object);

    if (!DebuggerObject::requirePromise(cx, object))
        return false;

    RootedValue result(cx);
    switch (object->promiseState()) {
      case JS::PromiseState::Pending:
        result.setString(cx->names().pending);
        break;
      case JS::PromiseState::Fulfilled:
        result.setString(cx->names().fulfilled);
        break;
      case JS::PromiseState::Rejected:
        result.setString(cx->names().rejected);
        break;
    }

    args.rval().set(result);
    return true;
}

/* static */ bool
DebuggerObject::promiseValueGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get promiseValue", args, object);

    if (!DebuggerObject::requirePromise(cx, object))
        return false;

    if (object->promiseState() != JS::PromiseState::Fulfilled) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROMISE_NOT_FULFILLED);
        return false;
    }

    return DebuggerObject::getPromiseValue(cx, object, args.rval());;
}

/* static */ bool
DebuggerObject::promiseReasonGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get promiseReason", args, object);

    if (!DebuggerObject::requirePromise(cx, object))
        return false;

    if (object->promiseState() != JS::PromiseState::Rejected) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROMISE_NOT_REJECTED);
        return false;
    }

    return DebuggerObject::getPromiseReason(cx, object, args.rval());;
}

/* static */ bool
DebuggerObject::promiseLifetimeGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get promiseLifetime", args, object);

    if (!DebuggerObject::requirePromise(cx, object))
        return false;

    args.rval().setNumber(object->promiseLifetime());
    return true;
}

/* static */ bool
DebuggerObject::promiseTimeToResolutionGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "get promiseTimeToResolution", args, object);

    if (!DebuggerObject::requirePromise(cx, object))
        return false;

    if (object->promiseState() == JS::PromiseState::Pending) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROMISE_NOT_RESOLVED);
        return false;
    }

    args.rval().setNumber(object->promiseTimeToResolution());
    return true;
}

/* static */ bool
DebuggerObject::promiseAllocationSiteGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_PROMISE(cx, argc, vp, "get promiseAllocationSite", args, refobj);

    RootedObject allocSite(cx, promise->allocationSite());
    if (!allocSite) {
        args.rval().setNull();
        return true;
    }

    if (!cx->compartment()->wrap(cx, &allocSite))
        return false;
    args.rval().set(ObjectValue(*allocSite));
    return true;
}

/* static */ bool
DebuggerObject::promiseResolutionSiteGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_PROMISE(cx, argc, vp, "get promiseResolutionSite", args, refobj);

    if (promise->state() == JS::PromiseState::Pending) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_PROMISE_NOT_RESOLVED);
        return false;
    }

    RootedObject resolutionSite(cx, promise->resolutionSite());
    if (!resolutionSite) {
        args.rval().setNull();
        return true;
    }

    if (!cx->compartment()->wrap(cx, &resolutionSite))
        return false;
    args.rval().set(ObjectValue(*resolutionSite));
    return true;
}

/* static */ bool
DebuggerObject::promiseIDGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_PROMISE(cx, argc, vp, "get promiseID", args, refobj);

    args.rval().setNumber(double(promise->getID()));
    return true;
}

/* static */ bool
DebuggerObject::promiseDependentPromisesGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_OWNER_PROMISE(cx, argc, vp, "get promiseDependentPromises", args, dbg, refobj);

    Rooted<GCVector<Value>> values(cx, GCVector<Value>(cx));
    {
        JSAutoCompartment ac(cx, promise);
        if (!promise->dependentPromises(cx, &values))
            return false;
    }
    for (size_t i = 0; i < values.length(); i++) {
        if (!dbg->wrapDebuggeeValue(cx, values[i]))
            return false;
    }
    RootedArrayObject promises(cx);
    if (values.length() == 0)
        promises = NewDenseEmptyArray(cx);
    else
        promises = NewDenseCopiedArray(cx, values.length(), values[0].address());
    if (!promises)
        return false;
    args.rval().setObject(*promises);
    return true;
}

/* static */ bool
DebuggerObject::isExtensibleMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "isExtensible", args, object)

    bool result;
    if (!DebuggerObject::isExtensible(cx, object, result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

/* static */ bool
DebuggerObject::isSealedMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "isSealed", args, object)

    bool result;
    if (!DebuggerObject::isSealed(cx, object, result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

/* static */ bool
DebuggerObject::isFrozenMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "isFrozen", args, object)

    bool result;
    if (!DebuggerObject::isFrozen(cx, object, result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

static JSObject*
IdVectorToArray(JSContext* cx, Handle<IdVector> ids)
{
    Rooted<ValueVector> vals(cx, ValueVector(cx));
    if (!vals.growBy(ids.length()))
        return nullptr;

    for (size_t i = 0, len = ids.length(); i < len; i++) {
         jsid id = ids[i];
         if (JSID_IS_INT(id)) {
             JSString* str = Int32ToString<CanGC>(cx, JSID_TO_INT(id));
             if (!str)
                 return nullptr;
             vals[i].setString(str);
         } else if (JSID_IS_ATOM(id)) {
             vals[i].setString(JSID_TO_STRING(id));
         } else if (JSID_IS_SYMBOL(id)) {
             vals[i].setSymbol(JSID_TO_SYMBOL(id));
         } else {
             MOZ_ASSERT_UNREACHABLE("IdVector must contain only string, int, and Symbol jsids");
         }
    }

    return NewDenseCopiedArray(cx, vals.length(), vals.begin());
}

/* static */ bool
DebuggerObject::getOwnPropertyNamesMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "getOwnPropertyNames", args, object)

    Rooted<IdVector> ids(cx, IdVector(cx));
    if (!DebuggerObject::getOwnPropertyNames(cx, object, &ids))
        return false;

    RootedObject obj(cx, IdVectorToArray(cx, ids));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* static */ bool
DebuggerObject::getOwnPropertySymbolsMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "getOwnPropertySymbols", args, object)

    Rooted<IdVector> ids(cx, IdVector(cx));
    if (!DebuggerObject::getOwnPropertySymbols(cx, object, &ids))
        return false;

    RootedObject obj(cx, IdVectorToArray(cx, ids));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* static */ bool
DebuggerObject::getOwnPropertyDescriptorMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "getOwnPropertyDescriptor", args, object)

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(0), &id))
        return false;

    Rooted<PropertyDescriptor> desc(cx);
    if (!DebuggerObject::getOwnPropertyDescriptor(cx, object, id, &desc))
      return false;

    return JS::FromPropertyDescriptor(cx, desc, args.rval());
}

/* static */ bool
DebuggerObject::preventExtensionsMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "preventExtensions", args, object)

    if (!DebuggerObject::preventExtensions(cx, object))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::sealMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "seal", args, object)

    if (!DebuggerObject::seal(cx, object))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::freezeMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "freeze", args, object)

    if (!DebuggerObject::freeze(cx, object))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::definePropertyMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "defineProperty", args, object)
    if (!args.requireAtLeast(cx, "Debugger.Object.defineProperty", 2))
        return false;

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[0], &id))
        return false;

    Rooted<PropertyDescriptor> desc(cx);
    if (!ToPropertyDescriptor(cx, args[1], false, &desc))
        return false;

    if (!DebuggerObject::defineProperty(cx, object, id, desc))
        return false;

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerObject::definePropertiesMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "defineProperties", args, object);
    if (!args.requireAtLeast(cx, "Debugger.Object.defineProperties", 1))
        return false;

    RootedValue arg(cx, args[0]);
    RootedObject props(cx, ToObject(cx, arg));
    if (!props)
        return false;
    AutoIdVector ids(cx);
    Rooted<PropertyDescriptorVector> descs(cx, PropertyDescriptorVector(cx));
    if (!ReadPropertyDescriptors(cx, props, false, &ids, &descs))
        return false;
    Rooted<IdVector> ids2(cx, IdVector(cx));
    if (!ids2.append(ids.begin(), ids.end()))
       return false;

    if (!DebuggerObject::defineProperties(cx, object, ids2, descs))
        return false;

    args.rval().setUndefined();
    return true;
}

/*
 * This does a non-strict delete, as a matter of API design. The case where the
 * property is non-configurable isn't necessarily exceptional here.
 */
/* static */ bool
DebuggerObject::deletePropertyMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "deleteProperty", args, object)

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(0), &id))
        return false;

    ObjectOpResult result;
    if (!DebuggerObject::deleteProperty(cx, object, id, result))
        return false;

    args.rval().setBoolean(result.ok());
    return true;
}

/* static */ bool
DebuggerObject::callMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "call", callArgs, object);

    RootedValue thisv(cx, callArgs.get(0));

    Rooted<ValueVector> args(cx, ValueVector(cx));
    if (callArgs.length() >= 2) {
        if (!args.growBy(callArgs.length() - 1))
            return false;
        for (size_t i = 1; i < callArgs.length(); ++i)
            args[i - 1].set(callArgs[i]);
    }

    return object->call(cx, object, thisv, args, callArgs.rval());
}

/* static */ bool
DebuggerObject::applyMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "apply", callArgs, object);

    RootedValue thisv(cx, callArgs.get(0));

    Rooted<ValueVector> args(cx, ValueVector(cx));
    if (callArgs.length() >= 2 && !callArgs[1].isNullOrUndefined()) {
        if (!callArgs[1].isObject()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_APPLY_ARGS,
                                      js_apply_str);
            return false;
        }

        RootedObject argsobj(cx, &callArgs[1].toObject());

        unsigned argc = 0;
        if (!GetLengthProperty(cx, argsobj, &argc))
            return false;
        argc = unsigned(Min(argc, ARGS_LENGTH_MAX));

        if (!args.growBy(argc) || !GetElements(cx, argsobj, argc, args.begin()))
            return false;
    }

    return object->call(cx, object, thisv, args, callArgs.rval());
}

/* static */ bool
DebuggerObject::asEnvironmentMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "asEnvironment", args, dbg, referent);
    if (!RequireGlobalObject(cx, args.thisv(), referent))
        return false;

    Rooted<Env*> env(cx);
    {
        AutoCompartment ac(cx, referent);
        env = GetDebugEnvironmentForGlobalLexicalEnvironment(cx);
        if (!env)
            return false;
    }

    return dbg->wrapEnvironment(cx, env, args.rval());
}

// Lookup a binding on the referent's global scope and change it to undefined
// if it is an uninitialized lexical, otherwise do nothing. The method's
// JavaScript return value is true _only_ when an uninitialized lexical has been
// altered, otherwise it is false.
/* static */ bool
DebuggerObject::forceLexicalInitializationByNameMethod(JSContext *cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "forceLexicalInitializationByName", args, object)
    if (!args.requireAtLeast(cx, "Debugger.Object.prototype.forceLexicalInitializationByName", 1))
        return false;

    if (!DebuggerObject::requireGlobal(cx, object))
        return false;

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    bool result;
    if (!DebuggerObject::forceLexicalInitializationByName(cx, object, id, result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

/* static */ bool
DebuggerObject::executeInGlobalMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "executeInGlobal", args, object);
    if (!args.requireAtLeast(cx, "Debugger.Object.prototype.executeInGlobal", 1))
        return false;

    if (!DebuggerObject::requireGlobal(cx, object))
        return false;

    AutoStableStringChars stableChars(cx);
    if (!ValueToStableChars(cx, "Debugger.Object.prototype.executeInGlobal", args[0],
                            stableChars))
    {
        return false;
    }
    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

    EvalOptions options;
    if (!ParseEvalOptions(cx, args.get(1), options))
        return false;

    JSTrapStatus status;
    RootedValue value(cx);
    if (!DebuggerObject::executeInGlobal(cx, object, chars, nullptr, options, status, &value))
        return false;

    return object->owner()->newCompletionValue(cx, status, value, args.rval());
}

/* static */ bool
DebuggerObject::executeInGlobalWithBindingsMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "executeInGlobalWithBindings", args, object);
    if (!args.requireAtLeast(cx, "Debugger.Object.prototype.executeInGlobalWithBindings", 2))
        return false;

    if (!DebuggerObject::requireGlobal(cx, object))
        return false;

    AutoStableStringChars stableChars(cx);
    if (!ValueToStableChars(cx, "Debugger.Object.prototype.executeInGlobalWithBindings", args[0],
                            stableChars))
    {
        return false;
    }
    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();

    RootedObject bindings(cx, NonNullObject(cx, args[1]));
    if (!bindings)
        return false;

    EvalOptions options;
    if (!ParseEvalOptions(cx, args.get(2), options))
        return false;

    JSTrapStatus status;
    RootedValue value(cx);
    if (!DebuggerObject::executeInGlobal(cx, object, chars, bindings, options, status, &value))
        return false;

    return object->owner()->newCompletionValue(cx, status, value, args.rval());
}

/* static */ bool
DebuggerObject::makeDebuggeeValueMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "makeDebuggeeValue", args, object);
    if (!args.requireAtLeast(cx, "Debugger.Object.prototype.makeDebuggeeValue", 1))
        return false;

    return DebuggerObject::makeDebuggeeValue(cx, object, args[0], args.rval());
}

/* static */ bool
DebuggerObject::unsafeDereferenceMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "unsafeDereference", args, object);

    RootedObject result(cx);
    if (!DebuggerObject::unsafeDereference(cx, object, &result))
       return false;

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerObject::unwrapMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGOBJECT(cx, argc, vp, "unwrap", args, object);

    RootedDebuggerObject result(cx);
    if (!DebuggerObject::unwrap(cx, object, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

const JSPropertySpec DebuggerObject::properties_[] = {
    JS_PSG("callable", DebuggerObject::callableGetter, 0),
    JS_PSG("isBoundFunction", DebuggerObject::isBoundFunctionGetter, 0),
    JS_PSG("isArrowFunction", DebuggerObject::isArrowFunctionGetter, 0),
    JS_PSG("isGeneratorFunction", DebuggerObject::isGeneratorFunctionGetter, 0),
    JS_PSG("isAsyncFunction", DebuggerObject::isAsyncFunctionGetter, 0),
    JS_PSG("proto", DebuggerObject::protoGetter, 0),
    JS_PSG("class", DebuggerObject::classGetter, 0),
    JS_PSG("name", DebuggerObject::nameGetter, 0),
    JS_PSG("displayName", DebuggerObject::displayNameGetter, 0),
    JS_PSG("parameterNames", DebuggerObject::parameterNamesGetter, 0),
    JS_PSG("script", DebuggerObject::scriptGetter, 0),
    JS_PSG("environment", DebuggerObject::environmentGetter, 0),
    JS_PSG("boundTargetFunction", DebuggerObject::boundTargetFunctionGetter, 0),
    JS_PSG("boundThis", DebuggerObject::boundThisGetter, 0),
    JS_PSG("boundArguments", DebuggerObject::boundArgumentsGetter, 0),
    JS_PSG("global", DebuggerObject::globalGetter, 0),
    JS_PSG("allocationSite", DebuggerObject::allocationSiteGetter, 0),
    JS_PSG("errorMessageName", DebuggerObject::errorMessageNameGetter, 0),
    JS_PSG("errorNotes", DebuggerObject::errorNotesGetter, 0),
    JS_PSG("errorLineNumber", DebuggerObject::errorLineNumberGetter, 0),
    JS_PSG("errorColumnNumber", DebuggerObject::errorColumnNumberGetter, 0),
    JS_PSG("isProxy", DebuggerObject::isProxyGetter, 0),
    JS_PSG("proxyTarget", DebuggerObject::proxyTargetGetter, 0),
    JS_PSG("proxyHandler", DebuggerObject::proxyHandlerGetter, 0),
    JS_PS_END
};

const JSPropertySpec DebuggerObject::promiseProperties_[] = {
    JS_PSG("isPromise", DebuggerObject::isPromiseGetter, 0),
    JS_PSG("promiseState", DebuggerObject::promiseStateGetter, 0),
    JS_PSG("promiseValue", DebuggerObject::promiseValueGetter, 0),
    JS_PSG("promiseReason", DebuggerObject::promiseReasonGetter, 0),
    JS_PSG("promiseLifetime", DebuggerObject::promiseLifetimeGetter, 0),
    JS_PSG("promiseTimeToResolution", DebuggerObject::promiseTimeToResolutionGetter, 0),
    JS_PSG("promiseAllocationSite", DebuggerObject::promiseAllocationSiteGetter, 0),
    JS_PSG("promiseResolutionSite", DebuggerObject::promiseResolutionSiteGetter, 0),
    JS_PSG("promiseID", DebuggerObject::promiseIDGetter, 0),
    JS_PSG("promiseDependentPromises", DebuggerObject::promiseDependentPromisesGetter, 0),
    JS_PS_END
};

const JSFunctionSpec DebuggerObject::methods_[] = {
    JS_FN("isExtensible", DebuggerObject::isExtensibleMethod, 0, 0),
    JS_FN("isSealed", DebuggerObject::isSealedMethod, 0, 0),
    JS_FN("isFrozen", DebuggerObject::isFrozenMethod, 0, 0),
    JS_FN("getOwnPropertyNames", DebuggerObject::getOwnPropertyNamesMethod, 0, 0),
    JS_FN("getOwnPropertySymbols", DebuggerObject::getOwnPropertySymbolsMethod, 0, 0),
    JS_FN("getOwnPropertyDescriptor", DebuggerObject::getOwnPropertyDescriptorMethod, 1, 0),
    JS_FN("preventExtensions", DebuggerObject::preventExtensionsMethod, 0, 0),
    JS_FN("seal", DebuggerObject::sealMethod, 0, 0),
    JS_FN("freeze", DebuggerObject::freezeMethod, 0, 0),
    JS_FN("defineProperty", DebuggerObject::definePropertyMethod, 2, 0),
    JS_FN("defineProperties", DebuggerObject::definePropertiesMethod, 1, 0),
    JS_FN("deleteProperty", DebuggerObject::deletePropertyMethod, 1, 0),
    JS_FN("call", DebuggerObject::callMethod, 0, 0),
    JS_FN("apply", DebuggerObject::applyMethod, 0, 0),
    JS_FN("asEnvironment", DebuggerObject::asEnvironmentMethod, 0, 0),
    JS_FN("forceLexicalInitializationByName", DebuggerObject::forceLexicalInitializationByNameMethod, 1, 0),
    JS_FN("executeInGlobal", DebuggerObject::executeInGlobalMethod, 1, 0),
    JS_FN("executeInGlobalWithBindings", DebuggerObject::executeInGlobalWithBindingsMethod, 2, 0),
    JS_FN("makeDebuggeeValue", DebuggerObject::makeDebuggeeValueMethod, 1, 0),
    JS_FN("unsafeDereference", DebuggerObject::unsafeDereferenceMethod, 0, 0),
    JS_FN("unwrap", DebuggerObject::unwrapMethod, 0, 0),
    JS_FS_END
};

/* static */ NativeObject*
DebuggerObject::initClass(JSContext* cx, HandleObject obj, HandleObject debugCtor)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));

    RootedNativeObject objectProto(cx, InitClass(cx, debugCtor, objProto, &class_,
                                                 construct, 0, properties_,
                                                 methods_, nullptr, nullptr));

    if (!objectProto)
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, objectProto, promiseProperties_, nullptr))
        return nullptr;

    return objectProto;
}

/* static */ DebuggerObject*
DebuggerObject::create(JSContext* cx, HandleObject proto, HandleObject referent,
                       HandleNativeObject debugger)
{
  NewObjectKind newKind = IsInsideNursery(referent) ? GenericObject : TenuredObject;
  JSObject* obj = NewObjectWithGivenProto(cx, &DebuggerObject::class_, proto, newKind);
  if (!obj)
    return nullptr;

  DebuggerObject& object = obj->as<DebuggerObject>();
  object.setPrivateGCThing(referent);
  object.setReservedSlot(JSSLOT_DEBUGOBJECT_OWNER, ObjectValue(*debugger));

  return &object;
}

bool
DebuggerObject::isCallable() const
{
    return referent()->isCallable();
}

bool
DebuggerObject::isFunction() const
{
    return referent()->is<JSFunction>();
}

bool
DebuggerObject::isDebuggeeFunction() const
{
    return referent()->is<JSFunction>() &&
           owner()->observesGlobal(&referent()->as<JSFunction>().global());
}

bool
DebuggerObject::isBoundFunction() const
{
    MOZ_ASSERT(isDebuggeeFunction());

    return referent()->isBoundFunction();
}

bool
DebuggerObject::isArrowFunction() const
{
    MOZ_ASSERT(isDebuggeeFunction());

    return RemoveAsyncWrapper(&referent()->as<JSFunction>())->isArrow();
}

bool
DebuggerObject::isAsyncFunction() const
{
    MOZ_ASSERT(isDebuggeeFunction());

    return RemoveAsyncWrapper(&referent()->as<JSFunction>())->isAsync();
}

bool
DebuggerObject::isGeneratorFunction() const
{
    MOZ_ASSERT(isDebuggeeFunction());

    JSFunction* fun = RemoveAsyncWrapper(&referent()->as<JSFunction>());
    return fun->isGenerator();
}

bool
DebuggerObject::isGlobal() const
{
    return referent()->is<GlobalObject>();
}

bool
DebuggerObject::isScriptedProxy() const
{
    return js::IsScriptedProxy(referent());
}

bool
DebuggerObject::isPromise() const
{
    JSObject* referent = this->referent();

    if (IsCrossCompartmentWrapper(referent)) {
        referent = CheckedUnwrap(referent);
        if (!referent)
            return false;
    }

    return referent->is<PromiseObject>();
}

/* static */ bool
DebuggerObject::getClassName(JSContext* cx, HandleDebuggerObject object,
                             MutableHandleString result)
{
    RootedObject referent(cx, object->referent());

    const char* className;
    {
        AutoCompartment ac(cx, referent);
        className = GetObjectClassName(cx, referent);
    }

    JSAtom* str = Atomize(cx, className, strlen(className));
    if (!str)
        return false;

    result.set(str);
    return true;
}

/* static */ bool
DebuggerObject::getGlobal(JSContext* cx, HandleDebuggerObject object,
                          MutableHandleDebuggerObject result)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    RootedObject global(cx, &referent->global());
    return dbg->wrapDebuggeeObject(cx, global, result);
}

JSAtom*
DebuggerObject::name(JSContext* cx) const
{
    MOZ_ASSERT(isFunction());

    JSAtom* atom = referent()->as<JSFunction>().explicitName();
    if (atom)
        cx->markAtom(atom);
    return atom;
}

JSAtom*
DebuggerObject::displayName(JSContext* cx) const
{
    MOZ_ASSERT(isFunction());

    JSAtom* atom = referent()->as<JSFunction>().displayAtom();
    if (atom)
        cx->markAtom(atom);
    return atom;
}

JS::PromiseState
DebuggerObject::promiseState() const
{
    return promise()->state();
}

double
DebuggerObject::promiseLifetime() const
{
    return promise()->lifetime();
}

double
DebuggerObject::promiseTimeToResolution() const
{
    MOZ_ASSERT(promiseState() != JS::PromiseState::Pending);

    return promise()->timeToResolution();
}

/* static */ bool
DebuggerObject::getParameterNames(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandle<StringVector> result)
{
    MOZ_ASSERT(object->isDebuggeeFunction());

    RootedFunction referent(cx, RemoveAsyncWrapper(&object->referent()->as<JSFunction>()));

    if (!result.growBy(referent->nargs()))
        return false;
    if (referent->isInterpreted()) {
        RootedScript script(cx, GetOrCreateFunctionScript(cx, referent));
        if (!script)
            return false;

        MOZ_ASSERT(referent->nargs() == script->numArgs());

        if (referent->nargs() > 0) {
            PositionalFormalParameterIter fi(script);
            for (size_t i = 0; i < referent->nargs(); i++, fi++) {
                MOZ_ASSERT(fi.argumentSlot() == i);
                JSAtom* atom = fi.name();
                if (atom)
                    cx->markAtom(atom);
                result[i].set(atom);
            }
        }
    } else {
        for (size_t i = 0; i < referent->nargs(); i++)
            result[i].set(nullptr);
    }

    return true;
}

/* static */ bool
DebuggerObject::getBoundTargetFunction(JSContext* cx, HandleDebuggerObject object,
                                       MutableHandleDebuggerObject result)
{
    MOZ_ASSERT(object->isBoundFunction());

    RootedFunction referent(cx, &object->referent()->as<JSFunction>());
    Debugger* dbg = object->owner();

    RootedObject target(cx, referent->getBoundFunctionTarget());
    return dbg->wrapDebuggeeObject(cx, target, result);
}

/* static */ bool
DebuggerObject::getBoundThis(JSContext* cx, HandleDebuggerObject object,
                             MutableHandleValue result)
{
    MOZ_ASSERT(object->isBoundFunction());

    RootedFunction referent(cx, &object->referent()->as<JSFunction>());
    Debugger* dbg = object->owner();

    result.set(referent->getBoundFunctionThis());
    return dbg->wrapDebuggeeValue(cx, result);
}

/* static */ bool
DebuggerObject::getBoundArguments(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandle<ValueVector> result)
{
    MOZ_ASSERT(object->isBoundFunction());

    RootedFunction referent(cx, &object->referent()->as<JSFunction>());
    Debugger* dbg = object->owner();

    size_t length = referent->getBoundFunctionArgumentCount();
    if (!result.resize(length))
        return false;
    for (size_t i = 0; i < length; i++) {
        result[i].set(referent->getBoundFunctionArgument(i));
        if (!dbg->wrapDebuggeeValue(cx, result[i]))
            return false;
    }
    return true;
}

/* static */ SavedFrame*
Debugger::getObjectAllocationSite(JSObject& obj)
{
    JSObject* metadata = GetAllocationMetadata(&obj);
    if (!metadata)
        return nullptr;

    MOZ_ASSERT(!metadata->is<WrapperObject>());
    return SavedFrame::isSavedFrameAndNotProto(*metadata)
        ? &metadata->as<SavedFrame>()
        : nullptr;
}

/* static */ bool
DebuggerObject::getAllocationSite(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandleObject result)
{
    RootedObject referent(cx, object->referent());

    RootedObject allocSite(cx, Debugger::getObjectAllocationSite(*referent));
    if (!cx->compartment()->wrap(cx, &allocSite))
        return false;

    result.set(allocSite);
    return true;
}

/* static */ bool
DebuggerObject::getErrorReport(JSContext* cx, HandleObject maybeError, JSErrorReport*& report)
{
    JSObject* obj = maybeError;
    if (IsCrossCompartmentWrapper(obj))
        obj = CheckedUnwrap(obj);

    if (!obj) {
        ReportAccessDenied(cx);
        return false;
    }

    if (!obj->is<ErrorObject>()) {
        report = nullptr;
        return true;
    }

    report = obj->as<ErrorObject>().getErrorReport();
    return true;
}

/* static */ bool
DebuggerObject::getErrorMessageName(JSContext* cx, HandleDebuggerObject object,
                                    MutableHandleString result)
{
    RootedObject referent(cx, object->referent());
    JSErrorReport* report;
    if (!getErrorReport(cx, referent, report))
        return false;

    if (!report) {
        result.set(nullptr);
        return true;
    }

    const JSErrorFormatString* efs = GetErrorMessage(nullptr, report->errorNumber);
    if (!efs) {
        result.set(nullptr);
        return true;
    }

    RootedString str(cx, JS_NewStringCopyZ(cx, efs->name));
    if (!cx->compartment()->wrap(cx, &str))
        return false;

    result.set(str);
    return true;
}

/* static */ bool
DebuggerObject::getErrorNotes(JSContext* cx, HandleDebuggerObject object,
                              MutableHandleValue result)
{
    RootedObject referent(cx, object->referent());
    JSErrorReport* report;
    if (!getErrorReport(cx, referent, report))
        return false;

    if (!report) {
        result.setUndefined();
        return true;
    }

    RootedObject errorNotesArray(cx, CreateErrorNotesArray(cx, report));
    if (!errorNotesArray)
        return false;

    if (!cx->compartment()->wrap(cx, &errorNotesArray))
        return false;
    result.setObject(*errorNotesArray);
    return true;
}

/* static */ bool
DebuggerObject::getErrorLineNumber(JSContext* cx, HandleDebuggerObject object,
                                   MutableHandleValue result)
{
    RootedObject referent(cx, object->referent());
    JSErrorReport* report;
    if (!getErrorReport(cx, referent, report))
        return false;

    if (!report) {
        result.setUndefined();
        return true;
    }

    result.setNumber(report->lineno);
    return true;
}

/* static */ bool
DebuggerObject::getErrorColumnNumber(JSContext* cx, HandleDebuggerObject object,
                                     MutableHandleValue result)
{
    RootedObject referent(cx, object->referent());
    JSErrorReport* report;
    if (!getErrorReport(cx, referent, report))
        return false;

    if (!report) {
        result.setUndefined();
        return true;
    }

    result.setNumber(report->column);
    return true;
}

/* static */ bool
DebuggerObject::getPromiseValue(JSContext* cx, HandleDebuggerObject object,
                                MutableHandleValue result)
{
    MOZ_ASSERT(object->promiseState() == JS::PromiseState::Fulfilled);

    result.set(object->promise()->value());
    return object->owner()->wrapDebuggeeValue(cx, result);
}

/* static */ bool
DebuggerObject::getPromiseReason(JSContext* cx, HandleDebuggerObject object,
                                 MutableHandleValue result)
{
    MOZ_ASSERT(object->promiseState() == JS::PromiseState::Rejected);

    result.set(object->promise()->reason());
    return object->owner()->wrapDebuggeeValue(cx, result);
}

/* static */ bool
DebuggerObject::isExtensible(JSContext* cx, HandleDebuggerObject object, bool& result)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);
    ErrorCopier ec(ac);
    return IsExtensible(cx, referent, &result);
}

/* static */ bool
DebuggerObject::isSealed(JSContext* cx, HandleDebuggerObject object, bool& result)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    ErrorCopier ec(ac);
    return TestIntegrityLevel(cx, referent, IntegrityLevel::Sealed, &result);
}

/* static */ bool
DebuggerObject::isFrozen(JSContext* cx, HandleDebuggerObject object, bool& result)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    ErrorCopier ec(ac);
    return TestIntegrityLevel(cx, referent, IntegrityLevel::Frozen, &result);
}

/* static */ bool
DebuggerObject::getPrototypeOf(JSContext* cx, HandleDebuggerObject object,
                               MutableHandleDebuggerObject result)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    RootedObject proto(cx);
    {
        AutoCompartment ac(cx, referent);
        if (!GetPrototype(cx, referent, &proto))
            return false;
    }

    if (!proto) {
        result.set(nullptr);
        return true;
    }

    return dbg->wrapDebuggeeObject(cx, proto, result);
}

/* static */ bool
DebuggerObject::getOwnPropertyNames(JSContext* cx, HandleDebuggerObject object,
                                    MutableHandle<IdVector> result)
{
    RootedObject referent(cx, object->referent());

    AutoIdVector ids(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);

        ErrorCopier ec(ac);
        if (!GetPropertyKeys(cx, referent, JSITER_OWNONLY | JSITER_HIDDEN, &ids))
            return false;
    }

    for (size_t i = 0; i < ids.length(); i++)
        cx->markId(ids[i]);

    return result.append(ids.begin(), ids.end());
}

/* static */ bool
DebuggerObject::getOwnPropertySymbols(JSContext* cx, HandleDebuggerObject object,
                                      MutableHandle<IdVector> result)
{
    RootedObject referent(cx, object->referent());

    AutoIdVector ids(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);

        ErrorCopier ec(ac);
        if (!GetPropertyKeys(cx, referent,
                             JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS | JSITER_SYMBOLSONLY,
                             &ids))
            return false;
    }

    for (size_t i = 0; i < ids.length(); i++)
        cx->markId(ids[i]);

    return result.append(ids.begin(), ids.end());
}

/* static */ bool
DebuggerObject::getOwnPropertyDescriptor(JSContext* cx, HandleDebuggerObject object,
                                         HandleId id, MutableHandle<PropertyDescriptor> desc)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    /* Bug: This can cause the debuggee to run! */
    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);
        cx->markId(id);

        ErrorCopier ec(ac);
        if (!GetOwnPropertyDescriptor(cx, referent, id, desc))
            return false;
    }

    if (desc.object()) {
        /* Rewrap the debuggee values in desc for the debugger. */
        if (!dbg->wrapDebuggeeValue(cx, desc.value()))
            return false;

        if (desc.hasGetterObject()) {
            RootedValue get(cx, ObjectOrNullValue(desc.getterObject()));
            if (!dbg->wrapDebuggeeValue(cx, &get))
                return false;
            desc.setGetterObject(get.toObjectOrNull());
        }
        if (desc.hasSetterObject()) {
            RootedValue set(cx, ObjectOrNullValue(desc.setterObject()));
            if (!dbg->wrapDebuggeeValue(cx, &set))
                return false;
            desc.setSetterObject(set.toObjectOrNull());
        }

        // Avoid tripping same-compartment assertions in JS::FromPropertyDescriptor().
        desc.object().set(object);
    }

    return true;
}

/* static */ bool
DebuggerObject::preventExtensions(JSContext* cx, HandleDebuggerObject object)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    ErrorCopier ec(ac);
    return PreventExtensions(cx, referent);
}

/* static */ bool
DebuggerObject::seal(JSContext* cx, HandleDebuggerObject object)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    ErrorCopier ec(ac);
    return SetIntegrityLevel(cx, referent, IntegrityLevel::Sealed);
}

/* static */ bool
DebuggerObject::freeze(JSContext* cx, HandleDebuggerObject object)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    ErrorCopier ec(ac);
    return SetIntegrityLevel(cx, referent, IntegrityLevel::Frozen);
}

/* static */ bool
DebuggerObject::defineProperty(JSContext* cx, HandleDebuggerObject object, HandleId id,
                               Handle<PropertyDescriptor> desc_)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    Rooted<PropertyDescriptor> desc(cx, desc_);
    if (!dbg->unwrapPropertyDescriptor(cx, referent, &desc))
        return false;
    JS_TRY_OR_RETURN_FALSE(cx, CheckPropertyDescriptorAccessors(cx, desc));

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);
    if (!cx->compartment()->wrap(cx, &desc))
        return false;
    cx->markId(id);

    ErrorCopier ec(ac);
    if (!DefineProperty(cx, referent, id, desc))
        return false;

    return true;
}

/* static */ bool
DebuggerObject::defineProperties(JSContext* cx, HandleDebuggerObject object,
                                 Handle<IdVector> ids,
                                 Handle<PropertyDescriptorVector> descs_)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    Rooted<PropertyDescriptorVector> descs(cx, PropertyDescriptorVector(cx));
    if (!descs.append(descs_.begin(), descs_.end()))
        return false;
    for (size_t i = 0; i < descs.length(); i++) {
        if (!dbg->unwrapPropertyDescriptor(cx, referent, descs[i]))
            return false;
        JS_TRY_OR_RETURN_FALSE(cx, CheckPropertyDescriptorAccessors(cx, descs[i]));
    }

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);
    for (size_t i = 0; i < descs.length(); i++) {
        if (!cx->compartment()->wrap(cx, descs[i]))
            return false;
        cx->markId(ids[i]);
    }

    ErrorCopier ec(ac);
    for (size_t i = 0; i < descs.length(); i++) {
        if (!DefineProperty(cx, referent, ids[i], descs[i]))
            return false;
    }

    return true;
}

/* static */ bool
DebuggerObject::deleteProperty(JSContext* cx, HandleDebuggerObject object, HandleId id,
                               ObjectOpResult& result)
{
    RootedObject referent(cx, object->referent());

    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);

    cx->markId(id);

    ErrorCopier ec(ac);
    return DeleteProperty(cx, referent, id, result);
}

/* static */ bool
DebuggerObject::call(JSContext* cx, HandleDebuggerObject object, HandleValue thisv_,
                     Handle<ValueVector> args, MutableHandleValue result)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    if (!referent->isCallable()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Object", "call", referent->getClass()->name);
        return false;
    }

    RootedValue calleev(cx, ObjectValue(*referent));

    /*
     * Unwrap Debugger.Objects. This happens in the debugger's compartment since
     * that is where any exceptions must be reported.
     */
    RootedValue thisv(cx, thisv_);
    if (!dbg->unwrapDebuggeeValue(cx, &thisv))
        return false;
    Rooted<ValueVector> args2(cx, ValueVector(cx));
    if (!args2.append(args.begin(), args.end()))
        return false;
    for (unsigned i = 0; i < args2.length(); ++i) {
        if (!dbg->unwrapDebuggeeValue(cx, args2[i]))
            return false;
    }

    /*
     * Enter the debuggee compartment and rewrap all input value for that compartment.
     * (Rewrapping always takes place in the destination compartment.)
     */
    Maybe<AutoCompartment> ac;
    ac.emplace(cx, referent);
    if (!cx->compartment()->wrap(cx, &calleev) || !cx->compartment()->wrap(cx, &thisv))
        return false;
    for (unsigned i = 0; i < args2.length(); ++i) {
        if (!cx->compartment()->wrap(cx, args2[i]))
             return false;
    }

    /*
     * Call the function. Use receiveCompletionValue to return to the debugger
     * compartment and populate args.rval().
     */
    LeaveDebuggeeNoExecute nnx(cx);

    bool ok;
    {
        InvokeArgs invokeArgs(cx);

        ok = invokeArgs.init(cx, args2.length());
        if (ok) {
            for (size_t i = 0; i < args2.length(); ++i)
                invokeArgs[i].set(args2[i]);

            ok = js::Call(cx, calleev, thisv, invokeArgs, result);
        }
    }

    return dbg->receiveCompletionValue(ac, ok, result, result);
}

/* static */ bool
DebuggerObject::forceLexicalInitializationByName(JSContext* cx, HandleDebuggerObject object,
                                                 HandleId id, bool& result)
{
    if (!JSID_IS_STRING(id)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "Debugger.Object.prototype.forceLexicalInitializationByName",
                                  "string", InformalValueTypeName(IdToValue(id)));
        return false;
    }

    MOZ_ASSERT(object->isGlobal());

    Rooted<GlobalObject*> referent(cx, &object->referent()->as<GlobalObject>());

    RootedObject globalLexical(cx, &referent->lexicalEnvironment());
    RootedObject pobj(cx);
    Rooted<PropertyResult> prop(cx);
    if (!LookupProperty(cx, globalLexical, id, &pobj, &prop))
        return false;

    result = false;
    if (prop) {
        MOZ_ASSERT(prop.isNativeProperty());
        Shape* shape = prop.shape();
        Value v = globalLexical->as<NativeObject>().getSlot(shape->slot());
        if (shape->isDataProperty() && v.isMagic() && v.whyMagic() == JS_UNINITIALIZED_LEXICAL) {
            globalLexical->as<NativeObject>().setSlot(shape->slot(), UndefinedValue());
            result = true;
        }
    }

    return true;
}

/* static */ bool
DebuggerObject::executeInGlobal(JSContext* cx, HandleDebuggerObject object,
                                mozilla::Range<const char16_t> chars, HandleObject bindings,
                                const EvalOptions& options, JSTrapStatus& status,
                                MutableHandleValue value)
{
    MOZ_ASSERT(object->isGlobal());

    Rooted<GlobalObject*> referent(cx, &object->referent()->as<GlobalObject>());
    Debugger* dbg = object->owner();

    RootedObject globalLexical(cx, &referent->lexicalEnvironment());
    return DebuggerGenericEval(cx, chars, bindings, options, status, value, dbg, globalLexical,
                               nullptr);
}

/* static */ bool
DebuggerObject::makeDebuggeeValue(JSContext* cx, HandleDebuggerObject object,
                                  HandleValue value_, MutableHandleValue result)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    RootedValue value(cx, value_);

    /* Non-objects are already debuggee values. */
    if (value.isObject()) {
        // Enter this Debugger.Object's referent's compartment, and wrap the
        // argument as appropriate for references from there.
        {
            AutoCompartment ac(cx, referent);
            if (!cx->compartment()->wrap(cx, &value))
                return false;
        }

        // Back in the debugger's compartment, produce a new Debugger.Object
        // instance referring to the wrapped argument.
        if (!dbg->wrapDebuggeeValue(cx, &value))
            return false;
    }

    result.set(value);
    return true;
}

/* static */ bool
DebuggerObject::unsafeDereference(JSContext* cx, HandleDebuggerObject object,
                                  MutableHandleObject result)
{
    RootedObject referent(cx, object->referent());

    if (!cx->compartment()->wrap(cx, &referent))
        return false;

    // Wrapping should return the WindowProxy.
    MOZ_ASSERT(!IsWindow(referent));

    result.set(referent);
    return true;
}

/* static */ bool
DebuggerObject::unwrap(JSContext* cx, HandleDebuggerObject object,
                       MutableHandleDebuggerObject result)
{
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();

    RootedObject unwrapped(cx, UnwrapOneChecked(referent));
    if (!unwrapped) {
        result.set(nullptr);
        return true;
    }

    // Don't allow unwrapping to create a D.O whose referent is in an
    // invisible-to-Debugger global. (If our referent is a *wrapper* to such,
    // and the wrapper is in a visible compartment, that's fine.)
    JSCompartment* unwrappedCompartment = unwrapped->compartment();
    if (unwrappedCompartment->creationOptions().invisibleToDebugger()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_INVISIBLE_COMPARTMENT);
        return false;
    }

    return dbg->wrapDebuggeeObject(cx, unwrapped, result);
}

/* static */ bool
DebuggerObject::requireGlobal(JSContext* cx, HandleDebuggerObject object)
{
    if (!object->isGlobal()) {
        RootedObject referent(cx, object->referent());

        const char* isWrapper = "";
        const char* isWindowProxy = "";

        /* Help the poor programmer by pointing out wrappers around globals... */
        if (referent->is<WrapperObject>()) {
            referent = js::UncheckedUnwrap(referent);
            isWrapper = "a wrapper around ";
        }

        /* ... and WindowProxies around Windows. */
        if (IsWindowProxy(referent)) {
            referent = ToWindowIfWindowProxy(referent);
            isWindowProxy = "a WindowProxy referring to ";
        }

        RootedValue dbgobj(cx, ObjectValue(*object));
        if (referent->is<GlobalObject>()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_WRAPPER_IN_WAY,
                                  JSDVG_SEARCH_STACK, dbgobj, nullptr,
                                  isWrapper, isWindowProxy);
        } else {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                                  JSDVG_SEARCH_STACK, dbgobj, nullptr,
                                  "a global object", nullptr);
        }
        return false;
    }

    return true;
}

/* static */ bool
DebuggerObject::requirePromise(JSContext* cx, HandleDebuggerObject object)
{
   RootedObject referent(cx, object->referent());

   if (IsCrossCompartmentWrapper(referent)) {
       referent = CheckedUnwrap(referent);
       if (!referent) {
           ReportAccessDenied(cx);
           return false;
       }
   }

   if (!referent->is<PromiseObject>()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                "Debugger", "Promise", object->getClass()->name);
      return false;
   }

   return true;
}

/* static */ bool
DebuggerObject::getScriptedProxyTarget(JSContext* cx, HandleDebuggerObject object,
                                       MutableHandleDebuggerObject result)
{
    MOZ_ASSERT(object->isScriptedProxy());
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();
    RootedObject unwrapped(cx, js::GetProxyTargetObject(referent));
    if(!unwrapped) {
      result.set(nullptr);
      return true;
    }
    return dbg->wrapDebuggeeObject(cx, unwrapped, result);
}

/* static */ bool
DebuggerObject::getScriptedProxyHandler(JSContext* cx, HandleDebuggerObject object,
                                        MutableHandleDebuggerObject result)
{
    MOZ_ASSERT(object->isScriptedProxy());
    RootedObject referent(cx, object->referent());
    Debugger* dbg = object->owner();
    RootedObject unwrapped(cx, ScriptedProxyHandler::handlerObject(referent));
    if(!unwrapped) {
      result.set(nullptr);
      return true;
    }
    return dbg->wrapDebuggeeObject(cx, unwrapped, result);
}


/*** Debugger.Environment ************************************************************************/

void
DebuggerEnv_trace(JSTracer* trc, JSObject* obj)
{
    /*
     * There is a barrier on private pointers, so the Unbarriered marking
     * is okay.
     */
    if (Env* referent = (JSObject*) obj->as<NativeObject>().getPrivate()) {
        TraceManuallyBarrieredCrossCompartmentEdge(trc, obj, &referent,
                                                   "Debugger.Environment referent");
        obj->as<NativeObject>().setPrivateUnbarriered(referent);
    }
}

static DebuggerEnvironment*
DebuggerEnvironment_checkThis(JSContext* cx, const CallArgs& args, const char* fnname,
                              bool requireDebuggee)
{
    JSObject* thisobj = NonNullObject(cx, args.thisv());
    if (!thisobj)
        return nullptr;
    if (thisobj->getClass() != &DebuggerEnvironment::class_) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Environment", fnname, thisobj->getClass()->name);
        return nullptr;
    }

    /*
     * Forbid Debugger.Environment.prototype, which is of class DebuggerEnvironment::class_
     * but isn't a real working Debugger.Environment. The prototype object is
     * distinguished by having no referent.
     */
    DebuggerEnvironment* nthisobj = &thisobj->as<DebuggerEnvironment>();
    if (!nthisobj->getPrivate()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                  "Debugger.Environment", fnname, "prototype object");
        return nullptr;
    }

    /*
     * Forbid access to Debugger.Environment objects that are not debuggee
     * environments.
     */
    if (requireDebuggee) {
        Rooted<Env*> env(cx, static_cast<Env*>(nthisobj->getPrivate()));
        if (!Debugger::fromChildJSObject(nthisobj)->observesGlobal(&env->global())) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_DEBUGGEE,
                                      "Debugger.Environment", "environment");
            return nullptr;
        }
    }

    return nthisobj;
}

#define THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, fnname, args, environment)                                 \
    CallArgs args = CallArgsFromVp(argc, vp);                                                              \
    Rooted<DebuggerEnvironment*> environment(cx, DebuggerEnvironment_checkThis(cx, args, fnname, false));  \
    if (!environment)                                                                                      \
        return false;                                                                                      \

/* static */ bool
DebuggerEnvironment::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                              "Debugger.Environment");
    return false;
}

static bool
IsDeclarative(Env* env)
{
    return env->is<DebugEnvironmentProxy>() &&
           env->as<DebugEnvironmentProxy>().isForDeclarative();
}

template <typename T>
static bool
IsDebugEnvironmentWrapper(Env* env)
{
    return env->is<DebugEnvironmentProxy>() &&
           env->as<DebugEnvironmentProxy>().environment().is<T>();
}

bool
DebuggerEnvironment::typeGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get type", args, environment);

    if (!environment->requireDebuggee(cx))
        return false;

    DebuggerEnvironmentType type = environment->type();

    const char* s;
    switch (type) {
      case DebuggerEnvironmentType::Declarative:
        s = "declarative";
        break;
      case DebuggerEnvironmentType::With:
        s = "with";
        break;
      case DebuggerEnvironmentType::Object:
        s = "object";
        break;
    }

    JSAtom* str = Atomize(cx, s, strlen(s), PinAtom);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

/* static */ bool
DebuggerEnvironment::parentGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get type", args, environment);

    if (!environment->requireDebuggee(cx))
        return false;

    RootedDebuggerEnvironment result(cx);
    if (!environment->getParent(cx, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerEnvironment::objectGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get type", args, environment);

    if (!environment->requireDebuggee(cx))
        return false;

    if (environment->type() == DebuggerEnvironmentType::Declarative) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_NO_ENV_OBJECT);
        return false;
    }

    RootedDebuggerObject result(cx);
    if (!environment->getObject(cx, &result))
        return false;

    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerEnvironment::calleeGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get callee", args, environment);

    if (!environment->requireDebuggee(cx))
        return false;

    RootedDebuggerObject result(cx);
    if (!environment->getCallee(cx, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerEnvironment::inspectableGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get inspectable", args, environment);

    args.rval().setBoolean(environment->isDebuggee());
    return true;
}

/* static */ bool
DebuggerEnvironment::optimizedOutGetter(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "get optimizedOut", args, environment);

    args.rval().setBoolean(environment->isOptimized());
    return true;
}

/* static */ bool
DebuggerEnvironment::namesMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "names", args, environment);

    if (!environment->requireDebuggee(cx))
        return false;

    Rooted<IdVector> ids(cx, IdVector(cx));
    if (!DebuggerEnvironment::getNames(cx, environment, &ids))
        return false;

    RootedObject obj(cx, IdVectorToArray(cx, ids));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* static */ bool
DebuggerEnvironment::findMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "find", args, environment);
    if (!args.requireAtLeast(cx, "Debugger.Environment.find", 1))
        return false;

    if (!environment->requireDebuggee(cx))
        return false;

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    RootedDebuggerEnvironment result(cx);
    if (!DebuggerEnvironment::find(cx, environment, id, &result))
        return false;

    args.rval().setObjectOrNull(result);
    return true;
}

/* static */ bool
DebuggerEnvironment::getVariableMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "getVariable", args, environment);
    if (!args.requireAtLeast(cx, "Debugger.Environment.getVariable", 1))
        return false;

    if (!environment->requireDebuggee(cx))
        return false;

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    return DebuggerEnvironment::getVariable(cx, environment, id, args.rval());
}

/* static */ bool
DebuggerEnvironment::setVariableMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_ENVIRONMENT(cx, argc, vp, "setVariable", args, environment);
    if (!args.requireAtLeast(cx, "Debugger.Environment.setVariable", 2))
        return false;

    if (!environment->requireDebuggee(cx))
        return false;

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    if (!DebuggerEnvironment::setVariable(cx, environment, id, args[1]))
        return false;

    args.rval().setUndefined();
    return true;
}

bool
DebuggerEnvironment::requireDebuggee(JSContext* cx) const
{
    if (!isDebuggee()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_DEBUGGEE,
                                  "Debugger.Environment", "environment");

        return false;
    }

    return true;
}

const JSPropertySpec DebuggerEnvironment::properties_[] = {
    JS_PSG("type", DebuggerEnvironment::typeGetter, 0),
    JS_PSG("parent", DebuggerEnvironment::parentGetter, 0),
    JS_PSG("object", DebuggerEnvironment::objectGetter, 0),
    JS_PSG("callee", DebuggerEnvironment::calleeGetter, 0),
    JS_PSG("inspectable", DebuggerEnvironment::inspectableGetter, 0),
    JS_PSG("optimizedOut", DebuggerEnvironment::optimizedOutGetter, 0),
    JS_PS_END
};

const JSFunctionSpec DebuggerEnvironment::methods_[] = {
    JS_FN("names", DebuggerEnvironment::namesMethod, 0, 0),
    JS_FN("find", DebuggerEnvironment::findMethod, 1, 0),
    JS_FN("getVariable", DebuggerEnvironment::getVariableMethod, 1, 0),
    JS_FN("setVariable", DebuggerEnvironment::setVariableMethod, 2, 0),
    JS_FS_END
};

/* static */ NativeObject*
DebuggerEnvironment::initClass(JSContext* cx, HandleObject dbgCtor, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));

    return InitClass(cx, dbgCtor, objProto, &DebuggerEnvironment::class_, construct, 0,
                     properties_, methods_, nullptr, nullptr);
}

/* static */ DebuggerEnvironment*
DebuggerEnvironment::create(JSContext* cx, HandleObject proto, HandleObject referent,
                            HandleNativeObject debugger)
{
    NewObjectKind newKind = IsInsideNursery(referent) ? GenericObject : TenuredObject;
    RootedObject obj(cx, NewObjectWithGivenProto(cx, &DebuggerEnvironment::class_, proto, newKind));
    if (!obj)
        return nullptr;

    DebuggerEnvironment& environment = obj->as<DebuggerEnvironment>();
    environment.setPrivateGCThing(referent);
    environment.setReservedSlot(OWNER_SLOT, ObjectValue(*debugger));

    return &environment;
}

/* static */ DebuggerEnvironmentType
DebuggerEnvironment::type() const
{
    /* Don't bother switching compartments just to check env's type. */
    if (IsDeclarative(referent()))
        return DebuggerEnvironmentType::Declarative;
    if (IsDebugEnvironmentWrapper<WithEnvironmentObject>(referent()))
        return DebuggerEnvironmentType::With;
    return DebuggerEnvironmentType::Object;
}

bool
DebuggerEnvironment::getParent(JSContext* cx, MutableHandleDebuggerEnvironment result) const
{
    /* Don't bother switching compartments just to get env's parent. */
    Rooted<Env*> parent(cx, referent()->enclosingEnvironment());
    if (!parent) {
        result.set(nullptr);
        return true;
    }

    return owner()->wrapEnvironment(cx, parent, result);
}

bool
DebuggerEnvironment::getObject(JSContext* cx, MutableHandleDebuggerObject result) const
{
    MOZ_ASSERT(type() != DebuggerEnvironmentType::Declarative);

    /* Don't bother switching compartments just to get env's object. */
    RootedObject object(cx);
    if (IsDebugEnvironmentWrapper<WithEnvironmentObject>(referent())) {
        object.set(&referent()->as<DebugEnvironmentProxy>()
                   .environment().as<WithEnvironmentObject>().object());
    } else if (IsDebugEnvironmentWrapper<NonSyntacticVariablesObject>(referent())) {
        object.set(&referent()->as<DebugEnvironmentProxy>()
                   .environment().as<NonSyntacticVariablesObject>());
    } else {
        object.set(referent());
        MOZ_ASSERT(!object->is<DebugEnvironmentProxy>());
    }

    return owner()->wrapDebuggeeObject(cx, object, result);
}

bool
DebuggerEnvironment::getCallee(JSContext* cx, MutableHandleDebuggerObject result) const
{
    if (!referent()->is<DebugEnvironmentProxy>()) {
        result.set(nullptr);
        return true;
    }

    JSObject& scope = referent()->as<DebugEnvironmentProxy>().environment();
    if (!scope.is<CallObject>()) {
        result.set(nullptr);
        return true;
    }

    RootedObject callee(cx, &scope.as<CallObject>().callee());
    if (IsInternalFunctionObject(*callee)) {
        result.set(nullptr);
        return true;
    }

    return owner()->wrapDebuggeeObject(cx, callee, result);
}

bool
DebuggerEnvironment::isDebuggee() const
{
    MOZ_ASSERT(referent());
    MOZ_ASSERT(!referent()->is<EnvironmentObject>());

    return owner()->observesGlobal(&referent()->global());
}

bool
DebuggerEnvironment::isOptimized() const
{
    return referent()->is<DebugEnvironmentProxy>() &&
           referent()->as<DebugEnvironmentProxy>().isOptimizedOut();
}

/* static */ bool
DebuggerEnvironment::getNames(JSContext* cx, HandleDebuggerEnvironment environment,
                              MutableHandle<IdVector> result)
{
    MOZ_ASSERT(environment->isDebuggee());

    Rooted<Env*> referent(cx, environment->referent());

    AutoIdVector ids(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);

        ErrorCopier ec(ac);
        if (!GetPropertyKeys(cx, referent, JSITER_HIDDEN, &ids))
            return false;
    }

    for (size_t i = 0; i < ids.length(); ++i) {
        jsid id = ids[i];
        if (JSID_IS_ATOM(id) && IsIdentifier(JSID_TO_ATOM(id))) {
            cx->markId(id);
            if (!result.append(id))
                return false;
        }
    }

    return true;
}

/* static */ bool
DebuggerEnvironment::find(JSContext* cx, HandleDebuggerEnvironment environment, HandleId id,
                          MutableHandleDebuggerEnvironment result)
{
    MOZ_ASSERT(environment->isDebuggee());

    Rooted<Env*> env(cx, environment->referent());
    Debugger* dbg = environment->owner();

    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, env);

        cx->markId(id);

        /* This can trigger resolve hooks. */
        ErrorCopier ec(ac);
        for (; env; env = env->enclosingEnvironment()) {
            bool found;
            if (!HasProperty(cx, env, id, &found))
                return false;
            if (found)
                break;
        }
    }

    if (!env) {
        result.set(nullptr);
        return true;
    }

    return dbg->wrapEnvironment(cx, env, result);
}

/* static */ bool
DebuggerEnvironment::getVariable(JSContext* cx, HandleDebuggerEnvironment environment,
                                 HandleId id, MutableHandleValue result)
{
    MOZ_ASSERT(environment->isDebuggee());

    Rooted<Env*> referent(cx, environment->referent());
    Debugger* dbg = environment->owner();

    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);

        cx->markId(id);

        /* This can trigger getters. */
        ErrorCopier ec(ac);

        bool found;
        if (!HasProperty(cx, referent, id, &found))
            return false;
        if (!found) {
            result.setUndefined();
            return true;
        }

        // For DebugEnvironmentProxys, we get sentinel values for optimized out
        // slots and arguments instead of throwing (the default behavior).
        //
        // See wrapDebuggeeValue for how the sentinel values are wrapped.
        if (referent->is<DebugEnvironmentProxy>()) {
            Rooted<DebugEnvironmentProxy*> env(cx, &referent->as<DebugEnvironmentProxy>());
            if (!DebugEnvironmentProxy::getMaybeSentinelValue(cx, env, id, result))
                return false;
        } else {
            if (!GetProperty(cx, referent, referent, id, result))
                return false;
        }
    }

    // When we've faked up scope chain objects for optimized-out scopes,
    // declarative environments may contain internal JSFunction objects, which
    // we shouldn't expose to the user.
    if (result.isObject()) {
        RootedObject obj(cx, &result.toObject());
        if (obj->is<JSFunction>() &&
            IsInternalFunctionObject(obj->as<JSFunction>()))
            result.setMagic(JS_OPTIMIZED_OUT);
    }

    return dbg->wrapDebuggeeValue(cx, result);
}

/* static */ bool
DebuggerEnvironment::setVariable(JSContext* cx, HandleDebuggerEnvironment environment,
                                 HandleId id, HandleValue value_)
{
    MOZ_ASSERT(environment->isDebuggee());

    Rooted<Env*> referent(cx, environment->referent());
    Debugger* dbg = environment->owner();

    RootedValue value(cx, value_);
    if (!dbg->unwrapDebuggeeValue(cx, &value))
        return false;

    {
        Maybe<AutoCompartment> ac;
        ac.emplace(cx, referent);
        if (!cx->compartment()->wrap(cx, &value))
            return false;
        cx->markId(id);

        /* This can trigger setters. */
        ErrorCopier ec(ac);

        /* Make sure the environment actually has the specified binding. */
        bool found;
        if (!HasProperty(cx, referent, id, &found))
            return false;
        if (!found) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_VARIABLE_NOT_FOUND);
            return false;
        }

        /* Just set the property. */
        if (!SetProperty(cx, referent, id, value))
            return false;
    }

    return true;
}


/*** JS::dbg::Builder ****************************************************************************/

Builder::Builder(JSContext* cx, js::Debugger* debugger)
  : debuggerObject(cx, debugger->toJSObject().get()),
    debugger(debugger)
{ }


#if DEBUG
void
Builder::assertBuilt(JSObject* obj)
{
    // We can't use assertSameCompartment here, because that is always keyed to
    // some JSContext's current compartment, whereas BuiltThings can be
    // constructed and assigned to without respect to any particular context;
    // the only constraint is that they should be in their debugger's compartment.
    MOZ_ASSERT_IF(obj, debuggerObject->compartment() == obj->compartment());
}
#endif

bool
Builder::Object::definePropertyToTrusted(JSContext* cx, const char* name,
                                         JS::MutableHandleValue trusted)
{
    // We should have checked for false Objects before calling this.
    MOZ_ASSERT(value);

    JSAtom* atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return false;
    RootedId id(cx, AtomToId(atom));

    return DefineDataProperty(cx, value, id, trusted);
}

bool
Builder::Object::defineProperty(JSContext* cx, const char* name, JS::HandleValue propval_)
{
    AutoCompartment ac(cx, debuggerObject());

    RootedValue propval(cx, propval_);
    if (!debugger()->wrapDebuggeeValue(cx, &propval))
        return false;

    return definePropertyToTrusted(cx, name, &propval);
}

bool
Builder::Object::defineProperty(JSContext* cx, const char* name, JS::HandleObject propval_)
{
    RootedValue propval(cx, ObjectOrNullValue(propval_));
    return defineProperty(cx, name, propval);
}

bool
Builder::Object::defineProperty(JSContext* cx, const char* name, Builder::Object& propval_)
{
    AutoCompartment ac(cx, debuggerObject());

    RootedValue propval(cx, ObjectOrNullValue(propval_.value));
    return definePropertyToTrusted(cx, name, &propval);
}

Builder::Object
Builder::newObject(JSContext* cx)
{
    AutoCompartment ac(cx, debuggerObject);

    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));

    // If the allocation failed, this will return a false Object, as the spec promises.
    return Object(cx, *this, obj);
}


/*** JS::dbg::AutoEntryMonitor ******************************************************************/

AutoEntryMonitor::AutoEntryMonitor(JSContext* cx)
  : cx_(cx),
    savedMonitor_(cx->entryMonitor)
{
    cx->entryMonitor = this;
}

AutoEntryMonitor::~AutoEntryMonitor()
{
    cx_->entryMonitor = savedMonitor_;
}


/*** Glue ****************************************************************************************/

extern JS_PUBLIC_API(bool)
JS_DefineDebuggerObject(JSContext* cx, HandleObject obj)
{
    RootedNativeObject
        objProto(cx),
        debugCtor(cx),
        debugProto(cx),
        frameProto(cx),
        scriptProto(cx),
        sourceProto(cx),
        objectProto(cx),
        envProto(cx),
        memoryProto(cx);
    RootedObject debuggeeWouldRunProto(cx);
    RootedValue debuggeeWouldRunCtor(cx);
    Handle<GlobalObject*> global = obj.as<GlobalObject>();

    objProto = GlobalObject::getOrCreateObjectPrototype(cx, global);
    if (!objProto)
        return false;
    debugProto = InitClass(cx, obj,
                           objProto, &Debugger::class_, Debugger::construct,
                           1, Debugger::properties, Debugger::methods, nullptr,
                           Debugger::static_methods, debugCtor.address());
    if (!debugProto)
        return false;

    frameProto = DebuggerFrame::initClass(cx, debugCtor, obj);
    if (!frameProto)
        return false;

    scriptProto = InitClass(cx, debugCtor, objProto, &DebuggerScript_class,
                            DebuggerScript_construct, 0,
                            DebuggerScript_properties, DebuggerScript_methods,
                            nullptr, nullptr);
    if (!scriptProto)
        return false;

    sourceProto = InitClass(cx, debugCtor, sourceProto, &DebuggerSource_class,
                            DebuggerSource_construct, 0,
                            DebuggerSource_properties, DebuggerSource_methods,
                            nullptr, nullptr);
    if (!sourceProto)
        return false;

    objectProto = DebuggerObject::initClass(cx, obj, debugCtor);
    if (!objectProto)
        return false;

    envProto = DebuggerEnvironment::initClass(cx, debugCtor, obj);
    if (!envProto)
        return false;

    memoryProto = InitClass(cx, debugCtor, objProto, &DebuggerMemory::class_,
                            DebuggerMemory::construct, 0, DebuggerMemory::properties,
                            DebuggerMemory::methods, nullptr, nullptr);
    if (!memoryProto)
        return false;

    debuggeeWouldRunProto =
        GlobalObject::getOrCreateCustomErrorPrototype(cx, global, JSEXN_DEBUGGEEWOULDRUN);
    if (!debuggeeWouldRunProto)
        return false;
    debuggeeWouldRunCtor = global->getConstructor(JSProto_DebuggeeWouldRun);
    RootedId debuggeeWouldRunId(cx, NameToId(ClassName(JSProto_DebuggeeWouldRun, cx)));
    if (!DefineDataProperty(cx, debugCtor, debuggeeWouldRunId, debuggeeWouldRunCtor, 0))
        return false;

    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_FRAME_PROTO, ObjectValue(*frameProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_OBJECT_PROTO, ObjectValue(*objectProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_SCRIPT_PROTO, ObjectValue(*scriptProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_SOURCE_PROTO, ObjectValue(*sourceProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_ENV_PROTO, ObjectValue(*envProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_PROTO, ObjectValue(*memoryProto));
    return true;
}

JS_PUBLIC_API(bool)
JS::dbg::IsDebugger(JSObject& obj)
{
    JSObject* unwrapped = CheckedUnwrap(&obj);
    return unwrapped &&
           js::GetObjectClass(unwrapped) == &Debugger::class_ &&
           js::Debugger::fromJSObject(unwrapped) != nullptr;
}

JS_PUBLIC_API(bool)
JS::dbg::GetDebuggeeGlobals(JSContext* cx, JSObject& dbgObj, AutoObjectVector& vector)
{
    MOZ_ASSERT(IsDebugger(dbgObj));
    js::Debugger* dbg = js::Debugger::fromJSObject(CheckedUnwrap(&dbgObj));

    if (!vector.reserve(vector.length() + dbg->debuggees.count())) {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty(); r.popFront())
        vector.infallibleAppend(static_cast<JSObject*>(r.front()));

    return true;
}

#ifdef DEBUG
/* static */ bool
Debugger::isDebuggerCrossCompartmentEdge(JSObject* obj, const gc::Cell* target)
{
    MOZ_ASSERT(target);

    auto cls = obj->getClass();
    const gc::Cell* referent = nullptr;
    if (cls == &DebuggerScript_class) {
        referent = GetScriptReferentCell(obj);
    } else if (cls == &DebuggerSource_class) {
        referent = GetSourceReferentRawObject(obj);
    } else if (obj->is<DebuggerObject>()) {
        referent = static_cast<gc::Cell*>(obj->as<DebuggerObject>().getPrivate());
    } else if (obj->is<DebuggerEnvironment>()) {
        referent = static_cast<gc::Cell*>(obj->as<DebuggerEnvironment>().getPrivate());
    }

    return referent == target;
}
#endif


/*** JS::dbg::GarbageCollectionEvent **************************************************************/

namespace JS {
namespace dbg {

/* static */ GarbageCollectionEvent::Ptr
GarbageCollectionEvent::Create(JSRuntime* rt, ::js::gcstats::Statistics& stats, uint64_t gcNumber)
{
    auto data = MakeUnique<GarbageCollectionEvent>(gcNumber);
    if (!data)
        return nullptr;

    data->nonincrementalReason = stats.nonincrementalReason();

    for (auto& slice : stats.slices()) {
        if (!data->reason) {
            // There is only one GC reason for the whole cycle, but for legacy
            // reasons this data is stored and replicated on each slice. Each
            // slice used to have its own GCReason, but now they are all the
            // same.
            data->reason = gcreason::ExplainReason(slice.reason);
            MOZ_ASSERT(data->reason);
        }

        if (!data->collections.growBy(1))
            return nullptr;

        data->collections.back().startTimestamp = slice.start;
        data->collections.back().endTimestamp = slice.end;
    }

    return data;
}

static bool
DefineStringProperty(JSContext* cx, HandleObject obj, PropertyName* propName, const char* strVal)
{
    RootedValue val(cx, UndefinedValue());
    if (strVal) {
        JSAtom* atomized = Atomize(cx, strVal, strlen(strVal));
        if (!atomized)
            return false;
        val = StringValue(atomized);
    }
    return DefineDataProperty(cx, obj, propName, val);
}

JSObject*
GarbageCollectionEvent::toJSObject(JSContext* cx) const
{
    RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    RootedValue gcCycleNumberVal(cx, NumberValue(majorGCNumber_));
    if (!obj ||
        !DefineStringProperty(cx, obj, cx->names().nonincrementalReason, nonincrementalReason) ||
        !DefineStringProperty(cx, obj, cx->names().reason, reason) ||
        !DefineDataProperty(cx, obj, cx->names().gcCycleNumber, gcCycleNumberVal))
    {
        return nullptr;
    }

    RootedArrayObject slicesArray(cx, NewDenseEmptyArray(cx));
    if (!slicesArray)
        return nullptr;

    TimeStamp originTime = TimeStamp::ProcessCreation();

    size_t idx = 0;
    for (auto range = collections.all(); !range.empty(); range.popFront()) {
        RootedPlainObject collectionObj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!collectionObj)
            return nullptr;

        RootedValue start(cx), end(cx);
        start = NumberValue((range.front().startTimestamp - originTime).ToMilliseconds());
        end = NumberValue((range.front().endTimestamp - originTime).ToMilliseconds());
        if (!DefineDataProperty(cx, collectionObj, cx->names().startTimestamp, start) ||
            !DefineDataProperty(cx, collectionObj, cx->names().endTimestamp, end))
        {
            return nullptr;
        }

        RootedValue collectionVal(cx, ObjectValue(*collectionObj));
        if (!DefineDataElement(cx, slicesArray, idx++, collectionVal))
            return nullptr;
    }

    RootedValue slicesValue(cx, ObjectValue(*slicesArray));
    if (!DefineDataProperty(cx, obj, cx->names().collections, slicesValue))
        return nullptr;

    return obj;
}

JS_PUBLIC_API(bool)
FireOnGarbageCollectionHookRequired(JSContext* cx)
{
    AutoCheckCannotGC noGC;

    for (ZoneGroupsIter group(cx->runtime()); !group.done(); group.next()) {
        for (Debugger* dbg : group->debuggerList()) {
            if (dbg->enabled &&
                dbg->observedGC(cx->runtime()->gc.majorGCCount()) &&
                dbg->getHook(Debugger::OnGarbageCollection))
            {
                return true;
            }
        }
    }

    return false;
}

JS_PUBLIC_API(bool)
FireOnGarbageCollectionHook(JSContext* cx, JS::dbg::GarbageCollectionEvent::Ptr&& data)
{
    AutoObjectVector triggered(cx);

    {
        // We had better not GC (and potentially get a dangling Debugger
        // pointer) while finding all Debuggers observing a debuggee that
        // participated in this GC.
        AutoCheckCannotGC noGC;

        for (ZoneGroupsIter group(cx->runtime()); !group.done(); group.next()) {
            for (Debugger* dbg : group->debuggerList()) {
                if (dbg->enabled &&
                    dbg->observedGC(data->majorGCNumber()) &&
                    dbg->getHook(Debugger::OnGarbageCollection))
                {
                    if (!triggered.append(dbg->object)) {
                        JS_ReportOutOfMemory(cx);
                        return false;
                    }
                }
            }
        }
    }

    for ( ; !triggered.empty(); triggered.popBack()) {
        Debugger* dbg = Debugger::fromJSObject(triggered.back());
        dbg->fireOnGarbageCollectionHook(cx, data);
        MOZ_ASSERT(!cx->isExceptionPending());
    }

    return true;
}

} // namespace dbg
} // namespace JS
