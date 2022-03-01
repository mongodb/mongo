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

#include "mozilla/Range.h"   // mozilla::Range
#include "mozilla/Vector.h"  // mozilla::Vector

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/GCVector.h"  // JS::GCVector

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSScript;

namespace JS {

class JS_PUBLIC_API ReadOnlyCompileOptions;
struct TranscodeSource;

template <typename UnitT>
class SourceText;

}  // namespace JS

namespace mozilla {
union Utf8Unit;
}

namespace JS {

class OffThreadToken;

using OffThreadCompileCallback = void (*)(OffThreadToken* token,
                                          void* callbackData);

/*
 * Off thread compilation control flow.
 *
 * After successfully triggering an off thread compile of a script, the
 * callback will eventually be invoked with the specified data and a token
 * for the compilation. The callback will be invoked while off thread,
 * so must ensure that its operations are thread safe. Afterwards, one of the
 * following functions must be invoked on the runtime's main thread:
 *
 * - FinishOffThreadScript, to get the result script (or nullptr on failure).
 * - CancelOffThreadScript, to free the resources without creating a script.
 *
 * The characters passed in to CompileOffThread must remain live until the
 * callback is invoked, and the resulting script will be rooted until the call
 * to FinishOffThreadScript.
 */

extern JS_PUBLIC_API bool CanCompileOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

extern JS_PUBLIC_API OffThreadToken* CompileOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

// NOTE: Unlike for the normal sync compilation functions, this function NEVER
//       INFLATES TO UTF-16.  Therefore, it is ALWAYS invoking experimental
//       UTF-8 support.  Inflate to UTF-16 yourself and use the other overload
//       if you're unable to take a risk using unstable functionality.
extern JS_PUBLIC_API OffThreadToken* CompileOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

// Finish the off-thread parse/decode task and return the script. Return the
// script on success, or return null on failure (usually with an error reported)
extern JS_PUBLIC_API JSScript* FinishOffThreadScript(JSContext* cx,
                                                     OffThreadToken* token);

// Finish the off-thread parse/decode task and return the script, and register
// an encoder on its script source, such that all functions can be encoded as
// they are parsed. This strategy is used to avoid blocking the main thread in
// a non-interruptible way.
//
// See also JS::FinishIncrementalEncoding.
//
// Return the script on success, or return null on failure (usually with an
// error reported)
extern JS_PUBLIC_API JSScript* FinishOffThreadScriptAndStartIncrementalEncoding(
    JSContext* cx, OffThreadToken* token);

extern JS_PUBLIC_API void CancelOffThreadScript(JSContext* cx,
                                                OffThreadToken* token);

extern JS_PUBLIC_API void CancelOffThreadCompileToStencil(
    JSContext* cx, OffThreadToken* token);

extern JS_PUBLIC_API OffThreadToken* CompileOffThreadModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

// NOTE: Unlike for the normal sync compilation functions, this function NEVER
//       INFLATES TO UTF-16.  Therefore, it is ALWAYS invoking experimental
//       UTF-8 support.  Inflate to UTF-16 yourself and use the other overload
//       if you're unable to take a risk using unstable functionality.
extern JS_PUBLIC_API OffThreadToken* CompileOffThreadModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

extern JS_PUBLIC_API JSObject* FinishOffThreadModule(JSContext* cx,
                                                     OffThreadToken* token);

extern JS_PUBLIC_API void CancelOffThreadModule(JSContext* cx,
                                                OffThreadToken* token);

extern JS_PUBLIC_API bool CanDecodeOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

// If options.useOffThreadParseGlobal is true,
// decode JSScript from the buffer.
//
// If options.useOffThreadParseGlobal is false,
// decode stencil from the buffer and instantiate JSScript from it.
//
// The start of `buffer` and `cursor` should meet
// IsTranscodingBytecodeAligned and IsTranscodingBytecodeOffsetAligned.
// (This should be handled while encoding).
//
// `buffer` should be alive until the end of `FinishOffThreadScriptDecoder`.
extern JS_PUBLIC_API OffThreadToken* DecodeOffThreadScript(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    mozilla::Vector<uint8_t>& buffer /* TranscodeBuffer& */, size_t cursor,
    OffThreadCompileCallback callback, void* callbackData);

// The start of `range` should be meet IsTranscodingBytecodeAligned and
// AlignTranscodingBytecodeOffset.
// (This should be handled while encoding).
//
// `range` should be alive until the end of `FinishOffThreadScriptDecoder`.
extern JS_PUBLIC_API OffThreadToken* DecodeOffThreadScript(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    const mozilla::Range<uint8_t>& range /* TranscodeRange& */,
    OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API JSScript* FinishOffThreadScriptDecoder(
    JSContext* cx, OffThreadToken* token);

extern JS_PUBLIC_API void CancelOffThreadScriptDecoder(JSContext* cx,
                                                       OffThreadToken* token);

// Decode multiple JSScript from the sources.
extern JS_PUBLIC_API OffThreadToken* DecodeMultiOffThreadScripts(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    mozilla::Vector<TranscodeSource>& sources,
    OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API bool FinishMultiOffThreadScriptsDecoder(
    JSContext* cx, OffThreadToken* token,
    MutableHandle<GCVector<JSScript*>> scripts);

extern JS_PUBLIC_API void CancelMultiOffThreadScriptsDecoder(
    JSContext* cx, OffThreadToken* token);

// Tell off-thread compilation to/not to use the parse global.
// The default value is true.
//
// If set to false, off-thread compilation will compile to stencil, and
// instantiate the stencil on main-thread.
extern JS_PUBLIC_API void SetUseOffThreadParseGlobal(bool value);

}  // namespace JS

namespace js {
// Returns the value set by JS::SetUseOffThreadParseGlobal.
// The default value is true.
//
// This value is consumed internally, and public API consumer shouldn't
// directly use this value.
extern bool UseOffThreadParseGlobal();
}  // namespace js

#endif /* js_OffThreadScriptCompilation_h */
