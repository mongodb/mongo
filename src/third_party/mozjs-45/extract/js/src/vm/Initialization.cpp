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
#include "jit/ExecutableAllocator.h"
#include "jit/Ion.h"
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

using JS::detail::InitState;
using JS::detail::libraryInitState;
using js::FutexRuntime;

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

JS_PUBLIC_API(bool)
JS_Init(void)
{
    MOZ_ASSERT(libraryInitState == InitState::Uninitialized,
               "must call JS_Init once before any JSAPI operation except "
               "JS_SetICUMemoryFunctions");
    MOZ_ASSERT(!JSRuntime::hasLiveRuntimes(),
               "how do we have live runtimes before JS_Init?");

    PRMJ_NowInit();

#ifdef DEBUG
    CheckMessageParameterCounts();
#endif

    using js::TlsPerThreadData;
    if (!TlsPerThreadData.initialized() && !TlsPerThreadData.init())
        return false;

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    if (!js::oom::InitThreadType())
        return false;
    js::oom::SetThreadType(js::oom::THREAD_TYPE_MAIN);
#endif

    js::jit::ExecutableAllocator::initStatic();

    if (!js::jit::InitializeIon())
        return false;

    js::DateTimeInfo::init();

#if EXPOSE_INTL_API
    UErrorCode err = U_ZERO_ERROR;
    u_init(&err);
    if (U_FAILURE(err))
        return false;
#endif // EXPOSE_INTL_API

    if (!js::CreateHelperThreadsState())
        return false;

    if (!FutexRuntime::initialize())
        return false;

    libraryInitState = InitState::Running;
    return true;
}

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

    FutexRuntime::destroy();

    js::DestroyHelperThreadsState();

#ifdef JS_TRACE_LOGGING
    js::DestroyTraceLoggerThreadState();
    js::DestroyTraceLoggerGraphState();
#endif

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
