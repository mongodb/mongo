/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ScriptPrivate_h
#define js_ScriptPrivate_h

#include "jstypes.h"

#include "js/TypeDecls.h"

namespace JS {

/**
 * Set a private value associated with a script. Note that this value is shared
 * by all nested scripts compiled from a single source file.
 */
extern JS_PUBLIC_API void SetScriptPrivate(JSScript* script,
                                           const JS::Value& value);

/**
 * Get the private value associated with a script. Note that this value is
 * shared by all nested scripts compiled from a single source file.
 */
extern JS_PUBLIC_API JS::Value GetScriptPrivate(JSScript* script);

/**
 * Return the private value associated with currently executing script or
 * module, or undefined if there is no such script.
 */
extern JS_PUBLIC_API JS::Value GetScriptedCallerPrivate(JSContext* cx);

/**
 * Hooks called when references to a script private value are created or
 * destroyed. This allows use of a reference counted object as the
 * script private.
 */
using ScriptPrivateReferenceHook = void (*)(const JS::Value&);

/**
 * Set the script private finalize hook for the runtime to the given function.
 */
extern JS_PUBLIC_API void SetScriptPrivateReferenceHooks(
    JSRuntime* rt, ScriptPrivateReferenceHook addRefHook,
    ScriptPrivateReferenceHook releaseHook);

}  // namespace JS

#endif  // js_ScriptPrivate_h
