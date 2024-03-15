/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WaitCallbacks_h
#define js_WaitCallbacks_h

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

struct JS_PUBLIC_API JSRuntime;

namespace JS {

/**
 * When the JSRuntime is about to block in an Atomics.wait() JS call or in a
 * `wait` instruction in WebAssembly, it can notify the host by means of a call
 * to BeforeWaitCallback.  After the wait, it can notify the host by means of a
 * call to AfterWaitCallback.  Both callbacks must be null, or neither.
 *
 * (If you change the callbacks from null to not-null or vice versa while some
 * thread on the runtime is in a wait, you will be sorry.)
 *
 * The argument to the BeforeWaitCallback is a pointer to uninitialized
 * stack-allocated working memory of size WAIT_CALLBACK_CLIENT_MAXMEM bytes.
 * The caller of SetWaitCallback() must pass the amount of memory it will need,
 * and this amount will be checked against that limit and the process will crash
 * reliably if the check fails.
 *
 * The value returned by the BeforeWaitCallback will be passed to the
 * AfterWaitCallback.
 *
 * The AfterWaitCallback will be called even if the wakeup is spurious and the
 * thread goes right back to waiting again.  Of course the thread will call the
 * BeforeWaitCallback once more before it goes to sleep in this situation.
 */

static constexpr size_t WAIT_CALLBACK_CLIENT_MAXMEM = 32;

using BeforeWaitCallback = void* (*)(uint8_t* memory);
using AfterWaitCallback = void (*)(void* cookie);

extern JS_PUBLIC_API void SetWaitCallback(JSRuntime* rt,
                                          BeforeWaitCallback beforeWait,
                                          AfterWaitCallback afterWait,
                                          size_t requiredMemory);

}  // namespace JS

#endif  // js_WaitCallbacks_h
