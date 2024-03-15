/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Types and functions related to the compilation of JavaScript off the
 * direct JSAPI-using thread.
 */

#ifndef js_OffThreadScriptCompilation_h
#define js_OffThreadScriptCompilation_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CompileOptions.h"  // JS::DecodeOptions, JS::ReadOnlyCompileOptions

struct JS_PUBLIC_API JSContext;

namespace JS {

class OffThreadToken;

using OffThreadCompileCallback = void (*)(OffThreadToken* token,
                                          void* callbackData);

/*
 * Off thread compilation control flow.
 *
 * (See also js/public/experimental/JSStencil.h)
 *
 * After successfully triggering an off thread compile of a script, the
 * callback will eventually be invoked with the specified data and a token
 * for the compilation. The callback will be invoked while off thread,
 * so must ensure that its operations are thread safe. Afterwards, one of the
 * following functions must be invoked on the runtime's main thread:
 *
 * - FinishCompileToStencilOffThread, to get the result stencil (or nullptr on
 *   failure).
 * - CancelCompileToStencilOffThread, to free the resources without creating a
 *   stencil.
 *
 * The characters passed in to CompileToStencilOffThread must remain live until
 * the callback is invoked.
 */

extern JS_PUBLIC_API bool CanCompileOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

extern JS_PUBLIC_API bool CanDecodeOffThread(JSContext* cx,
                                             const DecodeOptions& options,
                                             size_t length);

}  // namespace JS

#endif /* js_OffThreadScriptCompilation_h */
