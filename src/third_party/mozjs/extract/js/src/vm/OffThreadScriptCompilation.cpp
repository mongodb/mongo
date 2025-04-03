/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/OffThreadScriptCompilation.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Range.h"       // mozilla::Range
#include "mozilla/Utf8.h"        // mozilla::Utf8Unit
#include "mozilla/Vector.h"      // mozilla::Vector

#include <stddef.h>  // size_t

#include "jspubtd.h"  // js::CurrentThreadCanAccessRuntime
#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/experimental/JSStencil.h"  // JS::CompileToStencilOffThread, JS::FinishCompileToStencilOffThread
#include "js/SourceText.h"         // JS::SourceText
#include "vm/HelperThreadState.h"  // js::StartOffThreadParseScript
#include "vm/JSContext.h"          // JSContext
#include "vm/Runtime.h"            // js::CanUseExtraThreads

using namespace js;

using mozilla::Utf8Unit;

using JS::ReadOnlyCompileOptions;

enum class OffThread { Compile, Decode };

template <typename OptionT>
static bool CanDoOffThread(JSContext* cx, const OptionT& options,
                           size_t length) {
  static const size_t TINY_LENGTH = 5 * 1000;

  // These are heuristics which the caller may choose to ignore (e.g., for
  // testing purposes).
  if (!options.forceAsync) {
    // Compiling off the main thread inolves significant overheads.
    // Don't bother if the script is tiny.
    if (length < TINY_LENGTH) {
      return false;
    }
  }

  return cx->runtime()->canUseParallelParsing() && CanUseExtraThreads();
}

JS_PUBLIC_API bool JS::CanCompileOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options, size_t length) {
  return CanDoOffThread(cx, options, length);
}

JS_PUBLIC_API JS::OffThreadToken* JS::CompileToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData) {
  MOZ_ASSERT(CanCompileOffThread(cx, options, srcBuf.length()));
  return StartOffThreadCompileToStencil(cx, options, srcBuf, callback,
                                        callbackData);
}

JS_PUBLIC_API JS::OffThreadToken* JS::CompileToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Utf8Unit>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData) {
  MOZ_ASSERT(CanCompileOffThread(cx, options, srcBuf.length()));
  return StartOffThreadCompileToStencil(cx, options, srcBuf, callback,
                                        callbackData);
}

JS_PUBLIC_API JS::OffThreadToken* JS::CompileModuleToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData) {
  MOZ_ASSERT(CanCompileOffThread(cx, options, srcBuf.length()));
  return StartOffThreadCompileModuleToStencil(cx, options, srcBuf, callback,
                                              callbackData);
}

JS_PUBLIC_API JS::OffThreadToken* JS::CompileModuleToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Utf8Unit>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData) {
  MOZ_ASSERT(CanCompileOffThread(cx, options, srcBuf.length()));
  return StartOffThreadCompileModuleToStencil(cx, options, srcBuf, callback,
                                              callbackData);
}

JS_PUBLIC_API JS::OffThreadToken* JS::DecodeStencilOffThread(
    JSContext* cx, const DecodeOptions& options, const TranscodeBuffer& buffer,
    size_t cursor, OffThreadCompileCallback callback, void* callbackData) {
  JS::TranscodeRange range(buffer.begin() + cursor, buffer.length() - cursor);
  MOZ_ASSERT(CanDecodeOffThread(cx, options, range.length()));
  return StartOffThreadDecodeStencil(cx, options, range, callback,
                                     callbackData);
}

JS_PUBLIC_API JS::OffThreadToken* JS::DecodeStencilOffThread(
    JSContext* cx, const DecodeOptions& options, const TranscodeRange& range,
    OffThreadCompileCallback callback, void* callbackData) {
  MOZ_ASSERT(CanDecodeOffThread(cx, options, range.length()));
  return StartOffThreadDecodeStencil(cx, options, range, callback,
                                     callbackData);
}

JS_PUBLIC_API already_AddRefed<JS::Stencil> JS::FinishOffThreadStencil(
    JSContext* cx, JS::OffThreadToken* token,
    JS::InstantiationStorage* storage /* = nullptr */) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  RefPtr<JS::Stencil> stencil =
      HelperThreadState().finishStencilTask(cx, token, storage);
  return stencil.forget();
}

JS_PUBLIC_API void JS::CancelOffThreadToken(JSContext* cx,
                                            JS::OffThreadToken* token) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  HelperThreadState().cancelParseTask(cx->runtime(), token);
}

JS_PUBLIC_API bool JS::CanDecodeOffThread(JSContext* cx,
                                          const DecodeOptions& options,
                                          size_t length) {
  return CanDoOffThread(cx, options, length);
}

JS_PUBLIC_API JS::OffThreadToken* JS::DecodeMultiStencilsOffThread(
    JSContext* cx, const DecodeOptions& options, TranscodeSources& sources,
    OffThreadCompileCallback callback, void* callbackData) {
#ifdef DEBUG
  size_t length = 0;
  for (auto& source : sources) {
    length += source.range.length();
  }
  MOZ_ASSERT(CanDecodeOffThread(cx, options, length));
#endif
  return StartOffThreadDecodeMultiStencils(cx, options, sources, callback,
                                           callbackData);
}

JS_PUBLIC_API bool JS::FinishDecodeMultiStencilsOffThread(
    JSContext* cx, JS::OffThreadToken* token,
    mozilla::Vector<RefPtr<JS::Stencil>>* stencils) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  return HelperThreadState().finishMultiStencilsDecodeTask(cx, token, stencils);
}
