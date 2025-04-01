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

#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "frontend/TokenStream.h"
#include "gc/GC.h"
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
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin, JS::ColumnNumberOffset
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/friend/StackLimits.h"    // js::ReportOverRecursed
#include "util/StringBuffer.h"
#include "vm/MatchPairs.h"
#include "vm/PlainObject.h"
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
using v8::internal::HandleScope;
using v8::internal::InputOutputData;
using v8::internal::IrregexpInterpreter;
using v8::internal::NativeRegExpMacroAssembler;
using v8::internal::RegExpBytecodeGenerator;
using v8::internal::RegExpCapture;
using v8::internal::RegExpCompileData;
using v8::internal::RegExpCompiler;
using v8::internal::RegExpError;
using v8::internal::RegExpMacroAssembler;
using v8::internal::RegExpMacroAssemblerTracer;
using v8::internal::RegExpNode;
using v8::internal::RegExpParser;
using v8::internal::SMRegExpMacroAssembler;
using v8::internal::Zone;
using v8::internal::ZoneVector;

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

    case RegExpError::kInvalidClassSetOperation:
      return JSMSG_INVALID_CLASS_SET_OP;
    case RegExpError::kInvalidCharacterInClass:
      return JSMSG_INVALID_CHAR_IN_CLASS;
    case RegExpError::kNegatedCharacterClassWithStrings:
      return JSMSG_NEGATED_CLASS_WITH_STR;

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

void TraceIsolate(JSTracer* trc, Isolate* isolate) { isolate->trace(trc); }

void DestroyIsolate(Isolate* isolate) {
  MOZ_ASSERT(isolate->liveHandles() == 0);
  MOZ_ASSERT(isolate->livePseudoHandles() == 0);
  js_delete(isolate);
}

size_t IsolateSizeOfIncludingThis(Isolate* isolate,
                                  mozilla::MallocSizeOf mallocSizeOf) {
  return isolate->sizeOfIncludingThis(mallocSizeOf);
}

static JS::ColumnNumberOffset ComputeColumnOffset(const Latin1Char* begin,
                                                  const Latin1Char* end) {
  return JS::ColumnNumberOffset(
      AssertedCast<uint32_t>(PointerRangeSize(begin, end)));
}

static JS::ColumnNumberOffset ComputeColumnOffset(const char16_t* begin,
                                                  const char16_t* end) {
  return JS::ColumnNumberOffset(
      AssertedCast<uint32_t>(unicode::CountUTF16CodeUnits(begin, end)));
}

// This function is varargs purely so it can call ReportCompileErrorLatin1.
// We never call it with additional arguments.
template <typename CharT>
static void ReportSyntaxError(TokenStreamAnyChars& ts,
                              mozilla::Maybe<uint32_t> line,
                              mozilla::Maybe<JS::ColumnNumberOneOrigin> column,
                              RegExpCompileData& result, CharT* start,
                              size_t length, ...) {
  MOZ_ASSERT(line.isSome() == column.isSome());

  Maybe<gc::AutoSuppressGC> suppressGC;
  if (JSContext* maybeCx = ts.context()->maybeCurrentJSContext()) {
    suppressGC.emplace(maybeCx);
  }
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
    JS::ColumnNumberOffset columnOffset =
        ComputeColumnOffset(start, start + offset);
    if (line.isSome()) {
      // If this pattern is being checked by the frontend Parser instead
      // of other API entry points like |new RegExp|, then the parser will
      // have provided both a line and column pointing at the *beginning*
      // of the RegExp literal inside the source text.
      // We adjust the columnNumber to point to the actual syntax error
      // inside the literal.
      err.lineNumber = *line;
      err.columnNumber = *column + columnOffset;
    } else {
      // Line breaks are not significant in pattern text in the same way as
      // in source text, so act as though pattern text is a single line, then
      // compute a column based on "code point" count (treating a lone
      // surrogate as a "code point" in UTF-16).  Gak.
      err.lineNumber = 1;
      err.columnNumber = JS::ColumnNumberOneOrigin() + columnOffset;
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
  ReportCompileErrorLatin1VA(ts.context(), std::move(err), nullptr, errorNumber,
                             &args);
  va_end(args);
}

static void ReportSyntaxError(TokenStreamAnyChars& ts,
                              RegExpCompileData& result,
                              Handle<JSAtom*> pattern) {
  JS::AutoCheckCannotGC nogc_;
  if (pattern->hasLatin1Chars()) {
    ReportSyntaxError(ts, Nothing(), Nothing(), result,
                      pattern->latin1Chars(nogc_), pattern->length());
  } else {
    ReportSyntaxError(ts, Nothing(), Nothing(), result,
                      pattern->twoByteChars(nogc_), pattern->length());
  }
}

template <typename CharT>
static bool CheckPatternSyntaxImpl(js::LifoAlloc& alloc,
                                   JS::NativeStackLimit stackLimit,
                                   const CharT* input, uint32_t inputLength,
                                   JS::RegExpFlags flags,
                                   RegExpCompileData* result,
                                   JS::AutoAssertNoGC& nogc) {
  LifoAllocScope allocScope(&alloc);
  Zone zone(allocScope.alloc());

  return RegExpParser::VerifyRegExpSyntax(&zone, stackLimit, input, inputLength,
                                          flags, result, nogc);
}

bool CheckPatternSyntax(js::LifoAlloc& alloc, JS::NativeStackLimit stackLimit,
                        TokenStreamAnyChars& ts,
                        const mozilla::Range<const char16_t> chars,
                        JS::RegExpFlags flags, mozilla::Maybe<uint32_t> line,
                        mozilla::Maybe<JS::ColumnNumberOneOrigin> column) {
  RegExpCompileData result;
  JS::AutoAssertNoGC nogc;
  if (!CheckPatternSyntaxImpl(alloc, stackLimit, chars.begin().get(),
                              chars.length(), flags, &result, nogc)) {
    ReportSyntaxError(ts, line, column, result, chars.begin().get(),
                      chars.length());
    return false;
  }
  return true;
}

bool CheckPatternSyntax(JSContext* cx, JS::NativeStackLimit stackLimit,
                        TokenStreamAnyChars& ts, Handle<JSAtom*> pattern,
                        JS::RegExpFlags flags) {
  RegExpCompileData result;
  JS::AutoAssertNoGC nogc(cx);
  if (pattern->hasLatin1Chars()) {
    if (!CheckPatternSyntaxImpl(cx->tempLifoAlloc(), stackLimit,
                                pattern->latin1Chars(nogc), pattern->length(),
                                flags, &result, nogc)) {
      ReportSyntaxError(ts, result, pattern);
      return false;
    }
    return true;
  }
  if (!CheckPatternSyntaxImpl(cx->tempLifoAlloc(), stackLimit,
                              pattern->twoByteChars(nogc), pattern->length(),
                              flags, &result, nogc)) {
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
  bool character_found[tableSize] = {};
  uint32_t different = 0;
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
static bool UseBoyerMoore(Handle<JSAtom*> pattern, JS::AutoAssertNoGC& nogc) {
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
static void SampleCharacters(Handle<JSLinearString*> sample_subject,
                             RegExpCompiler& compiler) {
  static const int kSampleSize = 128;
  int chars_sampled = 0;

  int length = sample_subject->length();

  int half_way = (length - kSampleSize) / 2;
  for (int i = std::max(0, half_way); i < length && chars_sampled < kSampleSize;
       i++, chars_sampled++) {
    compiler.frequency_collator()->CountCharacter(
        sample_subject->latin1OrTwoByteChar(i));
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
  LEAF_DEPTH(ClassSetOperand)
  LEAF_DEPTH(ClassRanges)
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
  void* VisitClassSetExpression(v8::internal::RegExpClassSetExpression* node,
                                void*) override {
    uint8_t padding[FRAME_PADDING];
    dummy_ = padding; /* Prevent padding from being optimized away.*/
    AutoCheckRecursionLimit recursion(cx_);
    if (!recursion.checkDontReport(cx_)) {
      return nullptr;
    }
    for (auto* child : *node->operands()) {
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
    MutableHandleRegExpShared re, Handle<JSAtom*> pattern, Zone* zone,
    bool useNativeCode, bool isLatin1) {
  // Because we create a StackMacroAssembler, this function is not allowed
  // to GC. If needed, we allocate and throw errors in the caller.
  jit::TempAllocator temp(&cx->tempLifoAlloc());
  Maybe<jit::JitContext> jctx;
  Maybe<js::jit::StackMacroAssembler> stack_masm;
  UniquePtr<RegExpMacroAssembler> masm;
  if (useNativeCode) {
    NativeRegExpMacroAssembler::Mode mode =
        isLatin1 ? NativeRegExpMacroAssembler::LATIN1
                 : NativeRegExpMacroAssembler::UC16;
    // If we are compiling native code, we need a macroassembler,
    // which needs a jit context.
    jctx.emplace(cx);
    stack_masm.emplace(cx, temp);
#ifdef DEBUG
    // It would be much preferable to use `class AutoCreatedBy` here, but we
    // may be operating without an assembler at all if `useNativeCode` is
    // `false`, so there's no place to put such a call.
    stack_masm.ref().pushCreator("Assemble() in RegExpAPI.cpp");
#endif
    uint32_t num_capture_registers = re->pairCount() * 2;
    masm = MakeUnique<SMRegExpMacroAssembler>(cx, stack_masm.ref(), zone, mode,
                                              num_capture_registers);
  } else {
    masm = MakeUnique<RegExpBytecodeGenerator>(cx->isolate, zone);
  }
  if (!masm) {
    ReportOutOfMemory(cx);
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
  if (jit::JitOptions.trace_regexp_assembler) {
    tracer_masm = MakeUnique<RegExpMacroAssemblerTracer>(cx->isolate, masm_ptr);
    masm_ptr = tracer_masm.get();
  }
#endif

  // Compile the regexp.
  V8HandleString wrappedPattern(v8::internal::String(pattern), cx->isolate);
  RegExpCompiler::CompilationResult result = compiler->Assemble(
      cx->isolate, masm_ptr, data->node, data->capture_count, wrappedPattern);

  if (useNativeCode) {
#ifdef DEBUG
    // See comment referencing `pushCreator` above.
    stack_masm.ref().popCreator();
#endif
  }

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
        ReportOutOfMemory(cx);
        return AssembleResult::OutOfMemory;
      }
    }
    re->setJitCode(v8::internal::Code::cast(*result.code).inner(), isLatin1);
  } else {
    // Transfer ownership of the bytecode from the HandleScope to the
    // RegExpShared.
    ByteArray bytecode =
        v8::internal::ByteArray::cast(*result.code).takeOwnership(cx->isolate);
    uint32_t length = bytecode->length();
    re->setByteCode(bytecode.release(), isLatin1);
    js::AddCellMemory(re, length, MemoryUse::RegExpSharedBytecode);
  }

  return AssembleResult::Success;
}

struct RegExpNamedCapture {
  const ZoneVector<char16_t>* name;
  js::Vector<uint32_t> indices;

  RegExpNamedCapture(JSContext* cx, const ZoneVector<char16_t>* name)
      : name(name), indices(cx) {}
};

struct RegExpNamedCaptureIndexLess {
  bool operator()(const RegExpNamedCapture& lhs,
                  const RegExpNamedCapture& rhs) const {
    // Every name must have at least one corresponding capture index, and all
    // the capture indices must be distinct. This allows us to sort on the
    // lowest capture index.
    MOZ_ASSERT(!lhs.indices.empty());
    MOZ_ASSERT(!rhs.indices.empty());
    MOZ_ASSERT(lhs.indices[0] != rhs.indices[0]);
    return lhs.indices[0] < rhs.indices[0];
  }
};

bool InitializeNamedCaptures(JSContext* cx, HandleRegExpShared re,
                             ZoneVector<RegExpCapture*>* namedCaptures) {
  // The irregexp parser returns named capture information in the form
  // of a ZoneVector of RegExpCaptures nodes, each of which stores the
  // capture name and the corresponding capture index. We create a
  // template object with a property for each capture name, and store
  // the capture indices as a heap-allocated array.
  uint32_t numNamedCaptures = namedCaptures->size();

  // The input vector of named captures is already sorted by name, and then by
  // capture index if there are duplicates. We iterate through the captures,
  // creating groups for each set of indices corresponding to a name. Usually,
  // there will be a 1:1 mapping.
  js::Vector<RegExpNamedCapture> groups(cx);
  if (!groups.reserve(numNamedCaptures)) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  const ZoneVector<char16_t>* prevName = nullptr;
  uint32_t numDistinctNamedCaptures = 0;
  for (uint32_t i = 0; i < numNamedCaptures; i++) {
    RegExpCapture* capture = (*namedCaptures)[i];
    const ZoneVector<char16_t>* name = capture->name();
    if (!prevName || *name != *prevName) {
      if (!groups.emplaceBack(RegExpNamedCapture(cx, name))) {
        js::ReportOutOfMemory(cx);
        return false;
      }
      numDistinctNamedCaptures++;
      prevName = name;
    }
    // Make sure we're getting the indices in the order we expect
    MOZ_ASSERT_IF(!groups.back().indices.empty(),
                  groups.back().indices.back() < (uint32_t)capture->index());
    if (!groups.back().indices.emplaceBack(capture->index())) {
      js::ReportOutOfMemory(cx);
      return false;
    }
  }

  // The capture name map must be sorted by index.
  std::sort(groups.begin(), groups.end(), RegExpNamedCaptureIndexLess{});

  // Create a plain template object.
  Rooted<js::PlainObject*> templateObject(
      cx, js::NewPlainObjectWithProto(cx, nullptr, TenuredObject));
  if (!templateObject) {
    return false;
  }

  // Allocate the capture index array.
  uint32_t arraySize = numNamedCaptures * sizeof(uint32_t);
  UniquePtr<uint32_t[], JS::FreePolicy> captureIndices(
      static_cast<uint32_t*>(js_malloc(arraySize)));
  if (!captureIndices) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  // Allocate the capture slice index array, if necessary. We only use this
  // if we have duplicate named capture groups.
  bool hasDuplicateNames = numNamedCaptures != numDistinctNamedCaptures;
  UniquePtr<uint32_t[], JS::FreePolicy> sliceIndices;
  if (hasDuplicateNames) {
    arraySize = numDistinctNamedCaptures * sizeof(uint32_t);
    sliceIndices.reset(static_cast<uint32_t*>(js_malloc(arraySize)));
    if (!sliceIndices) {
      js::ReportOutOfMemory(cx);
      return false;
    }
  }

  // Initialize the properties of the template and store capture indices.
  RootedId id(cx);
  RootedValue dummyString(cx, StringValue(cx->runtime()->emptyString));
  size_t insertIndex = 0;

  for (size_t i = 0; i < numDistinctNamedCaptures; ++i) {
    RegExpNamedCapture& group = groups[i];
    // We store the names as properties on the template object, in the order of
    // their lowest capture index.
    JSAtom* name = js::AtomizeChars(cx, group.name->data(), group.name->size());
    if (!name) {
      return false;
    }
    id = NameToId(name->asPropertyName());
    if (!NativeDefineDataProperty(cx, templateObject, id, dummyString,
                                  JSPROP_ENUMERATE)) {
      return false;
    }
    // The slice index keeps track of the captureIndex where indices
    // corresponding to a name start. The difference between the current slice
    // index and the next slice index is used to calculate how many values to
    // return in the slice. This is only needed when we have duplicate capture
    // names, otherwise, there's a 1:1 mapping, and we don't need the extra
    // data.
    if (hasDuplicateNames) {
      sliceIndices[i] = insertIndex;
    }

    for (uint32_t captureIndex : groups[i].indices) {
      captureIndices[insertIndex++] = captureIndex;
    }
  }

  RegExpShared::InitializeNamedCaptures(
      cx, re, numNamedCaptures, numDistinctNamedCaptures, templateObject,
      captureIndices.release(), sliceIndices.release());
  return true;
}

bool CompilePattern(JSContext* cx, MutableHandleRegExpShared re,
                    Handle<JSLinearString*> input,
                    RegExpShared::CodeKind codeKind) {
  Rooted<JSAtom*> pattern(cx, re->getSource());
  JS::RegExpFlags flags = re->getFlags();
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  HandleScope handleScope(cx->isolate);
  Zone zone(allocScope.alloc());

  RegExpCompileData data;
  {
    V8HandleString wrappedPattern(v8::internal::String(pattern), cx->isolate);
    if (!RegExpParser::ParseRegExpFromHeapString(
            cx->isolate, &zone, wrappedPattern, flags, &data)) {
      AutoReportFrontendContext fc(cx);
      JS::CompileOptions options(cx);
      DummyTokenStream dummyTokenStream(&fc, options);
      ReportSyntaxError(dummyTokenStream, data, pattern);
      return false;
    }
  }

  // Avoid stack overflow while recursively walking the AST.
  RegExpDepthCheck depthCheck(cx);
  if (!depthCheck.check(data.tree)) {
    JS_ReportErrorASCII(cx, "regexp too big");
    cx->reportResourceExhaustion();
    return false;
  }

  if (re->kind() == RegExpShared::Kind::Unparsed) {
    // This is the first time we have compiled this regexp.
    // First, check to see if we should use simple string search
    // with an atom.
    if (!flags.ignoreCase() && !flags.sticky()) {
      Rooted<JSAtom*> searchAtom(cx);
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
    if (data.named_captures) {
      if (!InitializeNamedCaptures(cx, re, data.named_captures)) {
        return false;
      }
    }
    // All fallible initialization has succeeded, so we can change state.
    // Add one to capture_count to account for the whole-match capture.
    uint32_t pairCount = data.capture_count + 1;
    re->useRegExpMatch(pairCount);
  }

  MOZ_ASSERT(re->kind() == RegExpShared::Kind::RegExp);

  RegExpCompiler compiler(cx->isolate, &zone, data.capture_count, flags,
                          input->hasLatin1Chars());

  bool isLatin1 = input->hasLatin1Chars();

  SampleCharacters(input, compiler);
  data.node = compiler.PreprocessRegExp(&data, isLatin1);
  data.error = AnalyzeRegExp(cx->isolate, isLatin1, flags, data.node);
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
      cx->reportResourceExhaustion();
      return false;
    case AssembleResult::OutOfMemory:
      MOZ_ASSERT(cx->isThrowingOutOfMemory());
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

  static_assert(static_cast<int32_t>(RegExpRunStatus::Error) ==
                v8::internal::RegExp::kInternalRegExpException);
  static_assert(static_cast<int32_t>(RegExpRunStatus::Success) ==
                v8::internal::RegExp::kInternalRegExpSuccess);
  static_assert(static_cast<int32_t>(RegExpRunStatus::Success_NotFound) ==
                v8::internal::RegExp::kInternalRegExpFailure);

  typedef int (*RegExpCodeSignature)(InputOutputData*);
  auto function = reinterpret_cast<RegExpCodeSignature>(code->raw());
  {
    JS::AutoSuppressGCAnalysis nogc;
    return (RegExpRunStatus)CALL_GENERATED_1(function, &data);
  }
}

RegExpRunStatus Interpret(JSContext* cx, MutableHandleRegExpShared re,
                          Handle<JSLinearString*> input, size_t startIndex,
                          VectorMatchPairs* matches) {
  MOZ_ASSERT(re->getByteCode(input->hasLatin1Chars()));

  HandleScope handleScope(cx->isolate);
  V8HandleRegExp wrappedRegExp(v8::internal::JSRegExp(re), cx->isolate);
  V8HandleString wrappedInput(v8::internal::String(input), cx->isolate);

  static_assert(static_cast<int32_t>(RegExpRunStatus::Error) ==
                v8::internal::RegExp::kInternalRegExpException);
  static_assert(static_cast<int32_t>(RegExpRunStatus::Success) ==
                v8::internal::RegExp::kInternalRegExpSuccess);
  static_assert(static_cast<int32_t>(RegExpRunStatus::Success_NotFound) ==
                v8::internal::RegExp::kInternalRegExpFailure);

  RegExpRunStatus status =
      (RegExpRunStatus)IrregexpInterpreter::MatchForCallFromRuntime(
          cx->isolate, wrappedRegExp, wrappedInput, matches->pairsRaw(),
          uint32_t(matches->pairCount() * 2), uint32_t(startIndex));

  MOZ_ASSERT(status == RegExpRunStatus::Error ||
             status == RegExpRunStatus::Success ||
             status == RegExpRunStatus::Success_NotFound);

  return status;
}

RegExpRunStatus Execute(JSContext* cx, MutableHandleRegExpShared re,
                        Handle<JSLinearString*> input, size_t startIndex,
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

RegExpRunStatus ExecuteForFuzzing(JSContext* cx, Handle<JSAtom*> pattern,
                                  Handle<JSLinearString*> input,
                                  JS::RegExpFlags flags, size_t startIndex,
                                  VectorMatchPairs* matches,
                                  RegExpShared::CodeKind codeKind) {
  RootedRegExpShared re(cx, cx->zone()->regExps().get(cx, pattern, flags));
  if (!RegExpShared::compileIfNecessary(cx, &re, input, codeKind)) {
    return RegExpRunStatus::Error;
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

bool IsCharacterInRangeArray(uint32_t c, ByteArrayData* ranges) {
  return SMRegExpMacroAssembler::IsCharacterInRangeArray(c, ranges);
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
