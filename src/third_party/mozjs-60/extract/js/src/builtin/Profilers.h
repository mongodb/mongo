/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Functions for controlling profilers from within JS: Valgrind, Perf,
 * Shark, etc.
 */
#ifndef builtin_Profilers_h
#define builtin_Profilers_h

#include "jstypes.h"

#ifdef _MSC_VER
typedef int pid_t;
#else
#include <unistd.h>
#endif

/**
 * Start any profilers that are available and have been configured on for this
 * platform. This is NOT thread safe.
 *
 * The profileName is used by some profilers to describe the current profiling
 * run. It may be used for part of the filename of the output, but the
 * specifics depend on the profiler. Many profilers will ignore it. Passing in
 * nullptr is legal; some profilers may use it to output to stdout or similar.
 *
 * Returns true if no profilers fail to start.
 */
extern MOZ_MUST_USE JS_PUBLIC_API(bool)
JS_StartProfiling(const char* profileName, pid_t pid);

/**
 * Stop any profilers that were previously started with JS_StartProfiling.
 * Returns true if no profilers fail to stop.
 */
extern MOZ_MUST_USE JS_PUBLIC_API(bool)
JS_StopProfiling(const char* profileName);

/**
 * Write the current profile data to the given file, if applicable to whatever
 * profiler is being used.
 */
extern MOZ_MUST_USE JS_PUBLIC_API(bool)
JS_DumpProfile(const char* outfile, const char* profileName);

/**
 * Pause currently active profilers (only supported by some profilers). Returns
 * whether any profilers failed to pause. (Profilers that do not support
 * pause/resume do not count.)
 */
extern MOZ_MUST_USE JS_PUBLIC_API(bool)
JS_PauseProfilers(const char* profileName);

/**
 * Resume suspended profilers
 */
extern MOZ_MUST_USE JS_PUBLIC_API(bool)
JS_ResumeProfilers(const char* profileName);

/**
 * The profiling API calls are not able to report errors, so they use a
 * thread-unsafe global memory buffer to hold the last error encountered. This
 * should only be called after something returns false.
 */
JS_PUBLIC_API(const char*)
JS_UnsafeGetLastProfilingError();

#ifdef MOZ_CALLGRIND

extern MOZ_MUST_USE JS_FRIEND_API(bool)
js_StopCallgrind();

extern MOZ_MUST_USE JS_FRIEND_API(bool)
js_StartCallgrind();

extern MOZ_MUST_USE JS_FRIEND_API(bool)
js_DumpCallgrind(const char* outfile);

#endif /* MOZ_CALLGRIND */

#ifdef __linux__

extern MOZ_MUST_USE JS_FRIEND_API(bool)
js_StartPerf();

extern MOZ_MUST_USE JS_FRIEND_API(bool)
js_StopPerf();

#endif /* __linux__ */

#endif /* builtin_Profilers_h */
