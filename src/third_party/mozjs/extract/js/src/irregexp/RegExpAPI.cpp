/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "irregexp/RegExpAPI.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"

#include "gc/Zone.h"
#include "irregexp/imported/regexp-ast.h"
#include "irregexp/imported/regexp-bytecode-generator.h"
#include "irregexp/imported/regexp-compiler.h"
#include "irregexp/imported/regexp-interpreter.h"
#include "irregexp/imported/regexp-macro-assembler-arch.h"
#include "irregexp/imported/regexp-macro-assembler-tracer.h"
#include "irregexp/imported/regexp-parser.h"
#include "irregexp/imported/regexp-stack.h"
#include "irregexp/imported/regexp.h"
#include "irregexp/RegExpNativeMacroAssembler.h"
#include "irregexp/RegExpShim.h"
#include "jit/JitCommon.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/friend/StackLimits.h"    // js::ReportOverRecursed
#include "util/StringBuffer.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpShared.h"

namespace js {
namespace irregexp {

using mozilla::AssertedCast;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::PointerRangeSize;
using mozilla::Some;

using frontend::DummyTokenStream;
using frontend::TokenStreamAnyChars;

using v8::internal::DisallowGarbageCollection;
using v8::internal::FlatStringReader;
using v8::internal::HandleScope;
using v8::internal::InputOutputData;
using v8::internal::IrregexpInterpreter;
using v8::internal::NativeRegExpMacroAssembler;
using v8::internal::RegExpBytecodeGenerator;
using v8::internal::RegExpCompileData;
using v8::internal::RegExpCompiler;
using v8::internal::RegExpError;
using v8::internal::RegExpMacroAssembler;
using v8::internal::RegExpMacroAssemblerTracer;
using v8::internal::RegExpNode;
using v8::internal::RegExpParser;
using v8::internal::SMRegExpMacroAssembler;
using v8::internal::Zone;

using V8HandleString = v8::internal::Handle<v8::internal::String>;
using V8HandleRegExp = v8::internal::Handle<v8::internal::JSRegExp>;

using namespace v8::internal::regexp_compiler_constants;

static uint32_t ErrorNumber(RegExpError err) {
  switch (err) {
    case RegExpError::kNone:
      return JSMSG_NOT_AN_ERROR;
    case RegExpError::kStackOverflow:
      return JSMSG_OVER_RECURSED;
    case RegExpError::kAnalysisStackOverflow:
      return JSMSG_OVER_RECURSED;
    case RegExpError::kTooLarge:
      return JSMSG_TOO_MANY_PARENS;
    case RegExpError::kUnterminatedGroup:
      return JSMSG_MISSING_PAREN;
    case RegExpError::kUnmatchedParen:
      return JSMSG_UNMATCHED_RIGHT_PAREN;
    case RegExpError::kEscapeAtEndOfPattern:
      return JSMSG_ESCAPE_AT_END_OF_REGEXP;
    case RegExpError::kInvalidPropertyName:
      return JSMSG_INVALID_PROPERTY_NAME;
    case RegExpError::kInvalidEscape:
      return JSMSG_INVALID_IDENTITY_ESCAPE;
    case RegExpError::kInvalidDecimalEscape:
      return JSMSG_INVALID_DECIMAL_ESCAPE;
    case RegExpError::kInvalidUnicodeEscape:
      return JSMSG_INVALID_UNICODE_ESCAPE;
    case RegExpError::kNothingToRepeat:
      return JSMSG_NOTHING_TO_REPEAT;
    case RegExpError::kLoneQuantifierBrackets:
      // Note: V8 reports the same error for both ']' and '}'.
      return JSMSG_RAW_BRACKET_IN_REGEXP;
    case RegExpError::kRangeOutOfOrder:
      return JSMSG_NUMBERS_OUT_OF_ORDER;
    case RegExpError::kIncompleteQuantifier:
      return JSMSG_INCOMPLETE_QUANTIFIER;
    case RegExpError::kInvalidQuantifier:
      return JSMSG_INVALID_QUANTIFIER;
    case RegExpError::kInvalidGroup:
      return JSMSG_INVALID_GROUP;
    case RegExpError::kMultipleFlagDashes:
    case RegExpError::kRepeatedFlag:
    case RegExpError::kInvalidFlagGroup:
      // V8 contains experimental support for turning regexp flags on
      // and off in the middle of a regular expression. Unless it
      // becomes standardized, SM does not support this feature.
      MOZ_CRASH("Mode modifiers not supported");
    case RegExpError::kNotLinear:
      // V8 has an experimental non-backtracking engine. We do not
      // support it yet.
      MOZ_CRASH("Non-backtracking execution not supported");
    case RegExpError::kTooManyCaptures:
      return JSMSG_TOO_MANY_PARENS;
    case RegExpError::kInvalidCaptureGroupName:
      return JSMSG_INVALID_CAPTURE_NAME;
    case RegExpError::kDuplicateCaptureGroupName:
      return JSMSG_DUPLICATE_CAPTURE_NAME;
    case RegExpError::kInvalidNamedReference:
      return JSMSG_INVALID_NAMED_REF;
    case RegExpError::kInvalidNamedCaptureReference:
      return JSMSG_INVALID_NAMED_CAPTURE_REF;
    case RegExpError::kInvalidClassEscape:
      return JSMSG_RANGE_WITH_CLASS_ESCAPE;
    case RegExpError::kInvalidClassPropertyName:
      return JSMSG_INVALID_CLASS_PROPERTY_NAME;
    case RegExpError::kInvalidCharacterClass:
      return JSMSG_RANGE_WITH_CLASS_ESCAPE;
    case RegExpError::kUnterminatedCharacterClass:
      return JSMSG_UNTERM_CLASS;
    case RegExpError::kOutOfOrderCharacterClass:
      return JSMSG_BAD_CLASS_RANGE;
    case RegExpError::NumErrors:
      MOZ_CRASH("Unreachable");
  }
  MOZ_CRASH("Unreachable");
}

Isolate* CreateIsolate(JSContext* cx) {
  auto isolate = MakeUnique<Isolate>(cx);
  if (!isolate || !isolate->init()) {
    return nullptr;
  }
  return isolate.release();
}

void DestroyIsolate(Isolate* isolate) {
  MOZ_ASSERT(isolate->liveHandles() == 0);
  MOZ_ASSERT(isolate->livePseudoHandles() == 0);
  js_delete(isolate);
}

size_t IsolateSizeOfIncludingThis(Isolate* isolate,
                                  mozilla::MallocSizeOf mallocSizeOf) {
  return isolate->sizeOfIncludingThis(mallocSizeOf);
}

static size_t ComputeColumn(const Latin1Char* begin, const Latin1Char* end) {
  return PointerRangeSize(begin, end);
}

static size_t ComputeColumn(const char16_t* begin, const char16_t* end) {
  return unicode::CountCodePoints(begin, end);
}

// This function is varargs purely so it can call ReportCompileErrorLatin1.
// We never call it with additional arguments.
template <typename CharT>
static void ReportSyntaxError(TokenStreamAnyChars& ts,
                              mozilla::Maybe<uint32_t> line,
                              mozilla::Maybe<uint32_t> column,
                              RegExpCompileData& result, CharT* start,
                              size_t length, ...) {
  MOZ_ASSERT(line.isSome() == column.isSome());

  gc::AutoSuppressGC suppressGC(ts.context());
  uint32_t errorNumber = ErrorNumber(result.error);

  if (errorNumber == JSMSG_OVER_RECURSED) {
    ReportOverRecursed(ts.context());
    return;
  }

  uint32_t offset = std::max(result.error_pos, 0);
  MOZ_ASSERT(offset <= length);

  ErrorMetadata err;

  // Ordinarily this indicates whether line-of-context information can be
  // added, but we entirely ignore that here because we create a
  // a line of context based on the expression source.
  uint32_t location = ts.currentToken().pos.begin;
  if (ts.fillExceptingContext(&err, location)) {
    uint32_t columnNumber =
        AssertedCast<uint32_t>(ComputeColumn(start, start + offset));
    if (line.isSome()) {
      // If this pattern is being checked by the frontend Parser instead
      // of other API entry points like |new RegExp|, then the parser will
      // have provided both a line and column pointing at the *beginning*
      // of the RegExp literal inside the source text.
      // We adjust the columnNumber to point to the actual syntax error
      // inside the literal.
      err.lineNumber = *line;
      err.columnNumber = *column + columnNumber;
    } else {
      // Line breaks are not significant in pattern text in the same way as
      // in source text, so act as though pattern text is a single line, then
      // compute a column based on "code point" count (treating a lone
      // surrogate as a "code point" in UTF-16).  Gak.
      err.lineNumber = 1;
      err.columnNumber = columnNumber;
    }
  }

  // For most error reporting, the line of context derives from the token
  // stream.  So when location information doesn't come from the token
  // stream, we can't give a line of context.  But here the "line of context"
  // can be (and is) derived from the pattern text, so we can provide it no
  // matter if the location is derived from the caller.

  const CharT* windowStart =
      (offset > ErrorMetadata::lineOfContextRadius)
          ? start + (offset - ErrorMetadata::lineOfContextRadius)
          : start;

  const CharT* windowEnd =
      (length - offset > ErrorMetadata::lineOfContextRadius)
          ? start + offset + ErrorMetadata::lineOfContextRadius
          : start + length;

  size_t windowLength = PointerRangeSize(windowStart, windowEnd);
  MOZ_ASSERT(windowLength <= ErrorMetadata::lineOfContextRadius * 2);

  // Create the windowed string, not including the potential line
  // terminator.
  StringBuffer windowBuf(ts.context());
  if (!windowBuf.append(windowStart, windowEnd)) {
    return;
  }

  // The line of context must be null-terminated, and StringBuffer doesn't
  // make that happen unless we force it to.
  if (!windowBuf.append('\0')) {
    return;
  }

  err.lineOfContext.reset(windowBuf.stealChars());
  if (!err.lineOfContext) {
    return;
  }

  err.lineLength = windowLength;
  err.tokenOffset = offset - (windowStart - start);

  va_list args;
  va_start(args, length);
  ReportCompileErrorLatin1(ts.context(), std::move(err), nullptr, errorNumber,
                           &args);
  va_end(args);
}

static void ReportSyntaxError(TokenStreamAnyChars& ts,
                              RegExpCompileData& result, HandleAtom pattern) {
  JS::AutoCheckCannotGC nogc_;
  if (pattern->hasLatin1Chars()) {
    ReportSyntaxError(ts, Nothing(), Nothing(), result,
                      pattern->latin1Chars(nogc_), pattern->length());
  } else {
    ReportSyntaxError(ts, Nothing(), Nothing(), result,
                      pattern->twoByteChars(nogc_), pattern->length());
  }
}

static bool CheckPatternSyntaxImpl(JSContext* cx, FlatStringReader* pattern,
                                   JS::RegExpFlags flags,
                                   RegExpCompileData* result) {
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  Zone zone(allocScope.alloc());

  HandleScope handleScope(cx->isolate);
  DisallowGarbageCollection no_gc;
  return RegExpParser::VerifyRegExpSyntax(cx->isolate, &zone, pattern, flags,
                                          result, no_gc);
}

bool CheckPatternSyntax(JSContext* cx, TokenStreamAnyChars& ts,
                        const mozilla::Range<const char16_t> chars,
                        JS::RegExpFlags flags, mozilla::Maybe<uint32_t> line,
                        mozilla::Maybe<uint32_t> column) {
  FlatStringReader reader(chars);
  RegExpCompileData result;
  if (!CheckPatternSyntaxImpl(cx, &reader, flags, &result)) {
    ReportSyntaxError(ts, line, column, result, chars.begin().get(),
                      chars.length());
    return false;
  }
  return true;
}

bool CheckPatternSyntax(JSContext* cx, TokenStreamAnyChars& ts,
                        HandleAtom pattern, JS::RegExpFlags flags) {
  FlatStringReader reader(cx, pattern);
  RegExpCompileData result;
  if (!CheckPatternSyntaxImpl(cx, &reader, flags, &result)) {
    ReportSyntaxError(ts, result, pattern);
    return false;
  }
  return true;
}

// A regexp is a good candidate for Boyer-Moore if it has at least 3
// times as many characters as it has unique characters. Note that
// table lookups in irregexp are done modulo tableSize (128).
template <typename CharT>
static bool HasFewDifferentCharacters(const CharT* chars, size_t length) {
  const uint32_t tableSize =
      v8::internal::NativeRegExpMacroAssembler::kTableSize;
  bool character_found[tableSize];
  uint32_t different = 0;
  memset(&character_found[0], 0, sizeof(character_found));
  for (uint32_t i = 0; i < length; i++) {
    uint32_t ch = chars[i] % tableSize;
    if (!character_found[ch]) {
      character_found[ch] = true;
      different++;
      // We declare a regexp low-alphabet if it has at least 3 times as many
      // characters as it has different characters.
      if (different * 3 > length) {
        return false;
      }
    }
  }
  return true;
}

// Identifies the sort of pattern where Boyer-Moore is faster than string search
static bool UseBoyerMoore(HandleAtom pattern, JS::AutoAssertNoGC& nogc) {
  size_t length =
      std::min(size_t(kMaxLookaheadForBoyerMoore), pattern->length());
  if (length <= kPatternTooShortForBoyerMoore) {
    return false;
  }

  if (pattern->hasLatin1Chars()) {
    return HasFewDifferentCharacters(pattern->latin1Chars(nogc), length);
  }
  MOZ_ASSERT(pattern->hasTwoByteChars());
  return HasFewDifferentCharacters(pattern->twoByteChars(nogc), length);
}

// Sample character frequency information for use in Boyer-Moore.
static void SampleCharacters(FlatStringReader* sample_subject,
                             RegExpCompiler& compiler) {
  static const int kSampleSize = 128;
  int chars_sampled = 0;

  int length = sample_subject->length();

  int half_way = (length - kSampleSize) / 2;
  for (int i = std::max(0, half_way); i < length && chars_sampled < kSampleSize;
       i++, chars_sampled++) {
    compiler.frequency_collator()->CountCharacter(sample_subject->Get(i));
  }
}

// Recursively walking the AST for a deeply nested regexp (like
// `/(a(a(a(a(a(a(a(...(a)...))))))))/`) may overflow the stack while
// compiling. To avoid this, we use V8's implementation of the Visitor
// pattern to walk the AST first with an overly large stack frame.
class RegExpDepthCheck final : public v8::internal::RegExpVisitor {
 public:
  explicit RegExpDepthCheck(JSContext* cx) : cx_(cx) {}

  bool check(v8::internal::RegExpTree* root) {
    return !!root->Accept(this, nullptr);
  }

  // Leaf nodes with no children
#define LEAF_DEPTH(Kind)                                                \
  void* Visit##Kind(v8::internal::RegExp##Kind* node, void*) override { \
    uint8_t padding[FRAME_PADDING];                                     \
    dummy_ = padding; /* Prevent padding from being optimized away.*/   \
    AutoCheckRecursionLimit recursion(cx_);                             \
    return (void*)recursion.checkDontReport(cx_);                       \
  }

  LEAF_DEPTH(Assertion)
  LEAF_DEPTH(Atom)
  LEAF_DEPTH(BackReference)
  LEAF_DEPTH(CharacterClass)
  LEAF_DEPTH(Empty)
  LEAF_DEPTH(Text)
#undef LEAF_DEPTH

  // Wrapper nodes with one child
#define WRAPPER_DEPTH(Kind)                                             \
  void* Visit##Kind(v8::internal::RegExp##Kind* node, void*) override { \
    uint8_t padding[FRAME_PADDING];                                     \
    dummy_ = padding; /* Prevent padding from being optimized away.*/   \
    AutoCheckRecursionLimit recursion(cx_);                             \
    if (!recursion.checkDontReport(cx_)) {                              \
      return nullptr;                                                   \
    }                                                                   \
    return node->body()->Accept(this, nullptr);                         \
  }

  WRAPPER_DEPTH(Capture)
  WRAPPER_DEPTH(Group)
  WRAPPER_DEPTH(Lookaround)
  WRAPPER_DEPTH(Quantifier)
#undef WRAPPER_DEPTH

  void* VisitAlternative(v8::internal::RegExpAlternative* node,
                         void*) override {
    uint8_t padding[FRAME_PADDING];
    dummy_ = padding; /* Prevent padding from being optimized away.*/
    AutoCheckRecursionLimit recursion(cx_);
    if (!recursion.checkDontReport(cx_)) {
      return nullptr;
    }
    for (auto* child : *node->nodes()) {
      if (!child->Accept(this, nullptr)) {
        return nullptr;
      }
    }
    return (void*)true;
  }
  void* VisitDisjunction(v8::internal::RegExpDisjunction* node,
                         void*) override {
    uint8_t padding[FRAME_PADDING];
    dummy_ = padding; /* Prevent padding from being optimized away.*/
    AutoCheckRecursionLimit recursion(cx_);
    if (!recursion.checkDontReport(cx_)) {
      return nullptr;
    }
    for (auto* child : *node->alternatives()) {
      if (!child->Accept(this, nullptr)) {
        return nullptr;
      }
    }
    return (void*)true;
  }

 private:
  JSContext* cx_;
  void* dummy_ = nullptr;

  // This size is picked to be comfortably larger than any
  // RegExp*::ToNode stack frame.
  static const size_t FRAME_PADDING = 256;
};

enum class AssembleResult {
  Success,
  TooLarge,
  OutOfMemory,
};

[[nodiscard]] static AssembleResult Assemble(
    JSContext* cx, RegExpCompiler* compiler, RegExpCompileData* data,
    MutableHandleRegExpShared re, HandleAtom pattern, Zone* zone,
    bool useNativeCode, bool isLatin1) {
  // Because we create a StackMacroAssembler, this function is not allowed
  // to GC. If needed, we allocate and throw errors in the caller.
  Maybe<jit::JitContext> jctx;
  Maybe<js::jit::StackMacroAssembler> stack_masm;
  UniquePtr<RegExpMacroAssembler> masm;
  if (useNativeCode) {
    NativeRegExpMacroAssembler::Mode mode =
        isLatin1 ? NativeRegExpMacroAssembler::LATIN1
                 : NativeRegExpMacroAssembler::UC16;
    // If we are compiling native code, we need a macroassembler,
    // which needs a jit context.
    jctx.emplace(cx, nullptr);
    stack_masm.emplace();
    uint32_t num_capture_registers = re->pairCount() * 2;
    masm = MakeUnique<SMRegExpMacroAssembler>(cx, stack_masm.ref(), zone, mode,
                                              num_capture_registers);
  } else {
    masm = MakeUnique<RegExpBytecodeGenerator>(cx->isolate, zone);
  }
  if (!masm) {
    return AssembleResult::OutOfMemory;
  }

  bool isLargePattern =
      pattern->length() > v8::internal::RegExp::kRegExpTooLargeToOptimize;
  masm->set_slow_safe(isLargePattern);
  if (compiler->optimize()) {
    compiler->set_optimize(!isLargePattern);
  }

  // When matching a regexp with known maximum length that is anchored
  // at the end, we may be able to skip the beginning of long input
  // strings. This decision is made here because it depends on
  // information in the AST that isn't replicated in the Node
  // structure used inside the compiler.
  bool is_start_anchored = data->tree->IsAnchoredAtStart();
  bool is_end_anchored = data->tree->IsAnchoredAtEnd();
  int max_length = data->tree->max_match();
  static const int kMaxBacksearchLimit = 1024;
  if (is_end_anchored && !is_start_anchored && !re->sticky() &&
      max_length < kMaxBacksearchLimit) {
    masm->SetCurrentPositionFromEnd(max_length);
  }

  if (re->global()) {
    RegExpMacroAssembler::GlobalMode mode = RegExpMacroAssembler::GLOBAL;
    if (data->tree->min_match() > 0) {
      mode = RegExpMacroAssembler::GLOBAL_NO_ZERO_LENGTH_CHECK;
    } else if (re->unicode()) {
      mode = RegExpMacroAssembler::GLOBAL_UNICODE;
    }
    masm->set_global_mode(mode);
  }

  // The masm tracer works as a thin wrapper around another macroassembler.
  RegExpMacroAssembler* masm_ptr = masm.get();
#ifdef DEBUG
  UniquePtr<RegExpMacroAssembler> tracer_masm;
  if (jit::JitOptions.traceRegExpAssembler) {
    tracer_masm = MakeUnique<RegExpMacroAssemblerTracer>(cx->isolate, masm_ptr);
    masm_ptr = tracer_masm.get();
  }
#endif

  // Compile the regexp.
  V8HandleString wrappedPattern(v8::internal::String(pattern), cx->isolate);
  RegExpCompiler::CompilationResult result = compiler->Assemble(
      cx->isolate, masm_ptr, data->node, data->capture_count, wrappedPattern);
  if (!result.Succeeded()) {
    MOZ_ASSERT(result.error == RegExpError::kTooLarge);
    return AssembleResult::TooLarge;
  }
  if (result.code->value().isUndefined()) {
    // SMRegExpMacroAssembler::GetCode returns undefined on OOM.
    MOZ_ASSERT(useNativeCode);
    return AssembleResult::OutOfMemory;
  }

  re->updateMaxRegisters(result.num_registers);
  if (useNativeCode) {
    // Transfer ownership of the tables from the macroassembler to the
    // RegExpShared.
    SMRegExpMacroAssembler::TableVector& tables =
        static_cast<SMRegExpMacroAssembler*>(masm.get())->tables();
    for (uint32_t i = 0; i < tables.length(); i++) {
      if (!re->addTable(std::move(tables[i]))) {
        return AssembleResult::OutOfMemory;
      }
    }
    re->setJitCode(v8::internal::Code::cast(*result.code).inner(), isLatin1);
  } else {
    // Transfer ownership of the bytecode from the HandleScope to the
    // RegExpShared.
    ByteArray bytecode =
        v8::internal::ByteArray::cast(*result.code).takeOwnership(cx->isolate);
    uint32_t length = bytecode->length;
    re->setByteCode(bytecode.release(), isLatin1);
    js::AddCellMemory(re, length, MemoryUse::RegExpSharedBytecode);
  }

  return AssembleResult::Success;
}

bool CompilePattern(JSContext* cx, MutableHandleRegExpShared re,
                    HandleLinearString input, RegExpShared::CodeKind codeKind) {
  RootedAtom pattern(cx, re->getSource());
  JS::RegExpFlags flags = re->getFlags();
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  HandleScope handleScope(cx->isolate);
  Zone zone(allocScope.alloc());

  RegExpCompileData data;
  {
    FlatStringReader patternBytes(cx, pattern);
    if (!RegExpParser::ParseRegExp(cx->isolate, &zone, &patternBytes, flags,
                                   &data)) {
      JS::CompileOptions options(cx);
      DummyTokenStream dummyTokenStream(cx, options);
      ReportSyntaxError(dummyTokenStream, data, pattern);
      return false;
    }
  }

  // Avoid stack overflow while recursively walking the AST.
  RegExpDepthCheck depthCheck(cx);
  if (!depthCheck.check(data.tree)) {
    JS_ReportErrorASCII(cx, "regexp too big");
    return false;
  }

  if (re->kind() == RegExpShared::Kind::Unparsed) {
    // This is the first time we have compiled this regexp.
    // First, check to see if we should use simple string search
    // with an atom.
    if (!flags.ignoreCase() && !flags.sticky()) {
      RootedAtom searchAtom(cx);
      if (data.simple) {
        // The parse-tree is a single atom that is equal to the pattern.
        searchAtom = re->getSource();
      } else if (data.tree->IsAtom() && data.capture_count == 0) {
        // The parse-tree is a single atom that is not equal to the pattern.
        v8::internal::RegExpAtom* atom = data.tree->AsAtom();
        const char16_t* twoByteChars = atom->data().begin();
        searchAtom = AtomizeChars(cx, twoByteChars, atom->length());
        if (!searchAtom) {
          return false;
        }
      }
      JS::AutoAssertNoGC nogc(cx);
      if (searchAtom && !UseBoyerMoore(searchAtom, nogc)) {
        re->useAtomMatch(searchAtom);
        return true;
      }
    }
    if (!data.capture_name_map.is_null()) {
      RootedNativeObject namedCaptures(cx, data.capture_name_map->inner());
      if (!RegExpShared::initializeNamedCaptures(cx, re, namedCaptures)) {
        return false;
      }
    }
    // All fallible initialization has succeeded, so we can change state.
    // Add one to capture_count to account for the whole-match capture.
    uint32_t pairCount = data.capture_count + 1;
    re->useRegExpMatch(pairCount);
  }

  MOZ_ASSERT(re->kind() == RegExpShared::Kind::RegExp);

  RegExpCompiler compiler(cx->isolate, &zone, data.capture_count,
                          input->hasLatin1Chars());

  bool isLatin1 = input->hasLatin1Chars();

  FlatStringReader sample_subject(cx, input);
  SampleCharacters(&sample_subject, compiler);
  data.node = compiler.PreprocessRegExp(&data, flags, isLatin1);
  data.error = AnalyzeRegExp(cx->isolate, isLatin1, data.node);
  if (data.error != RegExpError::kNone) {
    MOZ_ASSERT(data.error == RegExpError::kAnalysisStackOverflow);
    ReportOverRecursed(cx);
    return false;
  }

  bool useNativeCode = codeKind == RegExpShared::CodeKind::Jitcode;
  MOZ_ASSERT_IF(useNativeCode, IsNativeRegExpEnabled());

  switch (Assemble(cx, &compiler, &data, re, pattern, &zone, useNativeCode,
                   isLatin1)) {
    case AssembleResult::TooLarge:
      JS_ReportErrorASCII(cx, "regexp too big");
      return false;
    case AssembleResult::OutOfMemory:
      ReportOutOfMemory(cx);
      return false;
    case AssembleResult::Success:
      break;
  }
  return true;
}

template <typename CharT>
RegExpRunStatus ExecuteRaw(jit::JitCode* code, const CharT* chars,
                           size_t length, size_t startIndex,
                           VectorMatchPairs* matches) {
  InputOutputData data(chars, chars + length, startIndex, matches);

  static_assert(RegExpRunStatus_Error ==
                v8::internal::RegExp::kInternalRegExpException);
  static_assert(RegExpRunStatus_Success ==
                v8::internal::RegExp::kInternalRegExpSuccess);
  static_assert(RegExpRunStatus_Success_NotFound ==
                v8::internal::RegExp::kInternalRegExpFailure);

  typedef int (*RegExpCodeSignature)(InputOutputData*);
  auto function = reinterpret_cast<RegExpCodeSignature>(code->raw());
  {
    JS::AutoSuppressGCAnalysis nogc;
    return (RegExpRunStatus)CALL_GENERATED_1(function, &data);
  }
}

RegExpRunStatus Interpret(JSContext* cx, MutableHandleRegExpShared re,
                          HandleLinearString input, size_t startIndex,
                          VectorMatchPairs* matches) {
  MOZ_ASSERT(re->getByteCode(input->hasLatin1Chars()));

  HandleScope handleScope(cx->isolate);
  V8HandleRegExp wrappedRegExp(v8::internal::JSRegExp(re), cx->isolate);
  V8HandleString wrappedInput(v8::internal::String(input), cx->isolate);

  static_assert(RegExpRunStatus_Error ==
                v8::internal::RegExp::kInternalRegExpException);
  static_assert(RegExpRunStatus_Success ==
                v8::internal::RegExp::kInternalRegExpSuccess);
  static_assert(RegExpRunStatus_Success_NotFound ==
                v8::internal::RegExp::kInternalRegExpFailure);

  RegExpRunStatus status =
      (RegExpRunStatus)IrregexpInterpreter::MatchForCallFromRuntime(
          cx->isolate, wrappedRegExp, wrappedInput, matches->pairsRaw(),
          uint32_t(matches->pairCount() * 2), uint32_t(startIndex));

  MOZ_ASSERT(status == RegExpRunStatus_Error ||
             status == RegExpRunStatus_Success ||
             status == RegExpRunStatus_Success_NotFound);

  return status;
}

RegExpRunStatus Execute(JSContext* cx, MutableHandleRegExpShared re,
                        HandleLinearString input, size_t startIndex,
                        VectorMatchPairs* matches) {
  bool latin1 = input->hasLatin1Chars();
  jit::JitCode* jitCode = re->getJitCode(latin1);
  bool isCompiled = !!jitCode;

  // Reset the Irregexp backtrack stack if it grows during execution.
  irregexp::RegExpStackScope stackScope(cx->isolate);

  if (isCompiled) {
    JS::AutoCheckCannotGC nogc;
    if (latin1) {
      return ExecuteRaw(jitCode, input->latin1Chars(nogc), input->length(),
                        startIndex, matches);
    }
    return ExecuteRaw(jitCode, input->twoByteChars(nogc), input->length(),
                      startIndex, matches);
  }

  return Interpret(cx, re, input, startIndex, matches);
}

RegExpRunStatus ExecuteForFuzzing(JSContext* cx, HandleAtom pattern,
                                  HandleLinearString input,
                                  JS::RegExpFlags flags, size_t startIndex,
                                  VectorMatchPairs* matches,
                                  RegExpShared::CodeKind codeKind) {
  RootedRegExpShared re(cx, cx->zone()->regExps().get(cx, pattern, flags));
  if (!RegExpShared::compileIfNecessary(cx, &re, input, codeKind)) {
    return RegExpRunStatus_Error;
  }
  return RegExpShared::execute(cx, &re, input, startIndex, matches);
}

bool GrowBacktrackStack(RegExpStack* regexp_stack) {
  return SMRegExpMacroAssembler::GrowBacktrackStack(regexp_stack);
}

uint32_t CaseInsensitiveCompareNonUnicode(const char16_t* substring1,
                                          const char16_t* substring2,
                                          size_t byteLength) {
  return SMRegExpMacroAssembler::CaseInsensitiveCompareNonUnicode(
      substring1, substring2, byteLength);
}

uint32_t CaseInsensitiveCompareUnicode(const char16_t* substring1,
                                       const char16_t* substring2,
                                       size_t byteLength) {
  return SMRegExpMacroAssembler::CaseInsensitiveCompareUnicode(
      substring1, substring2, byteLength);
}

#ifdef DEBUG
bool IsolateShouldSimulateInterrupt(Isolate* isolate) {
  return isolate->shouldSimulateInterrupt_ != 0;
}

void IsolateSetShouldSimulateInterrupt(Isolate* isolate) {
  isolate->shouldSimulateInterrupt_ = 1;
}
void IsolateClearShouldSimulateInterrupt(Isolate* isolate) {
  isolate->shouldSimulateInterrupt_ = 0;
}
#endif

}  // namespace irregexp
}  // namespace js
