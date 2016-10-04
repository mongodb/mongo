/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TestingFunctions.h"

#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"

#include "jsapi.h"
#include "jscntxt.h"
#include "jsfriendapi.h"
#include "jsgc.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jswrapper.h"

#include "asmjs/AsmJSLink.h"
#include "asmjs/AsmJSValidate.h"
#include "jit/InlinableNatives.h"
#include "jit/JitFrameIterator.h"
#include "js/Debug.h"
#include "js/HashTable.h"
#include "js/StructuredClone.h"
#include "js/UbiNode.h"
#include "js/UbiNodeBreadthFirst.h"
#include "js/Vector.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/ProxyObject.h"
#include "vm/SavedStacks.h"
#include "vm/Stack.h"
#include "vm/TraceLogging.h"

#include "jscntxtinlines.h"
#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::Move;
using mozilla::UniquePtr;

// If fuzzingSafe is set, remove functionality that could cause problems with
// fuzzers. Set this via the environment variable MOZ_FUZZING_SAFE.
static bool fuzzingSafe = false;

// If disableOOMFunctions is set, disable functionality that causes artificial
// OOM conditions.
static bool disableOOMFunctions = false;

static bool
EnvVarIsDefined(const char* name)
{
    const char* value = getenv(name);
    return value && *value;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
static bool
EnvVarAsInt(const char* name, int* valueOut)
{
    if (!EnvVarIsDefined(name))
        return false;

    *valueOut = atoi(getenv(name));
    return true;
}
#endif

static bool
GetBuildConfiguration(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject info(cx, JS_NewPlainObject(cx));
    if (!info)
        return false;

    if (!JS_SetProperty(cx, info, "rooting-analysis", FalseHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "exact-rooting", TrueHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "trace-jscalls-api", FalseHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "incremental-gc", TrueHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "generational-gc", TrueHandleValue))
        return false;

    RootedValue value(cx);
#ifdef DEBUG
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "debug", value))
        return false;

#ifdef RELEASE_BUILD
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "release", value))
        return false;

#ifdef JS_HAS_CTYPES
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "has-ctypes", value))
        return false;

#ifdef JS_CPU_X86
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "x86", value))
        return false;

#ifdef JS_CPU_X64
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "x64", value))
        return false;

#ifdef JS_SIMULATOR_ARM
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "arm-simulator", value))
        return false;

#ifdef JS_SIMULATOR_ARM64
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "arm64-simulator", value))
        return false;

#ifdef MOZ_ASAN
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "asan", value))
        return false;

#ifdef MOZ_TSAN
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "tsan", value))
        return false;

#ifdef JS_GC_ZEAL
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "has-gczeal", value))
        return false;

#ifdef JS_MORE_DETERMINISTIC
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "more-deterministic", value))
        return false;

#ifdef MOZ_PROFILING
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "profiling", value))
        return false;

#ifdef INCLUDE_MOZILLA_DTRACE
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "dtrace", value))
        return false;

#ifdef MOZ_VALGRIND
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "valgrind", value))
        return false;

#ifdef JS_OOM_DO_BACKTRACES
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "oom-backtraces", value))
        return false;

#ifdef ENABLE_BINARYDATA
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "binary-data", value))
        return false;

#ifdef EXPOSE_INTL_API
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "intl-api", value))
        return false;

#if defined(XP_WIN)
    value = BooleanValue(false);
#elif defined(SOLARIS)
    value = BooleanValue(false);
#elif defined(XP_UNIX)
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "mapped-array-buffer", value))
        return false;

#ifdef MOZ_MEMORY
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "moz-memory", value))
        return false;

    value.setInt32(sizeof(void*));
    if (!JS_SetProperty(cx, info, "pointer-byte-size", value))
        return false;

    args.rval().setObject(*info);
    return true;
}

static bool
GC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /*
     * If the first argument is 'compartment', we collect any compartments
     * previously scheduled for GC via schedulegc. If the first argument is an
     * object, we collect the object's compartment (and any other compartments
     * scheduled for GC). Otherwise, we collect all compartments.
     */
    bool compartment = false;
    if (args.length() >= 1) {
        Value arg = args[0];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "compartment", &compartment))
                return false;
        } else if (arg.isObject()) {
            PrepareZoneForGC(UncheckedUnwrap(&arg.toObject())->zone());
            compartment = true;
        }
    }

    bool shrinking = false;
    if (args.length() >= 2) {
        Value arg = args[1];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "shrinking", &shrinking))
                return false;
        }
    }

#ifndef JS_MORE_DETERMINISTIC
    size_t preBytes = cx->runtime()->gc.usage.gcBytes();
#endif

    if (compartment)
        PrepareForDebugGC(cx->runtime());
    else
        JS::PrepareForFullGC(cx->runtime());

    JSGCInvocationKind gckind = shrinking ? GC_SHRINK : GC_NORMAL;
    JS::GCForReason(cx->runtime(), gckind, JS::gcreason::API);

    char buf[256] = { '\0' };
#ifndef JS_MORE_DETERMINISTIC
    JS_snprintf(buf, sizeof(buf), "before %lu, after %lu\n",
                (unsigned long)preBytes, (unsigned long)cx->runtime()->gc.usage.gcBytes());
#endif
    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
MinorGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.get(0) == BooleanValue(true))
        cx->runtime()->gc.storeBuffer.setAboutToOverflow();

    cx->minorGC(JS::gcreason::API);
    args.rval().setUndefined();
    return true;
}

static const struct ParamPair {
    const char*     name;
    JSGCParamKey    param;
} paramMap[] = {
    {"maxBytes",            JSGC_MAX_BYTES },
    {"maxMallocBytes",      JSGC_MAX_MALLOC_BYTES},
    {"gcBytes",             JSGC_BYTES},
    {"gcNumber",            JSGC_NUMBER},
    {"sliceTimeBudget",     JSGC_SLICE_TIME_BUDGET},
    {"markStackLimit",      JSGC_MARK_STACK_LIMIT},
    {"minEmptyChunkCount",  JSGC_MIN_EMPTY_CHUNK_COUNT},
    {"maxEmptyChunkCount",  JSGC_MAX_EMPTY_CHUNK_COUNT}
};

// Keep this in sync with above params.
#define GC_PARAMETER_ARGS_LIST "maxBytes, maxMallocBytes, gcBytes, gcNumber, sliceTimeBudget, markStackLimit, minEmptyChunkCount or maxEmptyChunkCount"

static bool
GCParameter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSString* str = ToString(cx, args.get(0));
    if (!str)
        return false;

    JSFlatString* flatStr = JS_FlattenString(cx, str);
    if (!flatStr)
        return false;

    size_t paramIndex = 0;
    for (;; paramIndex++) {
        if (paramIndex == ArrayLength(paramMap)) {
            JS_ReportError(cx,
                           "the first argument must be one of " GC_PARAMETER_ARGS_LIST);
            return false;
        }
        if (JS_FlatStringEqualsAscii(flatStr, paramMap[paramIndex].name))
            break;
    }
    JSGCParamKey param = paramMap[paramIndex].param;

    // Request mode.
    if (args.length() == 1) {
        uint32_t value = JS_GetGCParameter(cx->runtime(), param);
        args.rval().setNumber(value);
        return true;
    }

    if (param == JSGC_NUMBER || param == JSGC_BYTES) {
        JS_ReportError(cx, "Attempt to change read-only parameter %s",
                       paramMap[paramIndex].name);
        return false;
    }

    if (disableOOMFunctions && (param == JSGC_MAX_BYTES || param == JSGC_MAX_MALLOC_BYTES)) {
        args.rval().setUndefined();
        return true;
    }

    uint32_t value;
    if (!ToUint32(cx, args[1], &value))
        return false;

    if (!value) {
        JS_ReportError(cx, "the second argument must be convertable to uint32_t "
                           "with non-zero value");
        return false;
    }

    if (param == JSGC_MARK_STACK_LIMIT && JS::IsIncrementalGCInProgress(cx->runtime())) {
        JS_ReportError(cx, "attempt to set markStackLimit while a GC is in progress");
        return false;
    }

    if (param == JSGC_MAX_BYTES) {
        uint32_t gcBytes = JS_GetGCParameter(cx->runtime(), JSGC_BYTES);
        if (value < gcBytes) {
            JS_ReportError(cx,
                           "attempt to set maxBytes to the value less than the current "
                           "gcBytes (%u)",
                           gcBytes);
            return false;
        }
    }

    JS_SetGCParameter(cx->runtime(), param, value);
    args.rval().setUndefined();
    return true;
}

static void
SetAllowRelazification(JSContext* cx, bool allow)
{
    JSRuntime* rt = cx->runtime();
    MOZ_ASSERT(rt->allowRelazificationForTesting != allow);
    rt->allowRelazificationForTesting = allow;

    for (AllFramesIter i(cx); !i.done(); ++i)
        i.script()->setDoNotRelazify(allow);
}

static bool
RelazifyFunctions(JSContext* cx, unsigned argc, Value* vp)
{
    // Relazifying functions on GC is usually only done for compartments that are
    // not active. To aid fuzzing, this testing function allows us to relazify
    // even if the compartment is active.

    SetAllowRelazification(cx, true);
    bool res = GC(cx, argc, vp);
    SetAllowRelazification(cx, false);
    return res;
}

static bool
IsProxy(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportError(cx, "the function takes exactly one argument");
        return false;
    }
    if (!args[0].isObject()) {
        args.rval().setBoolean(false);
        return true;
    }
    args.rval().setBoolean(args[0].toObject().is<ProxyObject>());
    return true;
}

static bool
IsLazyFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportError(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportError(cx, "The first argument should be a function.");
        return false;
    }
    args.rval().setBoolean(args[0].toObject().as<JSFunction>().isInterpretedLazy());
    return true;
}

static bool
IsRelazifiableFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportError(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() ||
        !args[0].toObject().is<JSFunction>())
    {
        JS_ReportError(cx, "The first argument should be a function.");
        return true;
    }

    JSFunction* fun = &args[0].toObject().as<JSFunction>();
    args.rval().setBoolean(fun->hasScript() && fun->nonLazyScript()->isRelazifiable());
    return true;
}

static bool
InternalConst(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() == 0) {
        JS_ReportError(cx, "the function takes exactly one argument");
        return false;
    }

    JSString* str = ToString(cx, args[0]);
    if (!str)
        return false;
    JSFlatString* flat = JS_FlattenString(cx, str);
    if (!flat)
        return false;

    if (JS_FlatStringEqualsAscii(flat, "INCREMENTAL_MARK_STACK_BASE_CAPACITY")) {
        args.rval().setNumber(uint32_t(js::INCREMENTAL_MARK_STACK_BASE_CAPACITY));
    } else {
        JS_ReportError(cx, "unknown const name");
        return false;
    }
    return true;
}

static bool
GCPreserveCode(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setAlwaysPreserveCode();

    args.rval().setUndefined();
    return true;
}

#ifdef JS_GC_ZEAL
static bool
GCZeal(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 2) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }

    uint32_t zeal;
    if (!ToUint32(cx, args.get(0), &zeal))
        return false;

    uint32_t frequency = JS_DEFAULT_ZEAL_FREQ;
    if (args.length() >= 2) {
        if (!ToUint32(cx, args.get(1), &frequency))
            return false;
    }

    JS_SetGCZeal(cx, (uint8_t)zeal, frequency);
    args.rval().setUndefined();
    return true;
}

static bool
ScheduleGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }

    if (args.length() == 0) {
        /* Fetch next zeal trigger only. */
    } else if (args[0].isInt32()) {
        /* Schedule a GC to happen after |arg| allocations. */
        JS_ScheduleGC(cx, args[0].toInt32());
    } else if (args[0].isObject()) {
        /* Ensure that |zone| is collected during the next GC. */
        Zone* zone = UncheckedUnwrap(&args[0].toObject())->zone();
        PrepareZoneForGC(zone);
    } else if (args[0].isString()) {
        /* This allows us to schedule atomsCompartment for GC. */
        PrepareZoneForGC(args[0].toString()->zone());
    }

    uint8_t zeal;
    uint32_t freq;
    uint32_t next;
    JS_GetGCZeal(cx, &zeal, &freq, &next);
    args.rval().setInt32(next);
    return true;
}

static bool
SelectForGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /*
     * The selectedForMarking set is intended to be manually marked at slice
     * start to detect missing pre-barriers. It is invalid for nursery things
     * to be in the set, so evict the nursery before adding items.
     */
    JSRuntime* rt = cx->runtime();
    rt->gc.evictNursery();

    for (unsigned i = 0; i < args.length(); i++) {
        if (args[i].isObject()) {
            if (!rt->gc.selectForMarking(&args[i].toObject()))
                return false;
        }
    }

    args.rval().setUndefined();
    return true;
}

static bool
VerifyPreBarriers(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }

    gc::VerifyBarriers(cx->runtime(), gc::PreBarrierVerifier);
    args.rval().setUndefined();
    return true;
}

static bool
VerifyPostBarriers(JSContext* cx, unsigned argc, Value* vp)
{
    // This is a no-op since the post barrier verifier was removed.
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length()) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }
    args.rval().setUndefined();
    return true;
}

static bool
GCState(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }

    const char* state;
    gc::State globalState = cx->runtime()->gc.state();
    if (globalState == gc::NO_INCREMENTAL)
        state = "none";
    else if (globalState == gc::MARK)
        state = "mark";
    else if (globalState == gc::SWEEP)
        state = "sweep";
    else if (globalState == gc::COMPACT)
        state = "compact";
    else
        MOZ_CRASH("Unobserveable global GC state");

    JSString* str = JS_NewStringCopyZ(cx, state);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
DeterministicGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setDeterministic(ToBoolean(args[0]));
    args.rval().setUndefined();
    return true;
}
#endif /* JS_GC_ZEAL */

static bool
StartGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 2) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    auto budget = SliceBudget::unlimited();
    if (args.length() >= 1) {
        uint32_t work = 0;
        if (!ToUint32(cx, args[0], &work))
            return false;
        budget = SliceBudget(WorkBudget(work));
    }

    bool shrinking = false;
    if (args.length() >= 2) {
        Value arg = args[1];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "shrinking", &shrinking))
                return false;
        }
    }

    JSRuntime* rt = cx->runtime();
    if (rt->gc.isIncrementalGCInProgress()) {
        RootedObject callee(cx, &args.callee());
        JS_ReportError(cx, "Incremental GC already in progress");
        return false;
    }

    JSGCInvocationKind gckind = shrinking ? GC_SHRINK : GC_NORMAL;
    rt->gc.startDebugGC(gckind, budget);

    args.rval().setUndefined();
    return true;
}

static bool
GCSlice(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    auto budget = SliceBudget::unlimited();
    if (args.length() == 1) {
        uint32_t work = 0;
        if (!ToUint32(cx, args[0], &work))
            return false;
        budget = SliceBudget(WorkBudget(work));
    }

    JSRuntime* rt = cx->runtime();
    if (!rt->gc.isIncrementalGCInProgress())
        rt->gc.startDebugGC(GC_NORMAL, budget);
    else
        rt->gc.debugGCSlice(budget);

    args.rval().setUndefined();
    return true;
}

static bool
AbortGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.abortGC();
    args.rval().setUndefined();
    return true;
}

static bool
ValidateGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setValidate(ToBoolean(args[0]));
    args.rval().setUndefined();
    return true;
}

static bool
FullCompartmentChecks(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setFullCompartmentChecks(ToBoolean(args[0]));
    args.rval().setUndefined();
    return true;
}

static bool
NondeterministicGetWeakMapKeys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }
    if (!args[0].isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                             "nondeterministicGetWeakMapKeys", "WeakMap",
                             InformalValueTypeName(args[0]));
        return false;
    }
    RootedObject arr(cx);
    RootedObject mapObj(cx, &args[0].toObject());
    if (!JS_NondeterministicGetWeakMapKeys(cx, mapObj, &arr))
        return false;
    if (!arr) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                             "nondeterministicGetWeakMapKeys", "WeakMap",
                             args[0].toObject().getClass()->name);
        return false;
    }
    args.rval().setObject(*arr);
    return true;
}

class HasChildTracer : public JS::CallbackTracer
{
    RootedValue child_;
    bool found_;

    void onChild(const JS::GCCellPtr& thing) override {
        if (thing.asCell() == child_.toGCThing())
            found_ = true;
    }

  public:
    HasChildTracer(JSRuntime* rt, HandleValue child)
      : JS::CallbackTracer(rt, TraceWeakMapKeysValues), child_(rt, child), found_(false)
    {}

    bool found() const { return found_; }
};

static bool
HasChild(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedValue parent(cx, args.get(0));
    RootedValue child(cx, args.get(1));

    if (!parent.isMarkable() || !child.isMarkable()) {
        args.rval().setBoolean(false);
        return true;
    }

    HasChildTracer trc(cx->runtime(), child);
    TraceChildren(&trc, parent.toGCThing(), parent.traceKind());
    args.rval().setBoolean(trc.found());
    return true;
}

static bool
SetSavedStacksRNGState(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "setSavedStacksRNGState", 1))
        return false;

    int32_t seed;
    if (!ToInt32(cx, args[0], &seed))
        return false;

    // Either one or the other of the seed arguments must be non-zero;
    // make this true no matter what value 'seed' has.
    cx->compartment()->savedStacks().setRNGState(seed, (seed + 1) * 33);
    return true;
}

static bool
GetSavedFrameCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(cx->compartment()->savedStacks().count());
    return true;
}

static bool
SaveStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    unsigned maxFrameCount = 0;
    if (args.length() >= 1) {
        double d;
        if (!ToNumber(cx, args[0], &d))
            return false;
        if (d < 0) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                  JSDVG_SEARCH_STACK, args[0], nullptr,
                                  "not a valid maximum frame count", NULL);
            return false;
        }
        maxFrameCount = d;
    }

    JSCompartment* targetCompartment = cx->compartment();
    if (args.length() >= 2) {
        if (!args[1].isObject()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                  JSDVG_SEARCH_STACK, args[0], nullptr,
                                  "not an object", NULL);
            return false;
        }
        RootedObject obj(cx, UncheckedUnwrap(&args[1].toObject()));
        if (!obj)
            return false;
        targetCompartment = obj->compartment();
    }

    RootedObject stack(cx);
    {
        AutoCompartment ac(cx, targetCompartment);
        if (!JS::CaptureCurrentStack(cx, &stack, maxFrameCount))
            return false;
    }

    if (stack && !cx->compartment()->wrap(cx, &stack))
        return false;

    args.rval().setObjectOrNull(stack);
    return true;
}

static bool
CallFunctionFromNativeFrame(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportError(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() || !IsCallable(args[0])) {
        JS_ReportError(cx, "The first argument should be a function.");
        return false;
    }

    RootedObject function(cx, &args[0].toObject());
    return Call(cx, UndefinedHandleValue, function,
                JS::HandleValueArray::empty(), args.rval());
}

static bool
CallFunctionWithAsyncStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 3) {
        JS_ReportError(cx, "The function takes exactly three arguments.");
        return false;
    }
    if (!args[0].isObject() || !IsCallable(args[0])) {
        JS_ReportError(cx, "The first argument should be a function.");
        return false;
    }
    if (!args[1].isObject() || !args[1].toObject().is<SavedFrame>()) {
        JS_ReportError(cx, "The second argument should be a SavedFrame.");
        return false;
    }
    if (!args[2].isString() || args[2].toString()->empty()) {
        JS_ReportError(cx, "The third argument should be a non-empty string.");
        return false;
    }

    RootedObject function(cx, &args[0].toObject());
    RootedObject stack(cx, &args[1].toObject());
    RootedString asyncCause(cx, args[2].toString());

    JS::AutoSetAsyncStackForNewCalls sas(cx, stack, asyncCause,
                                         JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::EXPLICIT);
    return Call(cx, UndefinedHandleValue, function,
                JS::HandleValueArray::empty(), args.rval());
}

static bool
EnableTrackAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    SetObjectMetadataCallback(cx, SavedStacksMetadataCallback);
    return true;
}

static bool
DisableTrackAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    SetObjectMetadataCallback(cx, nullptr);
    return true;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
static bool
OOMThreadTypes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(js::oom::THREAD_TYPE_MAX);
    return true;
}

static bool
SetupOOMFailure(JSContext* cx, bool failAlways, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (disableOOMFunctions) {
        args.rval().setUndefined();
        return true;
    }

    if (args.length() < 1) {
        JS_ReportError(cx, "Count argument required");
        return false;
    }

    if (args.length() > 2) {
        JS_ReportError(cx, "Too many arguments");
        return false;
    }

    int32_t count;
    if (!JS::ToInt32(cx, args.get(0), &count))
        return false;

    if (count <= 0) {
        JS_ReportError(cx, "OOM cutoff should be positive");
        return false;
    }

    uint32_t targetThread = js::oom::THREAD_TYPE_MAIN;
    if (args.length() > 1 && !ToUint32(cx, args[1], &targetThread))
        return false;

    if (targetThread == js::oom::THREAD_TYPE_NONE || targetThread >= js::oom::THREAD_TYPE_MAX) {
        JS_ReportError(cx, "Invalid thread type specified");
        return false;
    }

    HelperThreadState().waitForAllThreads();
    js::oom::targetThread = targetThread;
    if (uint64_t(OOM_counter) + count >= UINT32_MAX) {
        JS_ReportError(cx, "OOM cutoff out of range");
        return false;
    }
    OOM_maxAllocations = OOM_counter + count;
    OOM_failAlways = failAlways;
    args.rval().setUndefined();
    return true;
}

static bool
OOMAfterAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    return SetupOOMFailure(cx, true, argc, vp);
}

static bool
OOMAtAllocation(JSContext* cx, unsigned argc, Value* vp)
{
    return SetupOOMFailure(cx, false, argc, vp);
}

static bool
ResetOOMFailure(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(OOM_counter >= OOM_maxAllocations);
    js::oom::targetThread = js::oom::THREAD_TYPE_NONE;
    OOM_maxAllocations = UINT32_MAX;
    return true;
}

static bool
OOMTest(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1 || !args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportError(cx, "oomTest() takes a single function argument.");
        return false;
    }

    if (disableOOMFunctions) {
        args.rval().setUndefined();
        return true;
    }

    MOZ_ASSERT(!cx->isExceptionPending());
    cx->runtime()->hadOutOfMemory = false;

    RootedFunction function(cx, &args[0].toObject().as<JSFunction>());

    bool verbose = EnvVarIsDefined("OOM_VERBOSE");

    unsigned threadStart = oom::THREAD_TYPE_MAIN;
    unsigned threadEnd = oom::THREAD_TYPE_MAX;

    // Test a single thread type if specified by the OOM_THREAD environment variable.
    int threadOption = 0;
    if (EnvVarAsInt("OOM_THREAD", &threadOption)) {
        if (threadOption < oom::THREAD_TYPE_MAIN || threadOption > oom::THREAD_TYPE_MAX) {
            JS_ReportError(cx, "OOM_THREAD value out of range.");
            return false;
        }

        threadStart = threadOption;
        threadEnd = threadOption + 1;
    }

    JS_SetGCZeal(cx, 0, JS_DEFAULT_ZEAL_FREQ);

    for (unsigned thread = threadStart; thread < threadEnd; thread++) {
        if (verbose)
            fprintf(stderr, "thread %d\n", thread);

        HelperThreadState().waitForAllThreads();
        js::oom::targetThread = thread;

        unsigned allocation = 1;
        bool handledOOM;
        do {
            if (verbose)
                fprintf(stderr, "  allocation %d\n", allocation);

            MOZ_ASSERT(!cx->isExceptionPending());
            MOZ_ASSERT(!cx->runtime()->hadOutOfMemory);

            OOM_maxAllocations = OOM_counter + allocation;
            OOM_failAlways = false;

            RootedValue result(cx);
            bool ok = JS_CallFunction(cx, cx->global(), function,
                                      HandleValueArray::empty(), &result);

            handledOOM = OOM_counter >= OOM_maxAllocations;
            OOM_maxAllocations = UINT32_MAX;

            MOZ_ASSERT_IF(ok, !cx->isExceptionPending());

            // Note that it is possible that the function throws an exception
            // unconnected to OOM, in which case we ignore it. More correct
            // would be to have the caller pass some kind of exception
            // specification and to check the exception against it.

            cx->clearPendingException();
            cx->runtime()->hadOutOfMemory = false;

            allocation++;
        } while (handledOOM);

        if (verbose) {
            fprintf(stderr, "  finished after %d allocations\n", allocation - 2);
        }
    }

    js::oom::targetThread = js::oom::THREAD_TYPE_NONE;

    args.rval().setUndefined();
    return true;
}
#endif

static const js::Class FakePromiseClass = {
    "Promise", JSCLASS_IS_ANONYMOUS
};

static bool
MakeFakePromise(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, NewObjectWithGivenProto(cx, &FakePromiseClass, nullptr));
    if (!obj)
        return false;

    JS::dbg::onNewPromise(cx, obj);
    args.rval().setObject(*obj);
    return true;
}

static bool
SettleFakePromise(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "settleFakePromise", 1))
        return false;
    if (!args[0].isObject() || args[0].toObject().getClass() != &FakePromiseClass) {
        JS_ReportError(cx, "first argument must be a (fake) Promise object");
        return false;
    }

    RootedObject promise(cx, &args[0].toObject());
    JS::dbg::onPromiseSettled(cx, promise);
    return true;
}

static unsigned finalizeCount = 0;

static void
finalize_counter_finalize(JSFreeOp* fop, JSObject* obj)
{
    ++finalizeCount;
}

static const JSClass FinalizeCounterClass = {
    "FinalizeCounter", JSCLASS_IS_ANONYMOUS,
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    finalize_counter_finalize
};

static bool
MakeFinalizeObserver(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSObject* obj = JS_NewObjectWithGivenProto(cx, &FinalizeCounterClass, nullptr);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
FinalizeCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(finalizeCount);
    return true;
}

static bool
DumpHeap(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    DumpHeapNurseryBehaviour nurseryBehaviour = js::IgnoreNurseryObjects;
    FILE* dumpFile = nullptr;

    unsigned i = 0;
    if (args.length() > i) {
        Value v = args[i];
        if (v.isString()) {
            JSString* str = v.toString();
            bool same = false;
            if (!JS_StringEqualsAscii(cx, str, "collectNurseryBeforeDump", &same))
                return false;
            if (same) {
                nurseryBehaviour = js::CollectNurseryBeforeDump;
                ++i;
            }
        }
    }

    if (args.length() > i) {
        Value v = args[i];
        if (v.isString()) {
            if (!fuzzingSafe) {
                JSString* str = v.toString();
                JSAutoByteString fileNameBytes;
                if (!fileNameBytes.encodeLatin1(cx, str))
                    return false;
                const char* fileName = fileNameBytes.ptr();
                dumpFile = fopen(fileName, "w");
                if (!dumpFile) {
                    JS_ReportError(cx, "can't open %s", fileName);
                    return false;
                }
            }
            ++i;
        }
    }

    if (i != args.length()) {
        JS_ReportError(cx, "bad arguments passed to dumpHeap");
        if (dumpFile)
            fclose(dumpFile);
        return false;
    }

    js::DumpHeap(JS_GetRuntime(cx), dumpFile ? dumpFile : stdout, nurseryBehaviour);

    if (dumpFile)
        fclose(dumpFile);

    args.rval().setUndefined();
    return true;
}

static bool
Terminate(JSContext* cx, unsigned arg, Value* vp)
{
#ifdef JS_MORE_DETERMINISTIC
    // Print a message to stderr in more-deterministic builds to help jsfunfuzz
    // find uncatchable-exception bugs.
    fprintf(stderr, "terminate called\n");
#endif

    JS_ClearPendingException(cx);
    return false;
}

#define SPS_PROFILING_STACK_MAX_SIZE 1000
static ProfileEntry SPS_PROFILING_STACK[SPS_PROFILING_STACK_MAX_SIZE];
static uint32_t SPS_PROFILING_STACK_SIZE = 0;

static bool
EnableSPSProfiling(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Disable before re-enabling; see the assertion in |SPSProfiler::setProfilingStack|.
    if (cx->runtime()->spsProfiler.installed())
        cx->runtime()->spsProfiler.enable(false);

    SetRuntimeProfilingStack(cx->runtime(), SPS_PROFILING_STACK, &SPS_PROFILING_STACK_SIZE,
                             SPS_PROFILING_STACK_MAX_SIZE);
    cx->runtime()->spsProfiler.enableSlowAssertions(false);
    cx->runtime()->spsProfiler.enable(true);

    args.rval().setUndefined();
    return true;
}

static bool
EnableSPSProfilingWithSlowAssertions(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (cx->runtime()->spsProfiler.enabled()) {
        // If profiling already enabled with slow assertions disabled,
        // this is a no-op.
        if (cx->runtime()->spsProfiler.slowAssertionsEnabled())
            return true;

        // Slow assertions are off.  Disable profiling before re-enabling
        // with slow assertions on.
        cx->runtime()->spsProfiler.enable(false);
    }

    // Disable before re-enabling; see the assertion in |SPSProfiler::setProfilingStack|.
    if (cx->runtime()->spsProfiler.installed())
        cx->runtime()->spsProfiler.enable(false);

    SetRuntimeProfilingStack(cx->runtime(), SPS_PROFILING_STACK, &SPS_PROFILING_STACK_SIZE,
                             SPS_PROFILING_STACK_MAX_SIZE);
    cx->runtime()->spsProfiler.enableSlowAssertions(true);
    cx->runtime()->spsProfiler.enable(true);

    return true;
}

static bool
DisableSPSProfiling(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (cx->runtime()->spsProfiler.installed())
        cx->runtime()->spsProfiler.enable(false);
    args.rval().setUndefined();
    return true;
}

static bool
ReadSPSProfilingStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    // Return boolean 'false' if profiler is not enabled.
    if (!cx->runtime()->spsProfiler.enabled()) {
        args.rval().setBoolean(false);
        return true;
    }

    // Array holding physical jit stack frames.
    RootedObject stack(cx, NewDenseEmptyArray(cx));
    if (!stack)
        return false;

    RootedObject inlineStack(cx);
    RootedObject inlineFrameInfo(cx);
    RootedString frameKind(cx);
    RootedString frameLabel(cx);
    RootedId idx(cx);

    JS::ProfilingFrameIterator::RegisterState state;
    uint32_t physicalFrameNo = 0;
    const unsigned propAttrs = JSPROP_ENUMERATE;
    for (JS::ProfilingFrameIterator i(cx->runtime(), state); !i.done(); ++i, ++physicalFrameNo) {
        MOZ_ASSERT(i.stackAddress() != nullptr);

        // Array holding all inline frames in a single physical jit stack frame.
        inlineStack = NewDenseEmptyArray(cx);
        if (!inlineStack)
            return false;

        JS::ProfilingFrameIterator::Frame frames[16];
        uint32_t nframes = i.extractStack(frames, 0, 16);
        for (uint32_t inlineFrameNo = 0; inlineFrameNo < nframes; inlineFrameNo++) {

            // Object holding frame info.
            inlineFrameInfo = NewBuiltinClassInstance<PlainObject>(cx);
            if (!inlineFrameInfo)
                return false;

            const char* frameKindStr = nullptr;
            switch (frames[inlineFrameNo].kind) {
              case JS::ProfilingFrameIterator::Frame_Baseline:
                frameKindStr = "baseline";
                break;
              case JS::ProfilingFrameIterator::Frame_Ion:
                frameKindStr = "ion";
                break;
              case JS::ProfilingFrameIterator::Frame_AsmJS:
                frameKindStr = "asmjs";
                break;
              default:
                frameKindStr = "unknown";
            }
            frameKind = NewStringCopyZ<CanGC>(cx, frameKindStr);
            if (!frameKind)
                return false;

            if (!JS_DefineProperty(cx, inlineFrameInfo, "kind", frameKind, propAttrs))
                return false;

            frameLabel = NewStringCopyZ<CanGC>(cx, frames[inlineFrameNo].label);
            if (!frameLabel)
                return false;

            if (!JS_DefineProperty(cx, inlineFrameInfo, "label", frameLabel, propAttrs))
                return false;

            idx = INT_TO_JSID(inlineFrameNo);
            if (!JS_DefinePropertyById(cx, inlineStack, idx, inlineFrameInfo, 0))
                return false;
        }

        // Push inline array into main array.
        idx = INT_TO_JSID(physicalFrameNo);
        if (!JS_DefinePropertyById(cx, stack, idx, inlineStack, 0))
            return false;
    }

    args.rval().setObject(*stack);
    return true;
}

static bool
EnableOsiPointRegisterChecks(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#ifdef CHECK_OSIPOINT_REGISTERS
    jit::JitOptions.checkOsiPointRegisters = true;
#endif
    args.rval().setUndefined();
    return true;
}

static bool
DisplayName(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject() || !args[0].toObject().is<JSFunction>()) {
        RootedObject arg(cx, &args.callee());
        ReportUsageError(cx, arg, "Must have one function argument");
        return false;
    }

    JSFunction* fun = &args[0].toObject().as<JSFunction>();
    JSString* str = fun->displayAtom();
    args.rval().setString(str ? str : cx->runtime()->emptyString);
    return true;
}

static JSObject*
ShellObjectMetadataCallback(JSContext* cx, JSObject*)
{
    AutoEnterOOMUnsafeRegion oomUnsafe;

    RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        oomUnsafe.crash("ShellObjectMetadataCallback");

    RootedObject stack(cx, NewDenseEmptyArray(cx));
    if (!stack)
        oomUnsafe.crash("ShellObjectMetadataCallback");

    static int createdIndex = 0;
    createdIndex++;

    if (!JS_DefineProperty(cx, obj, "index", createdIndex, 0,
                           JS_STUBGETTER, JS_STUBSETTER))
    {
        oomUnsafe.crash("ShellObjectMetadataCallback");
    }

    if (!JS_DefineProperty(cx, obj, "stack", stack, 0,
                           JS_STUBGETTER, JS_STUBSETTER))
    {
        oomUnsafe.crash("ShellObjectMetadataCallback");
    }

    int stackIndex = 0;
    RootedId id(cx);
    RootedValue callee(cx);
    for (NonBuiltinScriptFrameIter iter(cx); !iter.done(); ++iter) {
        if (iter.isFunctionFrame() && iter.compartment() == cx->compartment()) {
            id = INT_TO_JSID(stackIndex);
            RootedObject callee(cx, iter.callee(cx));
            if (!JS_DefinePropertyById(cx, stack, id, callee, 0,
                                       JS_STUBGETTER, JS_STUBSETTER))
            {
                oomUnsafe.crash("ShellObjectMetadataCallback");
            }
            stackIndex++;
        }
    }

    return obj;
}

static bool
EnableShellObjectMetadataCallback(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    SetObjectMetadataCallback(cx, ShellObjectMetadataCallback);

    args.rval().setUndefined();
    return true;
}

static bool
GetObjectMetadata(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !args[0].isObject()) {
        JS_ReportError(cx, "Argument must be an object");
        return false;
    }

    args.rval().setObjectOrNull(GetObjectMetadata(&args[0].toObject()));
    return true;
}

static bool
testingFunc_bailout(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
testingFunc_inJit(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!jit::IsBaselineEnabled(cx)) {
        JSString* error = JS_NewStringCopyZ(cx, "Baseline is disabled.");
        if(!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    JSScript* script = cx->currentScript();
    if (script && script->getWarmUpResetCount() >= 20) {
        JSString* error = JS_NewStringCopyZ(cx, "Compilation is being repeatedly prevented. Giving up.");
        if (!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    args.rval().setBoolean(cx->currentlyRunningInJit());
    return true;
}

static bool
testingFunc_inIon(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!jit::IsIonEnabled(cx)) {
        JSString* error = JS_NewStringCopyZ(cx, "Ion is disabled.");
        if (!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    ScriptFrameIter iter(cx);
    if (iter.isIon()) {
        // Reset the counter of the IonScript's script.
        jit::JitFrameIterator jitIter(cx);
        ++jitIter;
        jitIter.script()->resetWarmUpResetCounter();
    } else {
        // Check if we missed multiple attempts at compiling the innermost script.
        JSScript* script = cx->currentScript();
        if (script && script->getWarmUpResetCount() >= 20) {
            JSString* error = JS_NewStringCopyZ(cx, "Compilation is being repeatedly prevented. Giving up.");
            if (!error)
                return false;

            args.rval().setString(error);
            return true;
        }
    }

    args.rval().setBoolean(iter.isIon());
    return true;
}

bool
js::testingFunc_assertFloat32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportError(cx, "Expects only 2 arguments");
        return false;
    }

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
TestingFunc_assertJitStackInvariants(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    jit::AssertJitStackInvariants(cx);
    args.rval().setUndefined();
    return true;
}

bool
js::testingFunc_assertRecoveredOnBailout(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportError(cx, "Expects only 2 arguments");
        return false;
    }

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
SetJitCompilerOption(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());

    if (args.length() != 2) {
        ReportUsageError(cx, callee, "Wrong number of arguments.");
        return false;
    }

    if (!args[0].isString()) {
        ReportUsageError(cx, callee, "First argument must be a String.");
        return false;
    }

    if (!args[1].isInt32()) {
        ReportUsageError(cx, callee, "Second argument must be an Int32.");
        return false;
    }

    JSFlatString* strArg = JS_FlattenString(cx, args[0].toString());

#define JIT_COMPILER_MATCH(key, string)                 \
    else if (JS_FlatStringEqualsAscii(strArg, string))  \
        opt = JSJITCOMPILER_ ## key;

    JSJitCompilerOption opt = JSJITCOMPILER_NOT_AN_OPTION;
    if (false) {}
    JIT_COMPILER_OPTIONS(JIT_COMPILER_MATCH);
#undef JIT_COMPILER_MATCH

    if (opt == JSJITCOMPILER_NOT_AN_OPTION) {
        ReportUsageError(cx, callee, "First argument does not name a valid option (see jsapi.h).");
        return false;
    }

    int32_t number = args[1].toInt32();
    if (number < 0)
        number = -1;

    // Throw if disabling the JITs and there's JIT code on the stack, to avoid
    // assertion failures.
    if ((opt == JSJITCOMPILER_BASELINE_ENABLE || opt == JSJITCOMPILER_ION_ENABLE) &&
        number == 0)
    {
        js::jit::JitActivationIterator iter(cx->runtime());
        if (!iter.done()) {
            JS_ReportError(cx, "Can't turn off JITs with JIT code on the stack.");
            return false;
        }
    }

    JS_SetGlobalJitCompilerOption(cx->runtime(), opt, uint32_t(number));

    args.rval().setUndefined();
    return true;
}

static bool
GetJitCompilerOptions(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject info(cx, JS_NewPlainObject(cx));
    if (!info)
        return false;

    RootedValue value(cx);

#define JIT_COMPILER_MATCH(key, string)                                \
    opt = JSJITCOMPILER_ ## key;                                       \
    value.setInt32(JS_GetGlobalJitCompilerOption(cx->runtime(), opt)); \
    if (!JS_SetProperty(cx, info, string, value))                      \
        return false;

    JSJitCompilerOption opt = JSJITCOMPILER_NOT_AN_OPTION;
    JIT_COMPILER_OPTIONS(JIT_COMPILER_MATCH);
#undef JIT_COMPILER_MATCH

    args.rval().setObject(*info);

    return true;
}

static bool
SetIonCheckGraphCoherency(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    jit::JitOptions.checkGraphConsistency = ToBoolean(args.get(0));
    args.rval().setUndefined();
    return true;
}

class CloneBufferObject : public NativeObject {
    static const JSPropertySpec props_[2];
    static const size_t DATA_SLOT   = 0;
    static const size_t LENGTH_SLOT = 1;
    static const size_t NUM_SLOTS   = 2;

  public:
    static const Class class_;

    static CloneBufferObject* Create(JSContext* cx) {
        RootedObject obj(cx, JS_NewObject(cx, Jsvalify(&class_)));
        if (!obj)
            return nullptr;
        obj->as<CloneBufferObject>().setReservedSlot(DATA_SLOT, PrivateValue(nullptr));
        obj->as<CloneBufferObject>().setReservedSlot(LENGTH_SLOT, Int32Value(0));

        if (!JS_DefineProperties(cx, obj, props_))
            return nullptr;

        return &obj->as<CloneBufferObject>();
    }

    static CloneBufferObject* Create(JSContext* cx, JSAutoStructuredCloneBuffer* buffer) {
        Rooted<CloneBufferObject*> obj(cx, Create(cx));
        if (!obj)
            return nullptr;
        uint64_t* datap;
        size_t nbytes;
        buffer->steal(&datap, &nbytes);
        obj->setData(datap);
        obj->setNBytes(nbytes);
        return obj;
    }

    uint64_t* data() const {
        return static_cast<uint64_t*>(getReservedSlot(DATA_SLOT).toPrivate());
    }

    void setData(uint64_t* aData) {
        MOZ_ASSERT(!data());
        setReservedSlot(DATA_SLOT, PrivateValue(aData));
    }

    size_t nbytes() const {
        return getReservedSlot(LENGTH_SLOT).toInt32();
    }

    void setNBytes(size_t nbytes) {
        MOZ_ASSERT(nbytes <= UINT32_MAX);
        setReservedSlot(LENGTH_SLOT, Int32Value(nbytes));
    }

    // Discard an owned clone buffer.
    void discard() {
        if (data())
            JS_ClearStructuredClone(data(), nbytes(), nullptr, nullptr);
        setReservedSlot(DATA_SLOT, PrivateValue(nullptr));
    }

    static bool
    setCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
        if (args.length() != 1 || !args[0].isString()) {
            JS_ReportError(cx,
                           "the first argument argument must be maxBytes, "
                           "maxMallocBytes, gcStackpoolLifespan, gcBytes or "
                           "gcNumber");
            JS_ReportError(cx, "clonebuffer setter requires a single string argument");
            return false;
        }

        if (fuzzingSafe) {
            // A manually-created clonebuffer could easily trigger a crash
            args.rval().setUndefined();
            return true;
        }

        Rooted<CloneBufferObject*> obj(cx, &args.thisv().toObject().as<CloneBufferObject>());
        obj->discard();

        char* str = JS_EncodeString(cx, args[0].toString());
        if (!str)
            return false;
        obj->setData(reinterpret_cast<uint64_t*>(str));
        obj->setNBytes(JS_GetStringLength(args[0].toString()));

        args.rval().setUndefined();
        return true;
    }

    static bool
    is(HandleValue v) {
        return v.isObject() && v.toObject().is<CloneBufferObject>();
    }

    static bool
    setCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod<is, setCloneBuffer_impl>(cx, args);
    }

    static bool
    getCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
        Rooted<CloneBufferObject*> obj(cx, &args.thisv().toObject().as<CloneBufferObject>());
        MOZ_ASSERT(args.length() == 0);

        if (!obj->data()) {
            args.rval().setUndefined();
            return true;
        }

        bool hasTransferable;
        if (!JS_StructuredCloneHasTransferables(obj->data(), obj->nbytes(), &hasTransferable))
            return false;

        if (hasTransferable) {
            JS_ReportError(cx, "cannot retrieve structured clone buffer with transferables");
            return false;
        }

        JSString* str = JS_NewStringCopyN(cx, reinterpret_cast<char*>(obj->data()), obj->nbytes());
        if (!str)
            return false;
        args.rval().setString(str);
        return true;
    }

    static bool
    getCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod<is, getCloneBuffer_impl>(cx, args);
    }

    static void Finalize(FreeOp* fop, JSObject* obj) {
        obj->as<CloneBufferObject>().discard();
    }
};

const Class CloneBufferObject::class_ = {
    "CloneBuffer", JSCLASS_HAS_RESERVED_SLOTS(CloneBufferObject::NUM_SLOTS),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    Finalize
};

const JSPropertySpec CloneBufferObject::props_[] = {
    JS_PSGS("clonebuffer", getCloneBuffer, setCloneBuffer, 0),
    JS_PS_END
};

static bool
Serialize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSAutoStructuredCloneBuffer clonebuf;
    if (!clonebuf.write(cx, args.get(0), args.get(1)))
        return false;

    RootedObject obj(cx, CloneBufferObject::Create(cx, &clonebuf));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
Deserialize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1 || !args[0].isObject()) {
        JS_ReportError(cx, "deserialize requires a single clonebuffer argument");
        return false;
    }

    if (!args[0].toObject().is<CloneBufferObject>()) {
        JS_ReportError(cx, "deserialize requires a clonebuffer");
        return false;
    }

    Rooted<CloneBufferObject*> obj(cx, &args[0].toObject().as<CloneBufferObject>());

    // Clone buffer was already consumed?
    if (!obj->data()) {
        JS_ReportError(cx, "deserialize given invalid clone buffer "
                       "(transferables already consumed?)");
        return false;
    }

    bool hasTransferable;
    if (!JS_StructuredCloneHasTransferables(obj->data(), obj->nbytes(), &hasTransferable))
        return false;

    RootedValue deserialized(cx);
    if (!JS_ReadStructuredClone(cx, obj->data(), obj->nbytes(),
                                JS_STRUCTURED_CLONE_VERSION, &deserialized, nullptr, nullptr)) {
        return false;
    }
    args.rval().set(deserialized);

    if (hasTransferable)
        obj->discard();

    return true;
}

static bool
Neuter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 2) {
        JS_ReportError(cx, "wrong number of arguments to neuter()");
        return false;
    }

    RootedObject obj(cx);
    if (!JS_ValueToObject(cx, args[0], &obj))
        return false;

    if (!obj) {
        JS_ReportError(cx, "neuter must be passed an object");
        return false;
    }

    NeuterDataDisposition changeData;
    RootedString str(cx, JS::ToString(cx, args[1]));
    if (!str)
        return false;
    JSAutoByteString dataDisposition(cx, str);
    if (!dataDisposition)
        return false;
    if (strcmp(dataDisposition.ptr(), "same-data") == 0) {
        changeData = KeepData;
    } else if (strcmp(dataDisposition.ptr(), "change-data") == 0) {
        changeData = ChangeData;
    } else {
        JS_ReportError(cx, "unknown parameter 2 to neuter()");
        return false;
    }

    if (!JS_NeuterArrayBuffer(cx, obj, changeData))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
HelperThreadCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#ifdef JS_MORE_DETERMINISTIC
    // Always return 0 to get consistent output with and without --no-threads.
    args.rval().setInt32(0);
#else
    if (CanUseExtraThreads())
        args.rval().setInt32(HelperThreadState().threadCount);
    else
        args.rval().setInt32(0);
#endif
    return true;
}

static bool
TimesAccessed(JSContext* cx, unsigned argc, Value* vp)
{
    static int32_t accessed = 0;
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(++accessed);
    return true;
}

#ifdef JS_TRACE_LOGGING
static bool
EnableTraceLogger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    if (!TraceLoggerEnable(logger, cx))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
DisableTraceLogger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    args.rval().setBoolean(TraceLoggerDisable(logger));

    return true;
}
#endif

#ifdef DEBUG
static bool
DumpObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.get(0)));
    if (!obj)
        return false;

    DumpObject(obj);

    args.rval().setUndefined();
    return true;
}
#endif

#ifdef NIGHTLY_BUILD
static bool
ObjectAddress(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }
    if (!args[0].isObject()) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Expected object");
        return false;
    }

#ifdef JS_MORE_DETERMINISTIC
    args.rval().setInt32(0);
#else
    void* ptr = js::UncheckedUnwrap(&args[0].toObject(), true);
    char buffer[64];
    JS_snprintf(buffer, sizeof(buffer), "%p", ptr);

    JSString* str = JS_NewStringCopyZ(cx, buffer);
    if (!str)
        return false;

    args.rval().setString(str);
#endif

    return true;
}

static bool
SharedAddress(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Wrong number of arguments");
        return false;
    }
    if (!args[0].isObject()) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Expected object");
        return false;
    }

#ifdef JS_MORE_DETERMINISTIC
    args.rval().setString(cx->staticStrings().getUint(0));
#else
    RootedObject obj(cx, CheckedUnwrap(&args[0].toObject()));
    if (!obj) {
        JS_ReportError(cx, "Permission denied to access object");
        return false;
    }
    if (!obj->is<SharedArrayBufferObject>()) {
        JS_ReportError(cx, "Argument must be a SharedArrayBuffer");
        return false;
    }
    char buffer[64];
    uint32_t nchar =
        JS_snprintf(buffer, sizeof(buffer), "%p",
                    obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(/*safeish*/));

    JSString* str = JS_NewStringCopyN(cx, buffer, nchar);
    if (!str)
        return false;

    args.rval().setString(str);
#endif

    return true;
}
#endif

static bool
DumpBacktrace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    DumpBacktrace(cx);
    args.rval().setUndefined();
    return true;
}

static bool
GetBacktrace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool showArgs = false;
    bool showLocals = false;
    bool showThisProps = false;

    if (args.length() > 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageError(cx, callee, "Too many arguments");
        return false;
    }

    if (args.length() == 1) {
        RootedObject cfg(cx, ToObject(cx, args[0]));
        if (!cfg)
            return false;
        RootedValue v(cx);

        if (!JS_GetProperty(cx, cfg, "args", &v))
            return false;
        showArgs = ToBoolean(v);

        if (!JS_GetProperty(cx, cfg, "locals", &v))
            return false;
        showLocals = ToBoolean(v);

        if (!JS_GetProperty(cx, cfg, "thisprops", &v))
            return false;
        showThisProps = ToBoolean(v);
    }

    char* buf = JS::FormatStackDump(cx, nullptr, showArgs, showLocals, showThisProps);
    if (!buf)
        return false;

    RootedString str(cx);
    if (!(str = JS_NewStringCopyZ(cx, buf)))
        return false;
    JS_smprintf_free(buf);

    args.rval().setString(str);
    return true;
}

static bool
ReportOutOfMemory(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JS_ReportOutOfMemory(cx);
    cx->clearPendingException();
    args.rval().setUndefined();
    return true;
}

static bool
ReportLargeAllocationFailure(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    void* buf = cx->runtime()->onOutOfMemoryCanGC(AllocFunction::Malloc, JSRuntime::LARGE_ALLOCATION);
    js_free(buf);
    args.rval().setUndefined();
    return true;
}

namespace heaptools {

typedef UniquePtr<char16_t[], JS::FreePolicy> EdgeName;

// An edge to a node from its predecessor in a path through the graph.
class BackEdge {
    // The node from which this edge starts.
    JS::ubi::Node predecessor_;

    // The name of this edge.
    EdgeName name_;

  public:
    BackEdge() : name_(nullptr) { }
    // Construct an initialized back edge, taking ownership of |name|.
    BackEdge(JS::ubi::Node predecessor, EdgeName name)
        : predecessor_(predecessor), name_(Move(name)) { }
    BackEdge(BackEdge&& rhs) : predecessor_(rhs.predecessor_), name_(Move(rhs.name_)) { }
    BackEdge& operator=(BackEdge&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~BackEdge();
        new(this) BackEdge(Move(rhs));
        return *this;
    }

    EdgeName forgetName() { return Move(name_); }
    JS::ubi::Node predecessor() const { return predecessor_; }

  private:
    // No copy constructor or copying assignment.
    BackEdge(const BackEdge&) = delete;
    BackEdge& operator=(const BackEdge&) = delete;
};

// A path-finding handler class for use with JS::ubi::BreadthFirst.
struct FindPathHandler {
    typedef BackEdge NodeData;
    typedef JS::ubi::BreadthFirst<FindPathHandler> Traversal;

    FindPathHandler(JSContext*cx, JS::ubi::Node start, JS::ubi::Node target,
                    AutoValueVector& nodes, Vector<EdgeName>& edges)
      : cx(cx), start(start), target(target), foundPath(false),
        nodes(nodes), edges(edges) { }

    bool
    operator()(Traversal& traversal, JS::ubi::Node origin, const JS::ubi::Edge& edge,
               BackEdge* backEdge, bool first)
    {
        // We take care of each node the first time we visit it, so there's
        // nothing to be done on subsequent visits.
        if (!first)
            return true;

        // Record how we reached this node. This is the last edge on a
        // shortest path to this node.
        EdgeName edgeName = DuplicateString(cx, edge.name.get());
        if (!edgeName)
            return false;
        *backEdge = mozilla::Move(BackEdge(origin, Move(edgeName)));

        // Have we reached our final target node?
        if (edge.referent == target) {
            // Record the path that got us here, which must be a shortest path.
            if (!recordPath(traversal))
                return false;
            foundPath = true;
            traversal.stop();
        }

        return true;
    }

    // We've found a path to our target. Walk the backlinks to produce the
    // (reversed) path, saving the path in |nodes| and |edges|. |nodes| is
    // rooted, so it can hold the path's nodes as we leave the scope of
    // the AutoCheckCannotGC.
    bool recordPath(Traversal& traversal) {
        JS::ubi::Node here = target;

        do {
            Traversal::NodeMap::Ptr p = traversal.visited.lookup(here);
            MOZ_ASSERT(p);
            JS::ubi::Node predecessor = p->value().predecessor();
            if (!nodes.append(predecessor.exposeToJS()) ||
                !edges.append(p->value().forgetName()))
                return false;
            here = predecessor;
        } while (here != start);

        return true;
    }

    JSContext* cx;

    // The node we're starting from.
    JS::ubi::Node start;

    // The node we're looking for.
    JS::ubi::Node target;

    // True if we found a path to target, false if we didn't.
    bool foundPath;

    // The nodes and edges of the path --- should we find one. The path is
    // stored in reverse order, because that's how it's easiest for us to
    // construct it:
    // - edges[i] is the name of the edge from nodes[i] to nodes[i-1].
    // - edges[0] is the name of the edge from nodes[0] to the target.
    // - The last node, nodes[n-1], is the start node.
    AutoValueVector& nodes;
    Vector<EdgeName>& edges;
};

} // namespace heaptools

static bool
FindPath(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (argc < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "findPath", "1", "");
        return false;
    }

    // We don't ToString non-objects given as 'start' or 'target', because this
    // test is all about object identity, and ToString doesn't preserve that.
    // Non-GCThing endpoints don't make much sense.
    if (!args[0].isObject() && !args[0].isString() && !args[0].isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[0], nullptr,
                              "not an object, string, or symbol", NULL);
        return false;
    }

    if (!args[1].isObject() && !args[1].isString() && !args[1].isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[0], nullptr,
                              "not an object, string, or symbol", NULL);
        return false;
    }

    AutoValueVector nodes(cx);
    Vector<heaptools::EdgeName> edges(cx);

    {
        // We can't tolerate the GC moving things around while we're searching
        // the heap. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node start(args[0]), target(args[1]);

        heaptools::FindPathHandler handler(cx, start, target, nodes, edges);
        heaptools::FindPathHandler::Traversal traversal(cx->runtime(), handler, autoCannotGC);
        if (!traversal.init() || !traversal.addStart(start))
            return false;

        if (!traversal.traverse())
            return false;

        if (!handler.foundPath) {
            // We didn't find any paths from the start to the target.
            args.rval().setUndefined();
            return true;
        }
    }

    // |nodes| and |edges| contain the path from |start| to |target|, reversed.
    // Construct a JavaScript array describing the path from the start to the
    // target. Each element has the form:
    //
    //   {
    //     node: <object or string or symbol>,
    //     edge: <string describing outgoing edge from node>
    //   }
    //
    // or, if the node is some internal thing that isn't a proper JavaScript
    // value:
    //
    //   { node: undefined, edge: <string> }
    size_t length = nodes.length();
    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, length);

    // Walk |nodes| and |edges| in the stored order, and construct the result
    // array in start-to-target order.
    for (size_t i = 0; i < length; i++) {
        // Build an object describing the node and edge.
        RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        if (!JS_DefineProperty(cx, obj, "node", nodes[i],
                               JSPROP_ENUMERATE, nullptr, nullptr))
            return false;

        heaptools::EdgeName edgeName = Move(edges[i]);

        RootedString edgeStr(cx, NewString<CanGC>(cx, edgeName.get(), js_strlen(edgeName.get())));
        if (!edgeStr)
            return false;
        edgeName.release(); // edgeStr acquired ownership

        if (!JS_DefineProperty(cx, obj, "edge", edgeStr, JSPROP_ENUMERATE, nullptr, nullptr))
            return false;

        result->setDenseElement(length - i - 1, ObjectValue(*obj));
    }

    args.rval().setObject(*result);
    return true;
}

static bool
EvalReturningScope(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "evalReturningScope", 1))
        return false;

    RootedString str(cx, ToString(cx, args[0]));
    if (!str)
        return false;

    RootedObject global(cx);
    if (args.hasDefined(1)) {
        global = ToObject(cx, args[1]);
        if (!global)
            return false;
    }

    AutoStableStringChars strChars(cx);
    if (!strChars.initTwoByte(cx, str))
        return false;

    mozilla::Range<const char16_t> chars = strChars.twoByteRange();
    size_t srclen = chars.length();
    const char16_t* src = chars.start().get();

    JS::AutoFilename filename;
    unsigned lineno;

    DescribeScriptedCaller(cx, &filename, &lineno);

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), lineno);
    options.setNoScriptRval(true);

    JS::SourceBufferHolder srcBuf(src, srclen, JS::SourceBufferHolder::NoOwnership);
    RootedScript script(cx);
    if (!JS::CompileForNonSyntacticScope(cx, options, srcBuf, &script))
        return false;

    if (global) {
        global = CheckedUnwrap(global);
        if (!global) {
            JS_ReportError(cx, "Permission denied to access global");
            return false;
        }
        if (!global->is<GlobalObject>()) {
            JS_ReportError(cx, "Argument must be a global object");
            return false;
        }
    } else {
        global = JS::CurrentGlobalOrNull(cx);
    }

    RootedObject varObj(cx);
    RootedObject lexicalScope(cx);

    {
        // If we're switching globals here, ExecuteInGlobalAndReturnScope will
        // take care of cloning the script into that compartment before
        // executing it.
        AutoCompartment ac(cx, global);

        if (!js::ExecuteInGlobalAndReturnScope(cx, global, script, &lexicalScope))
            return false;

        varObj = lexicalScope->enclosingScope();
    }

    RootedObject rv(cx, JS_NewPlainObject(cx));
    if (!rv)
        return false;

    RootedValue varObjVal(cx, ObjectValue(*varObj));
    if (!cx->compartment()->wrap(cx, &varObjVal))
        return false;
    if (!JS_SetProperty(cx, rv, "vars", varObjVal))
        return false;

    RootedValue lexicalScopeVal(cx, ObjectValue(*lexicalScope));
    if (!cx->compartment()->wrap(cx, &lexicalScopeVal))
        return false;
    if (!JS_SetProperty(cx, rv, "lexicals", lexicalScopeVal))
        return false;

    args.rval().setObject(*rv);
    return true;
}

static bool
ShellCloneAndExecuteScript(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "cloneAndExecuteScript", 2))
        return false;

    RootedString str(cx, ToString(cx, args[0]));
    if (!str)
        return false;

    RootedObject global(cx, ToObject(cx, args[1]));
    if (!global)
        return false;

    AutoStableStringChars strChars(cx);
    if (!strChars.initTwoByte(cx, str))
        return false;

    mozilla::Range<const char16_t> chars = strChars.twoByteRange();
    size_t srclen = chars.length();
    const char16_t* src = chars.start().get();

    JS::AutoFilename filename;
    unsigned lineno;

    DescribeScriptedCaller(cx, &filename, &lineno);

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), lineno);
    options.setNoScriptRval(true);

    JS::SourceBufferHolder srcBuf(src, srclen, JS::SourceBufferHolder::NoOwnership);
    RootedScript script(cx);
    if (!JS::Compile(cx, options, srcBuf, &script))
        return false;

    global = CheckedUnwrap(global);
    if (!global) {
        JS_ReportError(cx, "Permission denied to access global");
        return false;
    }
    if (!global->is<GlobalObject>()) {
        JS_ReportError(cx, "Argument must be a global object");
        return false;
    }

    AutoCompartment ac(cx, global);

    if (!JS::CloneAndExecuteScript(cx, script))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
IsSimdAvailable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#ifdef JS_CODEGEN_NONE
    bool available = false;
#else
    bool available = cx->jitSupportsSimd();
#endif
    args.rval().set(BooleanValue(available));
    return true;
}

static bool
ByteSize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    {
        // We can't tolerate the GC moving things around while we're using a
        // ubi::Node. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node node = args.get(0);
        if (node)
            args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
        else
            args.rval().setUndefined();
    }
    return true;
}

static bool
ByteSizeOfScript(JSContext*cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "byteSizeOfScript", 1))
        return false;
    if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportError(cx, "Argument must be a Function object");
        return false;
    }

    JSFunction* fun = &args[0].toObject().as<JSFunction>();
    if (fun->isNative()) {
        JS_ReportError(cx, "Argument must be a scripted function");
        return false;
    }

    RootedScript script(cx, fun->getOrCreateScript(cx));
    if (!script)
        return false;

    mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    {
        // We can't tolerate the GC moving things around while we're using a
        // ubi::Node. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node node = script;
        if (node)
            args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
        else
            args.rval().setUndefined();
    }
    return true;
}

static bool
ImmutablePrototypesEnabled(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    args.rval().setBoolean(JS_ImmutablePrototypesEnabled());
    return true;
}

static bool
SetImmutablePrototype(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject()) {
        JS_ReportError(cx, "setImmutablePrototype: object expected");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());

    bool succeeded;
    if (!js::SetImmutablePrototype(cx, obj, &succeeded))
        return false;

    args.rval().setBoolean(succeeded);
    return true;
}

#ifdef DEBUG
static bool
DumpStringRepresentation(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedString str(cx, ToString(cx, args.get(0)));
    if (!str)
        return false;

    str->dumpRepresentation(stderr, 0);

    args.rval().setUndefined();
    return true;
}
#endif

static bool
SetLazyParsingDisabled(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool disable = !args.hasDefined(0) || ToBoolean(args[0]);
    JS::CompartmentOptionsRef(cx->compartment()).setDisableLazyParsing(disable);

    args.rval().setUndefined();
    return true;
}

static bool
SetDiscardSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool discard = !args.hasDefined(0) || ToBoolean(args[0]);
    JS::CompartmentOptionsRef(cx->compartment()).setDiscardSource(discard);

    args.rval().setUndefined();
    return true;
}

static bool
GetConstructorName(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "getConstructorName", 1))
        return false;

    if (!args[0].isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                             "getConstructorName", "Object",
                             InformalValueTypeName(args[0]));
        return false;
    }

    RootedAtom name(cx);
    if (!args[0].toObject().constructorDisplayAtom(cx, &name))
        return false;

    if (name) {
        args.rval().setString(name);
    } else {
        args.rval().setNull();
    }
    return true;
}

static bool
AllocationMarker(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool allocateInsideNursery = true;
    if (args.length() > 0 && args[0].isObject()) {
        RootedObject options(cx, &args[0].toObject());

        RootedValue nurseryVal(cx);
        if (!JS_GetProperty(cx, options, "nursery", &nurseryVal))
            return false;
        allocateInsideNursery = ToBoolean(nurseryVal);
    }

    static const Class cls = { "AllocationMarker" };

    auto newKind = allocateInsideNursery ? GenericObject : TenuredObject;
    RootedObject obj(cx, NewObjectWithGivenProto(cx, &cls, nullptr, newKind));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

namespace gcCallback {

struct MajorGC {
    int32_t depth;
    int32_t phases;
};

static void
majorGC(JSRuntime* rt, JSGCStatus status, void* data)
{
    auto info = static_cast<MajorGC*>(data);
    if (!(info->phases & (1 << status)))
        return;

    if (info->depth > 0) {
        info->depth--;
        JS::PrepareForFullGC(rt);
        JS::GCForReason(rt, GC_NORMAL, JS::gcreason::API);
        info->depth++;
    }
}

struct MinorGC {
    int32_t phases;
    bool active;
};

static void
minorGC(JSRuntime* rt, JSGCStatus status, void* data)
{
    auto info = static_cast<MinorGC*>(data);
    if (!(info->phases & (1 << status)))
        return;

    if (info->active) {
        info->active = false;
        rt->gc.evictNursery(JS::gcreason::DEBUG_GC);
        info->active = true;
    }
}

// Process global, should really be runtime-local. Also, the final one of these
// is currently leaked, since they are only deleted when changing.
MajorGC* prevMajorGC = nullptr;
MinorGC* prevMinorGC = nullptr;

} /* namespace gcCallback */

static bool
SetGCCallback(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportError(cx, "Wrong number of arguments");
        return false;
    }

    RootedObject opts(cx, ToObject(cx, args[0]));
    if (!opts)
        return false;

    RootedValue v(cx);
    if (!JS_GetProperty(cx, opts, "action", &v))
        return false;

    JSString* str = JS::ToString(cx, v);
    if (!str)
        return false;
    JSAutoByteString action(cx, str);
    if (!action)
        return false;

    int32_t phases = 0;
    if ((strcmp(action.ptr(), "minorGC") == 0) || (strcmp(action.ptr(), "majorGC") == 0)) {
        if (!JS_GetProperty(cx, opts, "phases", &v))
            return false;
        if (v.isUndefined()) {
            phases = (1 << JSGC_END);
        } else {
            JSString* str = JS::ToString(cx, v);
            if (!str)
                return false;
            JSAutoByteString phasesStr(cx, str);
            if (!phasesStr)
                return false;

            if (strcmp(phasesStr.ptr(), "begin") == 0)
                phases = (1 << JSGC_BEGIN);
            else if (strcmp(phasesStr.ptr(), "end") == 0)
                phases = (1 << JSGC_END);
            else if (strcmp(phasesStr.ptr(), "both") == 0)
                phases = (1 << JSGC_BEGIN) | (1 << JSGC_END);
            else {
                JS_ReportError(cx, "Invalid callback phase");
                return false;
            }
        }
    }

    if (gcCallback::prevMajorGC) {
        JS_SetGCCallback(cx->runtime(), nullptr, nullptr);
        js_delete<gcCallback::MajorGC>(gcCallback::prevMajorGC);
        gcCallback::prevMajorGC = nullptr;
    }

    if (gcCallback::prevMinorGC) {
        JS_SetGCCallback(cx->runtime(), nullptr, nullptr);
        js_delete<gcCallback::MinorGC>(gcCallback::prevMinorGC);
        gcCallback::prevMinorGC = nullptr;
    }

    if (strcmp(action.ptr(), "minorGC") == 0) {
        auto info = js_new<gcCallback::MinorGC>();
        info->phases = phases;
        info->active = true;
        JS_SetGCCallback(cx->runtime(), gcCallback::minorGC, info);
    } else if (strcmp(action.ptr(), "majorGC") == 0) {
        if (!JS_GetProperty(cx, opts, "depth", &v))
            return false;
        int32_t depth = 1;
        if (!v.isUndefined()) {
            if (!ToInt32(cx, v, &depth))
                return false;
        }
        if (depth > int32_t(gcstats::Statistics::MAX_NESTING - 4)) {
            JS_ReportError(cx, "Nesting depth too large, would overflow");
            return false;
        }

        auto info = js_new<gcCallback::MajorGC>();
        info->phases = phases;
        info->depth = depth;
        JS_SetGCCallback(cx->runtime(), gcCallback::majorGC, info);
    } else {
        JS_ReportError(cx, "Unknown GC callback action");
        return false;
    }

    args.rval().setUndefined();
    return true;
}

static bool
SetARMHwCapFlags(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportError(cx, "Wrong number of arguments");
        return false;
    }

    RootedString flagsListString(cx, JS::ToString(cx, args.get(0)));
    if (!flagsListString)
        return false;

#if defined(JS_CODEGEN_ARM)
    JSAutoByteString flagsList(cx, flagsListString);
    if (!flagsList)
        return false;

    jit::ParseARMHwCapFlags(flagsList.ptr());
#endif

    args.rval().setUndefined();
    return true;
}

static bool
GetLcovInfo(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 1) {
        JS_ReportError(cx, "Wrong number of arguments");
        return false;
    }

    RootedObject global(cx);
    if (args.hasDefined(0)) {
        global = ToObject(cx, args[0]);
        if (!global) {
            JS_ReportError(cx, "First argument should be an object");
            return false;
        }
        global = CheckedUnwrap(global);
        if (!global) {
            JS_ReportError(cx, "Permission denied to access global");
            return false;
        }
        if (!global->is<GlobalObject>()) {
            JS_ReportError(cx, "Argument must be a global object");
            return false;
        }
    } else {
        global = JS::CurrentGlobalOrNull(cx);
    }

    size_t length = 0;
    char* content = nullptr;
    {
        AutoCompartment ac(cx, global);
        content = js::GetCodeCoverageSummary(cx, &length);
    }

    if (!content)
        return false;

    JSString* str = JS_NewStringCopyN(cx, content, length);
    free(content);

    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

#ifdef DEBUG
static bool
SetRNGState(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "SetRNGState", 2))
        return false;

    double d0;
    if (!ToNumber(cx, args[0], &d0))
        return false;

    double d1;
    if (!ToNumber(cx, args[1], &d1))
        return false;

    uint64_t seed0 = static_cast<uint64_t>(d0);
    uint64_t seed1 = static_cast<uint64_t>(d1);

    if (seed0 == 0 && seed1 == 0) {
        JS_ReportError(cx, "RNG requires non-zero seed");
        return false;
    }

    cx->compartment()->ensureRandomNumberGenerator();
    cx->compartment()->randomNumberGenerator.ref().setState(seed0, seed1);

    args.rval().setUndefined();
    return true;
}
#endif

static ModuleEnvironmentObject*
GetModuleEnvironment(JSContext* cx, HandleValue moduleValue)
{
    RootedModuleObject module(cx, &moduleValue.toObject().as<ModuleObject>());

    // Use the initial environment so that tests can check bindings exists
    // before they have been instantiated.
    RootedModuleEnvironmentObject env(cx, &module->initialEnvironment());
    MOZ_ASSERT(env);
    MOZ_ASSERT_IF(module->environment(), module->environment() == env);

    return env;
}

static bool
GetModuleEnvironmentNames(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportError(cx, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<ModuleObject>()) {
        JS_ReportError(cx, "First argument should be a ModuleObject");
        return false;
    }

    RootedModuleEnvironmentObject env(cx, GetModuleEnvironment(cx, args[0]));
    Rooted<IdVector> ids(cx, IdVector(cx));
    if (!JS_Enumerate(cx, env, &ids))
        return false;

    uint32_t length = ids.length();
    RootedArrayObject array(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!array)
        return false;

    array->setDenseInitializedLength(length);
    for (uint32_t i = 0; i < length; i++)
        array->initDenseElement(i, StringValue(JSID_TO_STRING(ids[i])));

    args.rval().setObject(*array);
    return true;
}

static bool
GetModuleEnvironmentValue(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportError(cx, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<ModuleObject>()) {
        JS_ReportError(cx, "First argument should be a ModuleObject");
        return false;
    }

    if (!args[1].isString()) {
        JS_ReportError(cx, "Second argument should be a string");
        return false;
    }

    RootedModuleEnvironmentObject env(cx, GetModuleEnvironment(cx, args[0]));
    RootedString name(cx, args[1].toString());
    RootedId id(cx);
    if (!JS_StringToId(cx, name, &id))
        return false;

    return GetProperty(cx, env, env, id, args.rval());
}

static const JSFunctionSpecWithHelp TestingFunctions[] = {
    JS_FN_HELP("gc", ::GC, 0, 0,
"gc([obj] | 'compartment' [, 'shrinking'])",
"  Run the garbage collector. When obj is given, GC only its compartment.\n"
"  If 'compartment' is given, GC any compartments that were scheduled for\n"
"  GC via schedulegc.\n"
"  If 'shrinking' is passed as the optional second argument, perform a\n"
"  shrinking GC rather than a normal GC."),

    JS_FN_HELP("minorgc", ::MinorGC, 0, 0,
"minorgc([aboutToOverflow])",
"  Run a minor collector on the Nursery. When aboutToOverflow is true, marks\n"
"  the store buffer as about-to-overflow before collecting."),

    JS_FN_HELP("gcparam", GCParameter, 2, 0,
"gcparam(name [, value])",
"  Wrapper for JS_[GS]etGCParameter. The name is one of " GC_PARAMETER_ARGS_LIST),

    JS_FN_HELP("relazifyFunctions", RelazifyFunctions, 0, 0,
"relazifyFunctions(...)",
"  Perform a GC and allow relazification of functions. Accepts the same\n"
"  arguments as gc()."),

    JS_FN_HELP("getBuildConfiguration", GetBuildConfiguration, 0, 0,
"getBuildConfiguration()",
"  Return an object describing some of the configuration options SpiderMonkey\n"
"  was built with."),

    JS_FN_HELP("hasChild", HasChild, 0, 0,
"hasChild(parent, child)",
"  Return true if |child| is a child of |parent|, as determined by a call to\n"
"  TraceChildren"),

    JS_FN_HELP("setSavedStacksRNGState", SetSavedStacksRNGState, 1, 0,
"setSavedStacksRNGState(seed)",
"  Set this compartment's SavedStacks' RNG state.\n"),

    JS_FN_HELP("getSavedFrameCount", GetSavedFrameCount, 0, 0,
"getSavedFrameCount()",
"  Return the number of SavedFrame instances stored in this compartment's\n"
"  SavedStacks cache."),

    JS_FN_HELP("saveStack", SaveStack, 0, 0,
"saveStack([maxDepth [, compartment]])",
"  Capture a stack. If 'maxDepth' is given, capture at most 'maxDepth' number\n"
"  of frames. If 'compartment' is given, allocate the js::SavedFrame instances\n"
"  with the given object's compartment."),

    JS_FN_HELP("callFunctionFromNativeFrame", CallFunctionFromNativeFrame, 1, 0,
"callFunctionFromNativeFrame(function)",
"  Call 'function' with a (C++-)native frame on stack.\n"
"  Required for testing that SaveStack properly handles native frames."),

    JS_FN_HELP("callFunctionWithAsyncStack", CallFunctionWithAsyncStack, 0, 0,
"callFunctionWithAsyncStack(function, stack, asyncCause)",
"  Call 'function', using the provided stack as the async stack responsible\n"
"  for the call, and propagate its return value or the exception it throws.\n"
"  The function is called with no arguments, and 'this' is 'undefined'. The\n"
"  specified |asyncCause| is attached to the provided stack frame."),

    JS_FN_HELP("enableTrackAllocations", EnableTrackAllocations, 0, 0,
"enableTrackAllocations()",
"  Start capturing the JS stack at every allocation. Note that this sets an\n"
"  object metadata callback that will override any other object metadata\n"
"  callback that may be set."),

    JS_FN_HELP("disableTrackAllocations", DisableTrackAllocations, 0, 0,
"disableTrackAllocations()",
"  Stop capturing the JS stack at every allocation."),

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    JS_FN_HELP("oomThreadTypes", OOMThreadTypes, 0, 0,
"oomThreadTypes()",
"  Get the number of thread types that can be used as an argument for\n"
"oomAfterAllocations() and oomAtAllocation()."),

    JS_FN_HELP("oomAfterAllocations", OOMAfterAllocations, 2, 0,
"oomAfterAllocations(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail every following allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("oomAtAllocation", OOMAtAllocation, 2, 0,
"oomAtAllocation(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail the next allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("resetOOMFailure", ResetOOMFailure, 0, 0,
"resetOOMFailure()",
"  Remove the allocation failure scheduled by either oomAfterAllocations() or\n"
"  oomAtAllocation() and return whether any allocation had been caused to fail."),

    JS_FN_HELP("oomTest", OOMTest, 0, 0,
"oomTest(function)",
"  Test that the passed function behaves correctly under OOM conditions by\n"
"  repeatedly executing it and simulating allocation failure at successive\n"
"  allocations until the function completes without seeing a failure."),
#endif

    JS_FN_HELP("makeFakePromise", MakeFakePromise, 0, 0,
"makeFakePromise()",
"  Create an object whose [[Class]] name is 'Promise' and call\n"
"  JS::dbg::onNewPromise on it before returning it. It doesn't actually have\n"
"  any of the other behavior associated with promises."),

    JS_FN_HELP("settleFakePromise", SettleFakePromise, 1, 0,
"settleFakePromise(promise)",
"  'Settle' a 'promise' created by makeFakePromise(). This doesn't have any\n"
"  observable effects outside of firing any onPromiseSettled hooks set on\n"
"  Debugger instances that are observing the given promise's global as a\n"
"  debuggee."),

    JS_FN_HELP("makeFinalizeObserver", MakeFinalizeObserver, 0, 0,
"makeFinalizeObserver()",
"  Get a special object whose finalization increases the counter returned\n"
"  by the finalizeCount function."),

    JS_FN_HELP("finalizeCount", FinalizeCount, 0, 0,
"finalizeCount()",
"  Return the current value of the finalization counter that is incremented\n"
"  each time an object returned by the makeFinalizeObserver is finalized."),

    JS_FN_HELP("gcPreserveCode", GCPreserveCode, 0, 0,
"gcPreserveCode()",
"  Preserve JIT code during garbage collections."),

#ifdef JS_GC_ZEAL
    JS_FN_HELP("gczeal", GCZeal, 2, 0,
"gczeal(level, [N])",
gc::ZealModeHelpText),

    JS_FN_HELP("schedulegc", ScheduleGC, 1, 0,
"schedulegc([num | obj])",
"  If num is given, schedule a GC after num allocations.\n"
"  If obj is given, schedule a GC of obj's compartment.\n"
"  Returns the number of allocations before the next trigger."),

    JS_FN_HELP("selectforgc", SelectForGC, 0, 0,
"selectforgc(obj1, obj2, ...)",
"  Schedule the given objects to be marked in the next GC slice."),

    JS_FN_HELP("verifyprebarriers", VerifyPreBarriers, 0, 0,
"verifyprebarriers()",
"  Start or end a run of the pre-write barrier verifier."),

    JS_FN_HELP("verifypostbarriers", VerifyPostBarriers, 0, 0,
"verifypostbarriers()",
"  Does nothing (the post-write barrier verifier has been remove)."),

    JS_FN_HELP("gcstate", GCState, 0, 0,
"gcstate()",
"  Report the global GC state."),

    JS_FN_HELP("deterministicgc", DeterministicGC, 1, 0,
"deterministicgc(true|false)",
"  If true, only allow determinstic GCs to run."),
#endif

    JS_FN_HELP("startgc", StartGC, 1, 0,
"startgc([n [, 'shrinking']])",
"  Start an incremental GC and run a slice that processes about n objects.\n"
"  If 'shrinking' is passesd as the optional second argument, perform a\n"
"  shrinking GC rather than a normal GC."),

    JS_FN_HELP("gcslice", GCSlice, 1, 0,
"gcslice([n])",
"  Start or continue an an incremental GC, running a slice that processes about n objects."),

    JS_FN_HELP("abortgc", AbortGC, 1, 0,
"abortgc()",
"  Abort the current incremental GC."),

    JS_FN_HELP("validategc", ValidateGC, 1, 0,
"validategc(true|false)",
"  If true, a separate validation step is performed after an incremental GC."),

    JS_FN_HELP("fullcompartmentchecks", FullCompartmentChecks, 1, 0,
"fullcompartmentchecks(true|false)",
"  If true, check for compartment mismatches before every GC."),

    JS_FN_HELP("nondeterministicGetWeakMapKeys", NondeterministicGetWeakMapKeys, 1, 0,
"nondeterministicGetWeakMapKeys(weakmap)",
"  Return an array of the keys in the given WeakMap."),

    JS_FN_HELP("internalConst", InternalConst, 1, 0,
"internalConst(name)",
"  Query an internal constant for the engine. See InternalConst source for\n"
"  the list of constant names."),

    JS_FN_HELP("isProxy", IsProxy, 1, 0,
"isProxy(obj)",
"  If true, obj is a proxy of some sort"),

    JS_FN_HELP("dumpHeap", DumpHeap, 1, 0,
"dumpHeap(['collectNurseryBeforeDump'], [filename])",
"  Dump reachable and unreachable objects to the named file, or to stdout.  If\n"
"  'collectNurseryBeforeDump' is specified, a minor GC is performed first,\n"
"  otherwise objects in the nursery are ignored."),

    JS_FN_HELP("terminate", Terminate, 0, 0,
"terminate()",
"  Terminate JavaScript execution, as if we had run out of\n"
"  memory or been terminated by the slow script dialog."),

    JS_FN_HELP("enableSPSProfiling", EnableSPSProfiling, 0, 0,
"enableSPSProfiling()",
"  Enables SPS instrumentation and corresponding assertions, with slow\n"
"  assertions disabled.\n"),

    JS_FN_HELP("enableSPSProfilingWithSlowAssertions", EnableSPSProfilingWithSlowAssertions, 0, 0,
"enableSPSProfilingWithSlowAssertions()",
"  Enables SPS instrumentation and corresponding assertions, with slow\n"
"  assertions enabled.\n"),

    JS_FN_HELP("disableSPSProfiling", DisableSPSProfiling, 0, 0,
"disableSPSProfiling()",
"  Disables SPS instrumentation"),

    JS_FN_HELP("readSPSProfilingStack", ReadSPSProfilingStack, 0, 0,
"readSPSProfilingStack()",
"  Reads the jit stack using ProfilingFrameIterator."),

    JS_FN_HELP("enableOsiPointRegisterChecks", EnableOsiPointRegisterChecks, 0, 0,
"enableOsiPointRegisterChecks()",
"Emit extra code to verify live regs at the start of a VM call are not\n"
"modified before its OsiPoint."),

    JS_FN_HELP("displayName", DisplayName, 1, 0,
"displayName(fn)",
"  Gets the display name for a function, which can possibly be a guessed or\n"
"  inferred name based on where the function was defined. This can be\n"
"  different from the 'name' property on the function."),

    JS_FN_HELP("isAsmJSCompilationAvailable", IsAsmJSCompilationAvailable, 0, 0,
"isAsmJSCompilationAvailable",
"  Returns whether asm.js compilation is currently available or whether it is disabled\n"
"  (e.g., by the debugger)."),

    JS_FN_HELP("isSimdAvailable", IsSimdAvailable, 0, 0,
"isSimdAvailable",
"  Returns true if SIMD extensions are supported on this platform."),

    JS_FN_HELP("getJitCompilerOptions", GetJitCompilerOptions, 0, 0,
"getCompilerOptions()",
"Return an object describing some of the JIT compiler options.\n"),

    JS_FN_HELP("isAsmJSModule", IsAsmJSModule, 1, 0,
"isAsmJSModule(fn)",
"  Returns whether the given value is a function containing \"use asm\" that has been\n"
"  validated according to the asm.js spec."),

    JS_FN_HELP("isAsmJSModuleLoadedFromCache", IsAsmJSModuleLoadedFromCache, 1, 0,
"isAsmJSModuleLoadedFromCache(fn)",
"  Return whether the given asm.js module function has been loaded directly\n"
"  from the cache. This function throws an error if fn is not a validated asm.js\n"
"  module."),

    JS_FN_HELP("isAsmJSFunction", IsAsmJSFunction, 1, 0,
"isAsmJSFunction(fn)",
"  Returns whether the given value is a nested function in an asm.js module that has been\n"
"  both compile- and link-time validated."),

    JS_FN_HELP("isLazyFunction", IsLazyFunction, 1, 0,
"isLazyFunction(fun)",
"  True if fun is a lazy JSFunction."),

    JS_FN_HELP("isRelazifiableFunction", IsRelazifiableFunction, 1, 0,
"isRelazifiableFunction(fun)",
"  Ture if fun is a JSFunction with a relazifiable JSScript."),

    JS_FN_HELP("enableShellObjectMetadataCallback", EnableShellObjectMetadataCallback, 0, 0,
"enableShellObjectMetadataCallback()",
"  Use ShellObjectMetadataCallback to supply metadata for all newly created objects."),

    JS_FN_HELP("getObjectMetadata", GetObjectMetadata, 1, 0,
"getObjectMetadata(obj)",
"  Get the metadata for an object."),

    JS_INLINABLE_FN_HELP("bailout", testingFunc_bailout, 0, 0, TestBailout,
"bailout()",
"  Force a bailout out of ionmonkey (if running in ionmonkey)."),

    JS_FN_HELP("inJit", testingFunc_inJit, 0, 0,
"inJit()",
"  Returns true when called within (jit-)compiled code. When jit compilation is disabled this\n"
"  function returns an error string. This function returns false in all other cases.\n"
"  Depending on truthiness, you should continue to wait for compilation to happen or stop execution.\n"),

    JS_FN_HELP("inIon", testingFunc_inIon, 0, 0,
"inIon()",
"  Returns true when called within ion. When ion is disabled or when compilation is abnormally\n"
"  slow to start, this function returns an error string. Otherwise, this function returns false.\n"
"  This behaviour ensures that a falsy value means that we are not in ion, but expect a\n"
"  compilation to occur in the future. Conversely, a truthy value means that we are either in\n"
"  ion or that there is litle or no chance of ion ever compiling the current script."),

    JS_FN_HELP("assertJitStackInvariants", TestingFunc_assertJitStackInvariants, 0, 0,
"assertJitStackInvariants()",
"  Iterates the Jit stack and check that stack invariants hold."),

    JS_FN_HELP("setJitCompilerOption", SetJitCompilerOption, 2, 0,
"setCompilerOption(<option>, <number>)",
"  Set a compiler option indexed in JSCompileOption enum to a number.\n"),

    JS_FN_HELP("setIonCheckGraphCoherency", SetIonCheckGraphCoherency, 1, 0,
"setIonCheckGraphCoherency(bool)",
"  Set whether Ion should perform graph consistency (DEBUG-only) assertions. These assertions\n"
"  are valuable and should be generally enabled, however they can be very expensive for large\n"
"  (asm.js) programs."),

    JS_FN_HELP("serialize", Serialize, 1, 0,
"serialize(data, [transferables])",
"  Serialize 'data' using JS_WriteStructuredClone. Returns a structured\n"
"  clone buffer object."),

    JS_FN_HELP("deserialize", Deserialize, 1, 0,
"deserialize(clonebuffer)",
"  Deserialize data generated by serialize."),

    JS_FN_HELP("neuter", Neuter, 1, 0,
"neuter(buffer, \"change-data\"|\"same-data\")",
"  Neuter the given ArrayBuffer object as if it had been transferred to a\n"
"  WebWorker. \"change-data\" will update the internal data pointer.\n"
"  \"same-data\" will leave it set to its original value, to mimic eg\n"
"  asm.js ArrayBuffer neutering."),

    JS_FN_HELP("helperThreadCount", HelperThreadCount, 0, 0,
"helperThreadCount()",
"  Returns the number of helper threads available for off-main-thread tasks."),

#ifdef JS_TRACE_LOGGING
    JS_FN_HELP("startTraceLogger", EnableTraceLogger, 0, 0,
"startTraceLogger()",
"  Start logging the mainThread.\n"
"  Note: tracelogging starts automatically. Disable it by setting environment variable\n"
"  TLOPTIONS=disableMainThread"),

    JS_FN_HELP("stopTraceLogger", DisableTraceLogger, 0, 0,
"stopTraceLogger()",
"  Stop logging the mainThread."),
#endif

    JS_FN_HELP("reportOutOfMemory", ReportOutOfMemory, 0, 0,
"reportOutOfMemory()",
"  Report OOM, then clear the exception and return undefined. For crash testing."),

    JS_FN_HELP("reportLargeAllocationFailure", ReportLargeAllocationFailure, 0, 0,
"reportLargeAllocationFailure()",
"  Call the large allocation failure callback, as though a large malloc call failed,\n"
"  then return undefined. In Gecko, this sends a memory pressure notification, which\n"
"  can free up some memory."),

    JS_FN_HELP("findPath", FindPath, 2, 0,
"findPath(start, target)",
"  Return an array describing one of the shortest paths of GC heap edges from\n"
"  |start| to |target|, or |undefined| if |target| is unreachable from |start|.\n"
"  Each element of the array is either of the form:\n"
"    { node: <object or string>, edge: <string describing edge from node> }\n"
"  if the node is a JavaScript object or value; or of the form:\n"
"    { type: <string describing node>, edge: <string describing edge> }\n"
"  if the node is some internal thing that is not a proper JavaScript value\n"
"  (like a shape or a scope chain element). The destination of the i'th array\n"
"  element's edge is the node of the i+1'th array element; the destination of\n"
"  the last array element is implicitly |target|.\n"),

#ifdef DEBUG
    JS_FN_HELP("dumpObject", DumpObject, 1, 0,
"dumpObject()",
"  Dump an internal representation of an object."),
#endif

#ifdef NIGHTLY_BUILD
    JS_FN_HELP("objectAddress", ObjectAddress, 1, 0,
"objectAddress(obj)",
"  Return the current address of the object. For debugging only--this\n"
"  address may change during a moving GC."),

    JS_FN_HELP("sharedAddress", SharedAddress, 1, 0,
"sharedAddress(obj)",
"  Return the address of the shared storage of a SharedArrayBuffer."),
#endif

    JS_FN_HELP("evalReturningScope", EvalReturningScope, 1, 0,
"evalReturningScope(scriptStr, [global])",
"  Evaluate the script in a new scope and return the scope.\n"
"  If |global| is present, clone the script to |global| before executing."),

    JS_FN_HELP("cloneAndExecuteScript", ShellCloneAndExecuteScript, 2, 0,
"cloneAndExecuteScript(source, global)",
"  Compile |source| in the current compartment, clone it into |global|'s\n"
"  compartment, and run it there."),

    JS_FN_HELP("backtrace", DumpBacktrace, 1, 0,
"backtrace()",
"  Dump out a brief backtrace."),

    JS_FN_HELP("getBacktrace", GetBacktrace, 1, 0,
"getBacktrace([options])",
"  Return the current stack as a string. Takes an optional options object,\n"
"  which may contain any or all of the boolean properties\n"
"    options.args - show arguments to each function\n"
"    options.locals - show local variables in each frame\n"
"    options.thisprops - show the properties of the 'this' object of each frame\n"),

    JS_FN_HELP("byteSize", ByteSize, 1, 0,
"byteSize(value)",
"  Return the size in bytes occupied by |value|, or |undefined| if value\n"
"  is not allocated in memory.\n"),

    JS_FN_HELP("byteSizeOfScript", ByteSizeOfScript, 1, 0,
"byteSizeOfScript(f)",
"  Return the size in bytes occupied by the function |f|'s JSScript.\n"),

    JS_FN_HELP("immutablePrototypesEnabled", ImmutablePrototypesEnabled, 0, 0,
"immutablePrototypesEnabled()",
"  Returns true if immutable-prototype behavior (triggered by setImmutablePrototype)\n"
"  is enabled, such that modifying an immutable prototype will fail."),

    JS_FN_HELP("setImmutablePrototype", SetImmutablePrototype, 1, 0,
"setImmutablePrototype(obj)",
"  Try to make obj's [[Prototype]] immutable, such that subsequent attempts to\n"
"  change it will fail.  Return true if obj's [[Prototype]] was successfully made\n"
"  immutable (or if it already was immutable), false otherwise.  Throws in case\n"
"  of internal error, or if the operation doesn't even make sense (for example,\n"
"  because the object is a revoked proxy)."),

#ifdef DEBUG
    JS_FN_HELP("dumpStringRepresentation", DumpStringRepresentation, 1, 0,
"dumpStringRepresentation(str)",
"  Print a human-readable description of how the string |str| is represented.\n"),
#endif

    JS_FN_HELP("setLazyParsingDisabled", SetLazyParsingDisabled, 1, 0,
"setLazyParsingDisabled(bool)",
"  Explicitly disable lazy parsing in the current compartment.  The default is that lazy "
"  parsing is not explicitly disabled."),

    JS_FN_HELP("setDiscardSource", SetDiscardSource, 1, 0,
"setDiscardSource(bool)",
"  Explicitly enable source discarding in the current compartment.  The default is that "
"  source discarding is not explicitly enabled."),

    JS_FN_HELP("getConstructorName", GetConstructorName, 1, 0,
"getConstructorName(object)",
"  If the given object was created with `new Ctor`, return the constructor's display name. "
"  Otherwise, return null."),

    JS_FN_HELP("allocationMarker", AllocationMarker, 0, 0,
"allocationMarker([options])",
"  Return a freshly allocated object whose [[Class]] name is\n"
"  \"AllocationMarker\". Such objects are allocated only by calls\n"
"  to this function, never implicitly by the system, making them\n"
"  suitable for use in allocation tooling tests. Takes an optional\n"
"  options object which may contain the following properties:\n"
"    * nursery: bool, whether to allocate the object in the nursery\n"),

    JS_FN_HELP("setGCCallback", SetGCCallback, 1, 0,
"setGCCallback({action:\"...\", options...})",
"  Set the GC callback. action may be:\n"
"    'minorGC' - run a nursery collection\n"
"    'majorGC' - run a major collection, nesting up to a given 'depth'\n"),

    JS_FN_HELP("setARMHwCapFlags", SetARMHwCapFlags, 1, 0,
"setARMHwCapFlags(\"flag1,flag2 flag3\")",
"  On non-ARM, no-op. On ARM, set the hardware capabilities. The list of \n"
"  flags is available by calling this function with \"help\" as the flag's name"),

    JS_FN_HELP("getLcovInfo", GetLcovInfo, 1, 0,
"getLcovInfo(global)",
"  Generate LCOV tracefile for the given compartment.  If no global are provided then\n"
"  the current global is used as the default one.\n"),

#ifdef DEBUG
    JS_FN_HELP("setRNGState", SetRNGState, 2, 0,
"setRNGState(seed0, seed1)",
"  Set this compartment's RNG state.\n"),
#endif

    JS_FN_HELP("getModuleEnvironmentNames", GetModuleEnvironmentNames, 1, 0,
"getModuleEnvironmentNames(module)",
"  Get the list of a module environment's bound names for a specified module.\n"),

    JS_FN_HELP("getModuleEnvironmentValue", GetModuleEnvironmentValue, 2, 0,
"getModuleEnvironmentValue(module, name)",
"  Get the value of a bound name in a module environment.\n"),

    JS_FS_HELP_END
};

static const JSPropertySpec TestingProperties[] = {
    JS_PSG("timesAccessed", TimesAccessed, 0),
    JS_PS_END
};

bool
js::DefineTestingFunctions(JSContext* cx, HandleObject obj, bool fuzzingSafe_,
                           bool disableOOMFunctions_)
{
    fuzzingSafe = fuzzingSafe_;
    if (EnvVarIsDefined("MOZ_FUZZING_SAFE"))
        fuzzingSafe = true;

    disableOOMFunctions = disableOOMFunctions_;

    if (!JS_DefineProperties(cx, obj, TestingProperties))
        return false;

    return JS_DefineFunctionsWithHelp(cx, obj, TestingFunctions);
}
