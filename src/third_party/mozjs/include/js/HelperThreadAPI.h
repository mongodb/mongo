/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * API for supplying an external thread pool to run internal work off the main
 * thread.
 */

#ifndef js_HelperThreadAPI_h
#define js_HelperThreadAPI_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

namespace JS {

// Argument passed to the task callback to indicate whether we're invoking it
// because a new task was added by the JS engine or because we're on a helper
// thread that just finished a task and there are other tasks pending.
enum class DispatchReason { NewTask, FinishedTask };

/**
 * Set callback to dispatch a tasks to an external thread pool.
 *
 * When the task runs it should call JS::RunHelperThreadTask.
 */
using HelperThreadTaskCallback = void (*)(DispatchReason reason);
extern JS_PUBLIC_API void SetHelperThreadTaskCallback(
    HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize);

// Function to call from external thread pool to run a helper thread task.
extern JS_PUBLIC_API void RunHelperThreadTask();

}  // namespace JS

#endif  // js_HelperThreadAPI_h
