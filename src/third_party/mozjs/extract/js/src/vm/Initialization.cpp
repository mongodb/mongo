/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* SpiderMonkey initialization and shutdown code. */

#include "js/Initialization.h"

#include "mozilla/Assertions.h"

#include <ctype.h>

#include "jstypes.h"

#include "builtin/AtomicsObject.h"
#include "ds/MemoryProtectionExceptionHandler.h"
#include "gc/Statistics.h"
#include "jit/ExecutableAllocator.h"
#include "jit/Ion.h"
#include "jit/JitCommon.h"
#include "js/Utility.h"
#if ENABLE_INTL_API
#include "unicode/uclean.h"
#include "unicode/utypes.h"
#endif // ENABLE_INTL_API
#include "vm/DateTime.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"
#include "vm/Time.h"
#include "vm/TraceLogging.h"
#include "vtune/VTuneWrapper.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmInstance.h"

using JS::detail::InitState;
using JS::detail::libraryInitState;
using js::FutexThread;

InitState JS::detail::libraryInitState;

#ifdef DEBUG
static unsigned
MessageParameterCount(const char* format)
{
    unsigned numfmtspecs = 0;
    for (const char* fmt = format; *fmt != '\0'; fmt++) {
        if (*fmt == '{' && isdigit(fmt[1]))
            ++numfmtspecs;
    }
    return numfmtspecs;
}

static void
CheckMessageParameterCounts()
{
    // Assert that each message format has the correct number of braced
    // parameters.
# define MSG_DEF(name, count, exception, format)           \
        MOZ_ASSERT(MessageParameterCount(format) == count);
# include "js.msg"
# undef MSG_DEF
}
#endif /* DEBUG */

#define RETURN_IF_FAIL(code) do { if (!code) return #code " failed"; } while (0)

JS_PUBLIC_API(const char*)
JS::detail::InitWithFailureDiagnostic(bool isDebugBuild)
{
    // Verify that our DEBUG setting matches the caller's.
#ifdef DEBUG
    MOZ_RELEASE_ASSERT(isDebugBuild);
#else
    MOZ_RELEASE_ASSERT(!isDebugBuild);
#endif

    MOZ_ASSERT(libraryInitState == InitState::Uninitialized,
               "must call JS_Init once before any JSAPI operation except "
               "JS_SetICUMemoryFunctions");
    MOZ_ASSERT(!JSRuntime::hasLiveRuntimes(),
               "how do we have live runtimes before JS_Init?");

    libraryInitState = InitState::Initializing;

    PRMJ_NowInit();

    // The first invocation of `ProcessCreation` creates a temporary thread
    // and crashes if that fails, i.e. because we're out of memory. To prevent
    // that from happening at some later time, get it out of the way during
    // startup.
    mozilla::TimeStamp::ProcessCreation();

#ifdef DEBUG
    CheckMessageParameterCounts();
#endif

    RETURN_IF_FAIL(js::TlsContext.init());

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    RETURN_IF_FAIL(js::oom::InitThreadType());
#endif

    js::InitMallocAllocator();

    RETURN_IF_FAIL(js::Mutex::Init());

    RETURN_IF_FAIL(js::wasm::InitInstanceStaticData());

    js::gc::InitMemorySubsystem(); // Ensure gc::SystemPageSize() works.

    RETURN_IF_FAIL(js::jit::InitProcessExecutableMemory());

    RETURN_IF_FAIL(js::MemoryProtectionExceptionHandler::install());

    RETURN_IF_FAIL(js::jit::InitializeIon());

    RETURN_IF_FAIL(js::InitDateTimeState());

#ifdef MOZ_VTUNE
    RETURN_IF_FAIL(js::vtune::Initialize());
#endif

#if EXPOSE_INTL_API
    UErrorCode err = U_ZERO_ERROR;
    u_init(&err);
    if (U_FAILURE(err))
        return "u_init() failed";
#endif // EXPOSE_INTL_API

    RETURN_IF_FAIL(js::CreateHelperThreadsState());
    RETURN_IF_FAIL(FutexThread::initialize());
    RETURN_IF_FAIL(js::gcstats::Statistics::initialize());

#ifdef JS_SIMULATOR
    RETURN_IF_FAIL(js::jit::SimulatorProcess::initialize());
#endif

    libraryInitState = InitState::Running;
    return nullptr;
}

#undef RETURN_IF_FAIL

JS_PUBLIC_API(void)
JS_ShutDown(void)
{
    MOZ_ASSERT(libraryInitState == InitState::Running,
               "JS_ShutDown must only be called after JS_Init and can't race with it");
#ifdef DEBUG
    if (JSRuntime::hasLiveRuntimes()) {
        // Gecko is too buggy to assert this just yet.
        fprintf(stderr,
                "WARNING: YOU ARE LEAKING THE WORLD (at least one JSRuntime "
                "and everything alive inside it, that is) AT JS_ShutDown "
                "TIME.  FIX THIS!\n");
    }
#endif

    FutexThread::destroy();

    js::DestroyHelperThreadsState();

#ifdef JS_SIMULATOR
    js::jit::SimulatorProcess::destroy();
#endif

#ifdef JS_TRACE_LOGGING
    js::DestroyTraceLoggerThreadState();
    js::DestroyTraceLoggerGraphState();
#endif

    js::MemoryProtectionExceptionHandler::uninstall();

    js::wasm::ShutDownInstanceStaticData();
    js::wasm::ShutDownProcessStaticData();

    js::Mutex::ShutDown();

    // The only difficult-to-address reason for the restriction that you can't
    // call JS_Init/stuff/JS_ShutDown multiple times is the Windows PRMJ
    // NowInit initialization code, which uses PR_CallOnce to initialize the
    // PRMJ_Now subsystem.  (For reinitialization to be permitted, we'd need to
    // "reset" the called-once status -- doable, but more trouble than it's
    // worth now.)  Initializing that subsystem from JS_Init eliminates the
    // problem, but initialization can take a comparatively long time (15ms or
    // so), so we really don't want to do it in JS_Init, and we really do want
    // to do it only when PRMJ_Now is eventually called.
    PRMJ_NowShutdown();

#if EXPOSE_INTL_API
    u_cleanup();
#endif // EXPOSE_INTL_API

#ifdef MOZ_VTUNE
    js::vtune::Shutdown();
#endif // MOZ_VTUNE

    js::FinishDateTimeState();

    if (!JSRuntime::hasLiveRuntimes()) {
        js::wasm::ReleaseBuiltinThunks();
        js::jit::ReleaseProcessExecutableMemory();
        MOZ_ASSERT(!js::LiveMappedBufferCount());
    }

    js::ShutDownMallocAllocator();

    libraryInitState = InitState::ShutDown;
}

JS_PUBLIC_API(bool)
JS_SetICUMemoryFunctions(JS_ICUAllocFn allocFn, JS_ICUReallocFn reallocFn, JS_ICUFreeFn freeFn)
{
    MOZ_ASSERT(libraryInitState == InitState::Uninitialized,
               "must call JS_SetICUMemoryFunctions before any other JSAPI "
               "operation (including JS_Init)");

#if EXPOSE_INTL_API
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(/* context = */ nullptr, allocFn, reallocFn, freeFn, &status);
    return U_SUCCESS(status);
#else
    return true;
#endif
}
