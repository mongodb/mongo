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

// Underlying opaque type.
namespace js::frontend {
struct InitialStencilAndDelazifications;
}  // namespace js::frontend

namespace JS {

using Stencil = js::frontend::InitialStencilAndDelazifications;

class JS_PUBLIC_API ReadOnlyCompileOptions;

using TranscodeBuffer = mozilla::Vector<uint8_t>;
using TranscodeRange = mozilla::Range<const uint8_t>;

struct TranscodeSource final {
  TranscodeSource(const TranscodeRange& range_, const char* file, uint32_t line)
      : range(range_), filename(file), lineno(line) {}

  const TranscodeRange range;
  const char* filename;
  const uint32_t lineno;
};

enum class TranscodeResult : uint8_t {
  // Successful encoding / decoding.
  Ok = 0,

  // A warning message, is set to the message out-param.
  Failure = 0x10,
  Failure_BadBuildId = Failure | 0x1,
  Failure_AsmJSNotSupported = Failure | 0x2,
  Failure_BadDecode = Failure | 0x3,

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

// Check if the compile options and script's flag matches.
//
// JS::DecodeScript* and JS::DecodeOffThreadScript internally check this.
extern JS_PUBLIC_API bool CheckCompileOptionsMatch(
    const ReadOnlyCompileOptions& options, JSScript* script);

}  // namespace JS

#endif /* js_Transcoding_h */
