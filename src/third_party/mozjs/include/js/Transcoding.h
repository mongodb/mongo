/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Structures and functions for transcoding compiled scripts and functions to
 * and from memory.
 */

#ifndef js_Transcoding_h
#define js_Transcoding_h

#include "mozilla/Range.h"   // mozilla::Range
#include "mozilla/Vector.h"  // mozilla::Vector

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include "js/TypeDecls.h"

namespace JS {

class ReadOnlyCompileOptions;

using TranscodeBuffer = mozilla::Vector<uint8_t>;
using TranscodeRange = mozilla::Range<const uint8_t>;

struct TranscodeSource final {
  TranscodeSource(const TranscodeRange& range_, const char* file, uint32_t line)
      : range(range_), filename(file), lineno(line) {}

  const TranscodeRange range;
  const char* filename;
  const uint32_t lineno;
};

using TranscodeSources = mozilla::Vector<TranscodeSource>;

enum class TranscodeResult : uint8_t {
  // Successful encoding / decoding.
  Ok = 0,

  // A warning message, is set to the message out-param.
  Failure = 0x10,
  Failure_BadBuildId = Failure | 0x1,
  Failure_RunOnceNotSupported = Failure | 0x2,
  Failure_AsmJSNotSupported = Failure | 0x3,
  Failure_BadDecode = Failure | 0x4,
  Failure_WrongCompileOption = Failure | 0x5,
  Failure_NotInterpretedFun = Failure | 0x6,

  // There is a pending exception on the context.
  Throw = 0x20
};

inline bool IsTranscodeFailureResult(const TranscodeResult result) {
  uint8_t raw_result = static_cast<uint8_t>(result);
  uint8_t raw_failure = static_cast<uint8_t>(TranscodeResult::Failure);
  TranscodeResult masked =
      static_cast<TranscodeResult>(raw_result & raw_failure);
  return masked == TranscodeResult::Failure;
}

static constexpr size_t BytecodeOffsetAlignment = 4;
static_assert(BytecodeOffsetAlignment <= alignof(std::max_align_t),
              "Alignment condition requires a custom allocator.");

// Align the bytecode offset for transcoding for the requirement.
inline size_t AlignTranscodingBytecodeOffset(size_t offset) {
  size_t extra = offset % BytecodeOffsetAlignment;
  if (extra == 0) {
    return offset;
  }
  size_t padding = BytecodeOffsetAlignment - extra;
  return offset + padding;
}

inline bool IsTranscodingBytecodeOffsetAligned(size_t offset) {
  return offset % BytecodeOffsetAlignment == 0;
}

inline bool IsTranscodingBytecodeAligned(const void* offset) {
  return IsTranscodingBytecodeOffsetAligned(size_t(offset));
}

// Encode JSScript into the buffer.
//
// If the `buffer` isn't empty, the start of the `buffer` should meet
// IsTranscodingBytecodeAligned, and the length should meet
// IsTranscodingBytecodeOffsetAligned.
//
// NOTE: As long as IsTranscodingBytecodeOffsetAligned is met, that means
//       there's JS::BytecodeOffsetAlignment+extra bytes in the buffer,
//       IsTranscodingBytecodeAligned should be guaranteed to meet by
//       malloc, used by MallocAllocPolicy in mozilla::Vector.
extern JS_PUBLIC_API TranscodeResult EncodeScript(JSContext* cx,
                                                  TranscodeBuffer& buffer,
                                                  Handle<JSScript*> script);

// Decode JSScript from the buffer.
//
// The start of `buffer` and `cursorIndex` should meet
// IsTranscodingBytecodeAligned and IsTranscodingBytecodeOffsetAligned.
// (This should be handled while encoding).
extern JS_PUBLIC_API TranscodeResult
DecodeScript(JSContext* cx, const ReadOnlyCompileOptions& options,
             TranscodeBuffer& buffer, MutableHandle<JSScript*> scriptp,
             size_t cursorIndex = 0);

// Decode JSScript from the range.
//
// The start of `range` should meet IsTranscodingBytecodeAligned and
// IsTranscodingBytecodeOffsetAligned.
// (This should be handled while encoding).
extern JS_PUBLIC_API TranscodeResult
DecodeScript(JSContext* cx, const ReadOnlyCompileOptions& options,
             const TranscodeRange& range, MutableHandle<JSScript*> scriptp);

// If js::UseOffThreadParseGlobal is true, decode JSScript from the buffer.
//
// If js::UseOffThreadParseGlobal is false, decode CompilationStencil from the
// buffer and instantiate JSScript from it.
//
// options.useOffThreadParseGlobal should match JS::SetUseOffThreadParseGlobal.
//
// The start of `buffer` and `cursorIndex` should meet
// IsTranscodingBytecodeAligned and IsTranscodingBytecodeOffsetAligned.
// (This should be handled while encoding).
extern JS_PUBLIC_API TranscodeResult DecodeScriptMaybeStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    TranscodeBuffer& buffer, MutableHandle<JSScript*> scriptp,
    size_t cursorIndex = 0);

// If js::UseOffThreadParseGlobal is true, decode JSScript from the buffer.
//
// If js::UseOffThreadParseGlobal is false, decode CompilationStencil from the
// buffer and instantiate JSScript from it.
//
// And then register an encoder on its script source, such that all functions
// can be encoded as they are parsed. This strategy is used to avoid blocking
// the main thread in a non-interruptible way.
//
// See also JS::FinishIncrementalEncoding.
//
// options.useOffThreadParseGlobal should match JS::SetUseOffThreadParseGlobal.
//
// The start of `buffer` and `cursorIndex` should meet
// IsTranscodingBytecodeAligned and IsTranscodingBytecodeOffsetAligned.
// (This should be handled while encoding).
extern JS_PUBLIC_API TranscodeResult DecodeScriptAndStartIncrementalEncoding(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    TranscodeBuffer& buffer, MutableHandle<JSScript*> scriptp,
    size_t cursorIndex = 0);

// Finish incremental encoding started by one of:
//   * JS::CompileAndStartIncrementalEncoding
//   * JS::FinishOffThreadScriptAndStartIncrementalEncoding
//   * JS::DecodeScriptAndStartIncrementalEncoding
//
// The |script| argument of |FinishIncrementalEncoding| should be the top-level
// script returned from one of the above.
//
// The |buffer| argument of |FinishIncrementalEncoding| is used for appending
// the encoded bytecode into the buffer. If any of these functions failed, the
// content of |buffer| would be undefined.
//
// If js::UseOffThreadParseGlobal is true, |buffer| contains encoded JSScript.
//
// If js::UseOffThreadParseGlobal is false, |buffer| contains encoded
// CompilationStencil.
//
// If the `buffer` isn't empty, the start of the `buffer` should meet
// IsTranscodingBytecodeAligned, and the length should meet
// IsTranscodingBytecodeOffsetAligned.
//
// NOTE: As long as IsTranscodingBytecodeOffsetAligned is met, that means
//       there's JS::BytecodeOffsetAlignment+extra bytes in the buffer,
//       IsTranscodingBytecodeAligned should be guaranteed to meet by
//       malloc, used by MallocAllocPolicy in mozilla::Vector.
extern JS_PUBLIC_API bool FinishIncrementalEncoding(JSContext* cx,
                                                    Handle<JSScript*> script,
                                                    TranscodeBuffer& buffer);

// Check if the compile options and script's flag matches.
//
// JS::DecodeScript* and JS::DecodeOffThreadScript internally check this.
//
// JS::DecodeMultiOffThreadScripts checks some options shared across multiple
// scripts. Caller is responsible for checking each script with this API when
// using the decoded script instead of compiling a new script wiht the given
// options.
extern JS_PUBLIC_API bool CheckCompileOptionsMatch(
    const ReadOnlyCompileOptions& options, JSScript* script);

}  // namespace JS

#endif /* js_Transcoding_h */
