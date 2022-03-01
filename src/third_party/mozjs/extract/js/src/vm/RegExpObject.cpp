/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RegExpObject.h"

#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include <algorithm>
#include <type_traits>

#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"  // REGEXP_*_FLAG
#include "frontend/TokenStream.h"
#include "gc/HashUtil.h"
#include "irregexp/RegExpAPI.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::ReportOverRecursed
#include "js/Object.h"                // JS::GetBuiltinClass
#include "js/RegExp.h"
#include "js/RegExpFlags.h"  // JS::RegExpFlags
#include "js/StableStringChars.h"
#include "util/StringBuffer.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpStatics.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"
#ifdef DEBUG
#  include "util/Unicode.h"
#endif
#include "vm/WellKnownAtom.h"  // js_*_str
#include "vm/Xdr.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::RegExpFlag;
using JS::RegExpFlags;
using mozilla::DebugOnly;
using mozilla::PodCopy;

using JS::AutoCheckCannotGC;

static_assert(RegExpFlag::HasIndices == REGEXP_HASINDICES_FLAG,
              "self-hosted JS and /d flag bits must agree");
static_assert(RegExpFlag::Global == REGEXP_GLOBAL_FLAG,
              "self-hosted JS and /g flag bits must agree");
static_assert(RegExpFlag::IgnoreCase == REGEXP_IGNORECASE_FLAG,
              "self-hosted JS and /i flag bits must agree");
static_assert(RegExpFlag::Multiline == REGEXP_MULTILINE_FLAG,
              "self-hosted JS and /m flag bits must agree");
static_assert(RegExpFlag::DotAll == REGEXP_DOTALL_FLAG,
              "self-hosted JS and /s flag bits must agree");
static_assert(RegExpFlag::Unicode == REGEXP_UNICODE_FLAG,
              "self-hosted JS and /u flag bits must agree");
static_assert(RegExpFlag::Sticky == REGEXP_STICKY_FLAG,
              "self-hosted JS and /y flag bits must agree");

RegExpObject* js::RegExpAlloc(JSContext* cx, NewObjectKind newKind,
                              HandleObject proto /* = nullptr */) {
  Rooted<RegExpObject*> regexp(
      cx, NewObjectWithClassProtoAndKind<RegExpObject>(cx, proto, newKind));
  if (!regexp) {
    return nullptr;
  }

  regexp->clearShared();

  if (!SharedShape::ensureInitialCustomShape<RegExpObject>(cx, regexp)) {
    return nullptr;
  }

  MOZ_ASSERT(regexp->lookupPure(cx->names().lastIndex)->slot() ==
             RegExpObject::lastIndexSlot());

  return regexp;
}

/* MatchPairs */

bool VectorMatchPairs::initArrayFrom(VectorMatchPairs& copyFrom) {
  MOZ_ASSERT(copyFrom.pairCount() > 0);

  if (!allocOrExpandArray(copyFrom.pairCount())) {
    return false;
  }

  PodCopy(pairs_, copyFrom.pairs_, pairCount_);

  return true;
}

bool VectorMatchPairs::allocOrExpandArray(size_t pairCount) {
  if (!vec_.resizeUninitialized(pairCount)) {
    return false;
  }

  pairs_ = &vec_[0];
  pairCount_ = pairCount;
  return true;
}

/* RegExpObject */

/* static */
RegExpShared* RegExpObject::getShared(JSContext* cx,
                                      Handle<RegExpObject*> regexp) {
  if (regexp->hasShared()) {
    return regexp->getShared();
  }

  return createShared(cx, regexp);
}

/* static */
bool RegExpObject::isOriginalFlagGetter(JSNative native, RegExpFlags* mask) {
  if (native == regexp_hasIndices) {
    *mask = RegExpFlag::HasIndices;
    return true;
  }
  if (native == regexp_global) {
    *mask = RegExpFlag::Global;
    return true;
  }
  if (native == regexp_ignoreCase) {
    *mask = RegExpFlag::IgnoreCase;
    return true;
  }
  if (native == regexp_multiline) {
    *mask = RegExpFlag::Multiline;
    return true;
  }
  if (native == regexp_dotAll) {
    *mask = RegExpFlag::DotAll;
    return true;
  }
  if (native == regexp_sticky) {
    *mask = RegExpFlag::Sticky;
    return true;
  }
  if (native == regexp_unicode) {
    *mask = RegExpFlag::Unicode;
    return true;
  }

  return false;
}

static inline bool IsMarkingTrace(JSTracer* trc) {
  // Determine whether tracing is happening during normal marking.  We need to
  // test all the following conditions, since:
  //
  //   1. During TraceRuntime, RuntimeHeapIsBusy() is true, but the
  //      tracer might not be a marking tracer.
  //   2. When a write barrier executes, isMarkingTracer is true, but
  //      RuntimeHeapIsBusy() will be false.

  return JS::RuntimeHeapIsCollecting() && trc->isMarkingTracer();
}

static const ClassSpec RegExpObjectClassSpec = {
    GenericCreateConstructor<js::regexp_construct, 2, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<RegExpObject>,
    nullptr,
    js::regexp_static_props,
    js::regexp_methods,
    js::regexp_properties};

const JSClass RegExpObject::class_ = {
    js_RegExp_str,
    JSCLASS_HAS_RESERVED_SLOTS(RegExpObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_RegExp),
    JS_NULL_CLASS_OPS, &RegExpObjectClassSpec};

const JSClass RegExpObject::protoClass_ = {
    "RegExp.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_RegExp),
    JS_NULL_CLASS_OPS, &RegExpObjectClassSpec};

template <typename CharT>
RegExpObject* RegExpObject::create(JSContext* cx, const CharT* chars,
                                   size_t length, RegExpFlags flags,
                                   NewObjectKind newKind) {
  static_assert(std::is_same_v<CharT, char16_t>,
                "this code may need updating if/when CharT encodes UTF-8");

  RootedAtom source(cx, AtomizeChars(cx, chars, length));
  if (!source) {
    return nullptr;
  }

  return create(cx, source, flags, newKind);
}

template RegExpObject* RegExpObject::create(JSContext* cx,
                                            const char16_t* chars,
                                            size_t length, RegExpFlags flags,
                                            NewObjectKind newKind);

RegExpObject* RegExpObject::createSyntaxChecked(JSContext* cx,
                                                HandleAtom source,
                                                RegExpFlags flags,
                                                NewObjectKind newKind) {
  Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, newKind));
  if (!regexp) {
    return nullptr;
  }

  regexp->initAndZeroLastIndex(source, flags, cx);

  return regexp;
}

RegExpObject* RegExpObject::create(JSContext* cx, HandleAtom source,
                                   RegExpFlags flags, NewObjectKind newKind) {
  CompileOptions dummyOptions(cx);
  frontend::DummyTokenStream dummyTokenStream(cx, dummyOptions);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  if (!irregexp::CheckPatternSyntax(cx, dummyTokenStream, source, flags)) {
    return nullptr;
  }

  Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, newKind));
  if (!regexp) {
    return nullptr;
  }

  regexp->initAndZeroLastIndex(source, flags, cx);

  MOZ_ASSERT(!regexp->hasShared());

  return regexp;
}

/* static */
RegExpShared* RegExpObject::createShared(JSContext* cx,
                                         Handle<RegExpObject*> regexp) {
  MOZ_ASSERT(!regexp->hasShared());
  RootedAtom source(cx, regexp->getSource());
  RegExpShared* shared =
      cx->zone()->regExps().get(cx, source, regexp->getFlags());
  if (!shared) {
    return nullptr;
  }

  regexp->setShared(shared);

  MOZ_ASSERT(regexp->hasShared());

  return shared;
}

Shape* RegExpObject::assignInitialShape(JSContext* cx,
                                        Handle<RegExpObject*> self) {
  MOZ_ASSERT(self->empty());

  static_assert(LAST_INDEX_SLOT == 0);

  /* The lastIndex property alone is writable but non-configurable. */
  if (!NativeObject::addPropertyInReservedSlot(cx, self, cx->names().lastIndex,
                                               LAST_INDEX_SLOT,
                                               {PropertyFlag::Writable})) {
    return nullptr;
  }

  return self->shape();
}

void RegExpObject::initIgnoringLastIndex(JSAtom* source, RegExpFlags flags) {
  // If this is a re-initialization with an existing RegExpShared, 'flags'
  // may not match getShared()->flags, so forget the RegExpShared.
  clearShared();

  setSource(source);
  setFlags(flags);
}

void RegExpObject::initAndZeroLastIndex(JSAtom* source, RegExpFlags flags,
                                        JSContext* cx) {
  initIgnoringLastIndex(source, flags);
  zeroLastIndex(cx);
}

static MOZ_ALWAYS_INLINE bool IsRegExpLineTerminator(const JS::Latin1Char c) {
  return c == '\n' || c == '\r';
}

static MOZ_ALWAYS_INLINE bool IsRegExpLineTerminator(const char16_t c) {
  return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static MOZ_ALWAYS_INLINE bool AppendEscapedLineTerminator(
    StringBuffer& sb, const JS::Latin1Char c) {
  switch (c) {
    case '\n':
      if (!sb.append('n')) {
        return false;
      }
      break;
    case '\r':
      if (!sb.append('r')) {
        return false;
      }
      break;
    default:
      MOZ_CRASH("Bad LineTerminator");
  }
  return true;
}

static MOZ_ALWAYS_INLINE bool AppendEscapedLineTerminator(StringBuffer& sb,
                                                          const char16_t c) {
  switch (c) {
    case '\n':
      if (!sb.append('n')) {
        return false;
      }
      break;
    case '\r':
      if (!sb.append('r')) {
        return false;
      }
      break;
    case 0x2028:
      if (!sb.append("u2028")) {
        return false;
      }
      break;
    case 0x2029:
      if (!sb.append("u2029")) {
        return false;
      }
      break;
    default:
      MOZ_CRASH("Bad LineTerminator");
  }
  return true;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE bool SetupBuffer(StringBuffer& sb,
                                          const CharT* oldChars, size_t oldLen,
                                          const CharT* it) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    if (!sb.ensureTwoByteChars()) {
      return false;
    }
  }

  if (!sb.reserve(oldLen + 1)) {
    return false;
  }

  sb.infallibleAppend(oldChars, size_t(it - oldChars));
  return true;
}

// Note: leaves the string buffer empty if no escaping need be performed.
template <typename CharT>
static bool EscapeRegExpPattern(StringBuffer& sb, const CharT* oldChars,
                                size_t oldLen) {
  bool inBrackets = false;
  bool previousCharacterWasBackslash = false;

  for (const CharT* it = oldChars; it < oldChars + oldLen; ++it) {
    CharT ch = *it;
    if (!previousCharacterWasBackslash) {
      if (inBrackets) {
        if (ch == ']') {
          inBrackets = false;
        }
      } else if (ch == '/') {
        // There's a forward slash that needs escaping.
        if (sb.empty()) {
          // This is the first char we've seen that needs escaping,
          // copy everything up to this point.
          if (!SetupBuffer(sb, oldChars, oldLen, it)) {
            return false;
          }
        }
        if (!sb.append('\\')) {
          return false;
        }
      } else if (ch == '[') {
        inBrackets = true;
      }
    }

    if (IsRegExpLineTerminator(ch)) {
      // There's LineTerminator that needs escaping.
      if (sb.empty()) {
        // This is the first char we've seen that needs escaping,
        // copy everything up to this point.
        if (!SetupBuffer(sb, oldChars, oldLen, it)) {
          return false;
        }
      }
      if (!previousCharacterWasBackslash) {
        if (!sb.append('\\')) {
          return false;
        }
      }
      if (!AppendEscapedLineTerminator(sb, ch)) {
        return false;
      }
    } else if (!sb.empty()) {
      if (!sb.append(ch)) {
        return false;
      }
    }

    if (previousCharacterWasBackslash) {
      previousCharacterWasBackslash = false;
    } else if (ch == '\\') {
      previousCharacterWasBackslash = true;
    }
  }

  return true;
}

// ES6 draft rev32 21.2.3.2.4.
JSLinearString* js::EscapeRegExpPattern(JSContext* cx, HandleAtom src) {
  // Step 2.
  if (src->length() == 0) {
    return cx->names().emptyRegExp;
  }

  // We may never need to use |sb|. Start using it lazily.
  JSStringBuilder sb(cx);

  if (src->hasLatin1Chars()) {
    JS::AutoCheckCannotGC nogc;
    if (!::EscapeRegExpPattern(sb, src->latin1Chars(nogc), src->length())) {
      return nullptr;
    }
  } else {
    JS::AutoCheckCannotGC nogc;
    if (!::EscapeRegExpPattern(sb, src->twoByteChars(nogc), src->length())) {
      return nullptr;
    }
  }

  // Step 3.
  return sb.empty() ? src : sb.finishString();
}

// ES6 draft rev32 21.2.5.14. Optimized for RegExpObject.
JSLinearString* RegExpObject::toString(JSContext* cx,
                                       Handle<RegExpObject*> obj) {
  // Steps 3-4.
  RootedAtom src(cx, obj->getSource());
  if (!src) {
    return nullptr;
  }
  RootedLinearString escapedSrc(cx, EscapeRegExpPattern(cx, src));

  // Step 7.
  JSStringBuilder sb(cx);
  size_t len = escapedSrc->length();
  if (!sb.reserve(len + 2)) {
    return nullptr;
  }
  sb.infallibleAppend('/');
  if (!sb.append(escapedSrc)) {
    return nullptr;
  }
  sb.infallibleAppend('/');

  // Steps 5-7.
  if (obj->hasIndices() && !sb.append('d')) {
    return nullptr;
  }
  if (obj->global() && !sb.append('g')) {
    return nullptr;
  }
  if (obj->ignoreCase() && !sb.append('i')) {
    return nullptr;
  }
  if (obj->multiline() && !sb.append('m')) {
    return nullptr;
  }
  if (obj->dotAll() && !sb.append('s')) {
    return nullptr;
  }
  if (obj->unicode() && !sb.append('u')) {
    return nullptr;
  }
  if (obj->sticky() && !sb.append('y')) {
    return nullptr;
  }

  return sb.finishString();
}

template <typename CharT>
static MOZ_ALWAYS_INLINE bool IsRegExpMetaChar(CharT ch) {
  switch (ch) {
    /* ES 2016 draft Mar 25, 2016 21.2.1 SyntaxCharacter. */
    case '^':
    case '$':
    case '\\':
    case '.':
    case '*':
    case '+':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
      return true;
    default:
      return false;
  }
}

template <typename CharT>
bool js::HasRegExpMetaChars(const CharT* chars, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (IsRegExpMetaChar<CharT>(chars[i])) {
      return true;
    }
  }
  return false;
}

template bool js::HasRegExpMetaChars<Latin1Char>(const Latin1Char* chars,
                                                 size_t length);

template bool js::HasRegExpMetaChars<char16_t>(const char16_t* chars,
                                               size_t length);

bool js::StringHasRegExpMetaChars(JSLinearString* str) {
  AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return HasRegExpMetaChars(str->latin1Chars(nogc), str->length());
  }

  return HasRegExpMetaChars(str->twoByteChars(nogc), str->length());
}

/* RegExpShared */

RegExpShared::RegExpShared(JSAtom* source, RegExpFlags flags)
    : CellWithTenuredGCPointer(source), pairCount_(0), flags(flags) {}

void RegExpShared::traceChildren(JSTracer* trc) {
  // Discard code to avoid holding onto ExecutablePools.
  if (IsMarkingTrace(trc) && trc->runtime()->gc.isShrinkingGC()) {
    discardJitCode();
  }

  TraceNullableCellHeaderEdge(trc, this, "RegExpShared source");
  if (kind() == RegExpShared::Kind::Atom) {
    TraceNullableEdge(trc, &patternAtom_, "RegExpShared pattern atom");
  } else {
    for (auto& comp : compilationArray) {
      TraceNullableEdge(trc, &comp.jitCode, "RegExpShared code");
    }
    TraceNullableEdge(trc, &groupsTemplate_, "RegExpShared groups template");
  }
}

void RegExpShared::discardJitCode() {
  for (auto& comp : compilationArray) {
    comp.jitCode = nullptr;
  }

  // We can also purge the tables used by JIT code.
  tables.clearAndFree();
}

void RegExpShared::finalize(JSFreeOp* fop) {
  for (auto& comp : compilationArray) {
    if (comp.byteCode) {
      size_t length = comp.byteCodeLength();
      fop->free_(this, comp.byteCode, length, MemoryUse::RegExpSharedBytecode);
    }
  }
  if (namedCaptureIndices_) {
    size_t length = numNamedCaptures() * sizeof(uint32_t);
    fop->free_(this, namedCaptureIndices_, length,
               MemoryUse::RegExpSharedNamedCaptureData);
  }
  tables.~JitCodeTables();
}

/* static */
bool RegExpShared::compileIfNecessary(JSContext* cx,
                                      MutableHandleRegExpShared re,
                                      HandleLinearString input,
                                      RegExpShared::CodeKind codeKind) {
  if (codeKind == RegExpShared::CodeKind::Any) {
    // We start by interpreting regexps, then compile them once they are
    // sufficiently hot. For very long input strings, we tier up eagerly.
    codeKind = RegExpShared::CodeKind::Bytecode;
    if (re->markedForTierUp() || input->length() > 1000) {
      codeKind = RegExpShared::CodeKind::Jitcode;
    }
  }

  // Fall back to bytecode if native codegen is not available.
  if (!IsNativeRegExpEnabled() && codeKind == RegExpShared::CodeKind::Jitcode) {
    codeKind = RegExpShared::CodeKind::Bytecode;
  }

  bool needsCompile = false;
  if (re->kind() == RegExpShared::Kind::Unparsed) {
    needsCompile = true;
  }
  if (re->kind() == RegExpShared::Kind::RegExp) {
    if (!re->isCompiled(input->hasLatin1Chars(), codeKind)) {
      needsCompile = true;
    }
  }
  if (needsCompile) {
    return irregexp::CompilePattern(cx, re, input, codeKind);
  }
  return true;
}

/* static */
RegExpRunStatus RegExpShared::execute(JSContext* cx,
                                      MutableHandleRegExpShared re,
                                      HandleLinearString input, size_t start,
                                      VectorMatchPairs* matches) {
  MOZ_ASSERT(matches);

  // TODO: Add tracelogger support

  /* Compile the code at point-of-use. */
  if (!compileIfNecessary(cx, re, input, RegExpShared::CodeKind::Any)) {
    return RegExpRunStatus_Error;
  }

  /*
   * Ensure sufficient memory for output vector.
   * No need to initialize it. The RegExp engine fills them in on a match.
   */
  if (!matches->allocOrExpandArray(re->pairCount())) {
    ReportOutOfMemory(cx);
    return RegExpRunStatus_Error;
  }

  if (re->kind() == RegExpShared::Kind::Atom) {
    return RegExpShared::executeAtom(re, input, start, matches);
  }

  /*
   * Ensure sufficient memory for output vector.
   * No need to initialize it. The RegExp engine fills them in on a match.
   */
  if (!matches->allocOrExpandArray(re->pairCount())) {
    ReportOutOfMemory(cx);
    return RegExpRunStatus_Error;
  }

  uint32_t interruptRetries = 0;
  const uint32_t maxInterruptRetries = 4;
  do {
    DebugOnly<bool> alreadyThrowing = cx->isExceptionPending();
    RegExpRunStatus result = irregexp::Execute(cx, re, input, start, matches);
#ifdef DEBUG
    // Check if we must simulate the interruption
    if (js::irregexp::IsolateShouldSimulateInterrupt(cx->isolate)) {
      js::irregexp::IsolateClearShouldSimulateInterrupt(cx->isolate);
      cx->requestInterrupt(InterruptReason::CallbackUrgent);
    }
#endif
    if (result == RegExpRunStatus_Error) {
      /* Execute can return RegExpRunStatus_Error:
       *
       *  1. If the native stack overflowed
       *  2. If the backtrack stack overflowed
       *  3. If an interrupt was requested during execution.
       *
       * In the first two cases, we want to throw an error. In the
       * third case, we want to handle the interrupt and try again.
       * We cap the number of times we will retry.
       */
      if (cx->isExceptionPending()) {
        // If this regexp is being executed by recovery instructions
        // while bailing out to handle an exception, there may already
        // be an exception pending. If so, just return that exception
        // instead of reporting a new one.
        MOZ_ASSERT(alreadyThrowing);
        return RegExpRunStatus_Error;
      }
      if (cx->hasAnyPendingInterrupt()) {
        if (!CheckForInterrupt(cx)) {
          return RegExpRunStatus_Error;
        }
        if (interruptRetries++ < maxInterruptRetries) {
          // The initial execution may have been interpreted, or the
          // interrupt may have triggered a GC that discarded jitcode.
          // To maximize the chance of succeeding before being
          // interrupted again, we want to ensure we are compiled.
          if (!compileIfNecessary(cx, re, input,
                                  RegExpShared::CodeKind::Jitcode)) {
            return RegExpRunStatus_Error;
          }
          continue;
        }
      }
      // If we have run out of retries, this regexp takes too long to execute.
      ReportOverRecursed(cx);
      return RegExpRunStatus_Error;
    }

    MOZ_ASSERT(result == RegExpRunStatus_Success ||
               result == RegExpRunStatus_Success_NotFound);

    return result;
  } while (true);

  MOZ_CRASH("Unreachable");
}

void RegExpShared::useAtomMatch(HandleAtom pattern) {
  MOZ_ASSERT(kind() == RegExpShared::Kind::Unparsed);
  kind_ = RegExpShared::Kind::Atom;
  patternAtom_ = pattern;
  pairCount_ = 1;
}

void RegExpShared::useRegExpMatch(size_t pairCount) {
  MOZ_ASSERT(kind() == RegExpShared::Kind::Unparsed);
  kind_ = RegExpShared::Kind::RegExp;
  pairCount_ = pairCount;
  ticks_ = jit::JitOptions.regexpWarmUpThreshold;
}

/* static */
bool RegExpShared::initializeNamedCaptures(JSContext* cx, HandleRegExpShared re,
                                           HandleNativeObject namedCaptures) {
  MOZ_ASSERT(!re->groupsTemplate_);
  MOZ_ASSERT(!re->namedCaptureIndices_);

  // The irregexp parser returns named capture information in the form
  // of an ArrayObject, where even elements store the capture name and
  // odd elements store the corresponding capture index. We create a
  // template object with a property for each capture name, and store
  // the capture indices as a heap-allocated array.
  MOZ_ASSERT(namedCaptures->getDenseInitializedLength() % 2 == 0);
  uint32_t numNamedCaptures = namedCaptures->getDenseInitializedLength() / 2;

  // Create a plain template object.
  RootedPlainObject templateObject(
      cx, NewTenuredObjectWithGivenProto<PlainObject>(cx, nullptr));
  if (!templateObject) {
    return false;
  }

  // Initialize the properties of the template.
  RootedId id(cx);
  RootedValue dummyString(cx, StringValue(cx->runtime()->emptyString));
  for (uint32_t i = 0; i < numNamedCaptures; i++) {
    JSString* name = namedCaptures->getDenseElement(i * 2).toString();
    id = NameToId(name->asAtom().asPropertyName());
    if (!NativeDefineDataProperty(cx, templateObject, id, dummyString,
                                  JSPROP_ENUMERATE)) {
      return false;
    }
  }

  // Allocate the capture index array.
  uint32_t arraySize = numNamedCaptures * sizeof(uint32_t);
  uint32_t* captureIndices = static_cast<uint32_t*>(js_malloc(arraySize));
  if (!captureIndices) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  // Populate the capture index array
  for (uint32_t i = 0; i < numNamedCaptures; i++) {
    captureIndices[i] = namedCaptures->getDenseElement(i * 2 + 1).toInt32();
  }

  re->numNamedCaptures_ = numNamedCaptures;
  re->groupsTemplate_ = templateObject;
  re->namedCaptureIndices_ = captureIndices;
  js::AddCellMemory(re, arraySize, MemoryUse::RegExpSharedNamedCaptureData);
  return true;
}

void RegExpShared::tierUpTick() {
  MOZ_ASSERT(kind() == RegExpShared::Kind::RegExp);
  if (ticks_ > 0) {
    ticks_--;
  }
}

bool RegExpShared::markedForTierUp() const {
  if (!IsNativeRegExpEnabled()) {
    return false;
  }
  if (kind() != RegExpShared::Kind::RegExp) {
    return false;
  }
  return ticks_ == 0;
}

static RegExpRunStatus ExecuteAtomImpl(RegExpShared* re, JSLinearString* input,
                                       size_t start, MatchPairs* matches) {
  MOZ_ASSERT(re->pairCount() == 1);
  size_t length = input->length();
  size_t searchLength = re->patternAtom()->length();

  if (re->sticky()) {
    // First part checks size_t overflow.
    if (searchLength + start < searchLength || searchLength + start > length) {
      return RegExpRunStatus_Success_NotFound;
    }
    if (!HasSubstringAt(input, re->patternAtom(), start)) {
      return RegExpRunStatus_Success_NotFound;
    }

    (*matches)[0].start = start;
    (*matches)[0].limit = start + searchLength;
    matches->checkAgainst(input->length());
    return RegExpRunStatus_Success;
  }

  int res = StringFindPattern(input, re->patternAtom(), start);
  if (res == -1) {
    return RegExpRunStatus_Success_NotFound;
  }

  (*matches)[0].start = res;
  (*matches)[0].limit = res + searchLength;
  matches->checkAgainst(input->length());
  return RegExpRunStatus_Success;
}

RegExpRunStatus js::ExecuteRegExpAtomRaw(RegExpShared* re,
                                         JSLinearString* input, size_t start,
                                         MatchPairs* matchPairs) {
  AutoUnsafeCallWithABI unsafe;
  return ExecuteAtomImpl(re, input, start, matchPairs);
}

/* static */
RegExpRunStatus RegExpShared::executeAtom(MutableHandleRegExpShared re,
                                          HandleLinearString input,
                                          size_t start,
                                          VectorMatchPairs* matches) {
  return ExecuteAtomImpl(re, input, start, matches);
}

size_t RegExpShared::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  size_t n = 0;

  for (const auto& compilation : compilationArray) {
    if (compilation.byteCode) {
      n += mallocSizeOf(compilation.byteCode);
    }
  }

  n += tables.sizeOfExcludingThis(mallocSizeOf);
  for (size_t i = 0; i < tables.length(); i++) {
    n += mallocSizeOf(tables[i].get());
  }

  return n;
}

/* RegExpRealm */

RegExpRealm::RegExpRealm()
    : optimizableRegExpPrototypeShape_(nullptr),
      optimizableRegExpInstanceShape_(nullptr) {
  for (auto& templateObj : matchResultTemplateObjects_) {
    templateObj = nullptr;
  }
}

ArrayObject* RegExpRealm::createMatchResultTemplateObject(
    JSContext* cx, ResultTemplateKind kind) {
  MOZ_ASSERT(!matchResultTemplateObjects_[kind]);

  /* Create template array object */
  RootedArrayObject templateObject(
      cx, NewDenseUnallocatedArray(cx, RegExpObject::MaxPairCount, nullptr,
                                   TenuredObject));
  if (!templateObject) {
    return nullptr;
  }

  if (kind == ResultTemplateKind::Indices) {
    /* The |indices| array only has a |groups| property. */
    RootedValue groupsVal(cx, UndefinedValue());
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().groups,
                                  groupsVal, JSPROP_ENUMERATE)) {
      return nullptr;
    }
    MOZ_ASSERT(templateObject->getLastProperty().slot() == IndicesGroupsSlot);

    matchResultTemplateObjects_[kind].set(templateObject);
    return matchResultTemplateObjects_[kind];
  }

  /* Set dummy index property */
  RootedValue index(cx, Int32Value(0));
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().index, index,
                                JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectIndexSlot);

  /* Set dummy input property */
  RootedValue inputVal(cx, StringValue(cx->runtime()->emptyString));
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().input, inputVal,
                                JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectInputSlot);

  /* Set dummy groups property */
  RootedValue groupsVal(cx, UndefinedValue());
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().groups,
                                groupsVal, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectGroupsSlot);

  if (kind == ResultTemplateKind::WithIndices) {
    /* Set dummy indices property */
    RootedValue indicesVal(cx, UndefinedValue());
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().indices,
                                  indicesVal, JSPROP_ENUMERATE)) {
      return nullptr;
    }
    MOZ_ASSERT(templateObject->getLastProperty().slot() ==
               MatchResultObjectIndicesSlot);
  }

  matchResultTemplateObjects_[kind].set(templateObject);

  return matchResultTemplateObjects_[kind];
}

void RegExpRealm::traceWeak(JSTracer* trc) {
  for (auto& templateObject : matchResultTemplateObjects_) {
    if (templateObject) {
      TraceWeakEdge(trc, &templateObject,
                    "RegExpRealm::matchResultTemplateObject_");
    }
  }

  if (optimizableRegExpPrototypeShape_) {
    TraceWeakEdge(trc, &optimizableRegExpPrototypeShape_,
                  "RegExpRealm::optimizableRegExpPrototypeShape_");
  }

  if (optimizableRegExpInstanceShape_) {
    TraceWeakEdge(trc, &optimizableRegExpInstanceShape_,
                  "RegExpRealm::optimizableRegExpInstanceShape_");
  }
}

RegExpShared* RegExpZone::get(JSContext* cx, HandleAtom source,
                              RegExpFlags flags) {
  DependentAddPtr<Set> p(cx, set_, Key(source, flags));
  if (p) {
    return *p;
  }

  auto shared = Allocate<RegExpShared>(cx);
  if (!shared) {
    return nullptr;
  }

  new (shared) RegExpShared(source, flags);

  if (!p.add(cx, set_, Key(source, flags), shared)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return shared;
}

size_t RegExpZone::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(this) + set_.sizeOfExcludingThis(mallocSizeOf);
}

RegExpZone::RegExpZone(Zone* zone) : set_(zone, zone) {}

/* Functions */

JSObject* js::CloneRegExpObject(JSContext* cx, Handle<RegExpObject*> regex) {
  // Unlike RegExpAlloc, all clones must use |regex|'s group.
  Rooted<TaggedProto> proto(cx, regex->staticPrototype());
  Rooted<RegExpObject*> clone(
      cx, NewObjectWithGivenTaggedProto<RegExpObject>(cx, proto));
  if (!clone) {
    return nullptr;
  }

  clone->clearShared();

  clone->setShape(regex->shape());

  RegExpShared* shared = RegExpObject::getShared(cx, regex);
  if (!shared) {
    return nullptr;
  }

  clone->initAndZeroLastIndex(shared->getSource(), shared->getFlags(), cx);
  clone->setShared(shared);

  return clone;
}

template <typename CharT>
static bool ParseRegExpFlags(const CharT* chars, size_t length,
                             RegExpFlags* flagsOut, char16_t* invalidFlag) {
  *flagsOut = RegExpFlag::NoFlags;

  for (size_t i = 0; i < length; i++) {
    uint8_t flag;
    switch (chars[i]) {
      case 'd':
        flag = RegExpFlag::HasIndices;
        break;
      case 'g':
        flag = RegExpFlag::Global;
        break;
      case 'i':
        flag = RegExpFlag::IgnoreCase;
        break;
      case 'm':
        flag = RegExpFlag::Multiline;
        break;
      case 's':
        flag = RegExpFlag::DotAll;
        break;
      case 'u':
        flag = RegExpFlag::Unicode;
        break;
      case 'y':
        flag = RegExpFlag::Sticky;
        break;
      default:
        *invalidFlag = chars[i];
        return false;
    }
    if (*flagsOut & flag) {
      *invalidFlag = chars[i];
      return false;
    }
    *flagsOut |= flag;
  }

  return true;
}

bool js::ParseRegExpFlags(JSContext* cx, JSString* flagStr,
                          RegExpFlags* flagsOut) {
  JSLinearString* linear = flagStr->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  size_t len = linear->length();

  bool ok;
  char16_t invalidFlag;
  if (linear->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    ok = ::ParseRegExpFlags(linear->latin1Chars(nogc), len, flagsOut,
                            &invalidFlag);
  } else {
    AutoCheckCannotGC nogc;
    ok = ::ParseRegExpFlags(linear->twoByteChars(nogc), len, flagsOut,
                            &invalidFlag);
  }

  if (!ok) {
    JS::TwoByteChars range(&invalidFlag, 1);
    UniqueChars utf8(JS::CharsToNewUTF8CharsZ(cx, range).c_str());
    if (!utf8) {
      return false;
    }
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_BAD_REGEXP_FLAG, utf8.get());
    return false;
  }

  return true;
}

template <XDRMode mode>
XDRResult js::XDRScriptRegExpObject(XDRState<mode>* xdr,
                                    MutableHandle<RegExpObject*> objp) {
  /* NB: Keep this in sync with CloneScriptRegExpObject. */

  RootedAtom source(xdr->cx());
  uint8_t flags = 0;

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(objp);
    RegExpObject& reobj = *objp;
    source = reobj.getSource();
    flags = reobj.getFlags().value();
  }
  MOZ_TRY(XDRAtom(xdr, &source));
  MOZ_TRY(xdr->codeUint8(&flags));
  if (mode == XDR_DECODE) {
    RegExpObject* reobj = RegExpObject::create(
        xdr->cx(), source, RegExpFlags(flags), TenuredObject);
    if (!reobj) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    objp.set(reobj);
  }
  return Ok();
}

template XDRResult js::XDRScriptRegExpObject(XDRState<XDR_ENCODE>* xdr,
                                             MutableHandle<RegExpObject*> objp);

template XDRResult js::XDRScriptRegExpObject(XDRState<XDR_DECODE>* xdr,
                                             MutableHandle<RegExpObject*> objp);

JSObject* js::CloneScriptRegExpObject(JSContext* cx, RegExpObject& reobj) {
  /* NB: Keep this in sync with XDRScriptRegExpObject. */

  RootedAtom source(cx, reobj.getSource());
  cx->markAtom(source);

  return RegExpObject::create(cx, source, reobj.getFlags(), TenuredObject);
}

JS::ubi::Node::Size JS::ubi::Concrete<RegExpShared>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return js::gc::Arena::thingSize(gc::AllocKind::REGEXP_SHARED) +
         get().sizeOfExcludingThis(mallocSizeOf);
}

/*
 * Regular Expressions.
 */
JS_PUBLIC_API JSObject* JS::NewRegExpObject(JSContext* cx, const char* bytes,
                                            size_t length, RegExpFlags flags) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  UniqueTwoByteChars chars(InflateString(cx, bytes, length));
  if (!chars) {
    return nullptr;
  }

  return RegExpObject::create(cx, chars.get(), length, flags, GenericObject);
}

JS_PUBLIC_API JSObject* JS::NewUCRegExpObject(JSContext* cx,
                                              const char16_t* chars,
                                              size_t length,
                                              RegExpFlags flags) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return RegExpObject::create(cx, chars, length, flags, GenericObject);
}

JS_PUBLIC_API bool JS::SetRegExpInput(JSContext* cx, HandleObject obj,
                                      HandleString input) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(input);

  Handle<GlobalObject*> global = obj.as<GlobalObject>();
  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, global);
  if (!res) {
    return false;
  }

  res->reset(input);
  return true;
}

JS_PUBLIC_API bool JS::ClearRegExpStatics(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_ASSERT(obj);

  Handle<GlobalObject*> global = obj.as<GlobalObject>();
  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, global);
  if (!res) {
    return false;
  }

  res->clear();
  return true;
}

JS_PUBLIC_API bool JS::ExecuteRegExp(JSContext* cx, HandleObject obj,
                                     HandleObject reobj, const char16_t* chars,
                                     size_t length, size_t* indexp, bool test,
                                     MutableHandleValue rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  Handle<GlobalObject*> global = obj.as<GlobalObject>();
  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, global);
  if (!res) {
    return false;
  }

  RootedLinearString input(cx, NewStringCopyN<CanGC>(cx, chars, length));
  if (!input) {
    return false;
  }

  return ExecuteRegExpLegacy(cx, res, reobj.as<RegExpObject>(), input, indexp,
                             test, rval);
}

JS_PUBLIC_API bool JS::ExecuteRegExpNoStatics(JSContext* cx, HandleObject obj,
                                              const char16_t* chars,
                                              size_t length, size_t* indexp,
                                              bool test,
                                              MutableHandleValue rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  RootedLinearString input(cx, NewStringCopyN<CanGC>(cx, chars, length));
  if (!input) {
    return false;
  }

  return ExecuteRegExpLegacy(cx, nullptr, obj.as<RegExpObject>(), input, indexp,
                             test, rval);
}

JS_PUBLIC_API bool JS::ObjectIsRegExp(JSContext* cx, HandleObject obj,
                                      bool* isRegExp) {
  cx->check(obj);

  ESClass cls;
  if (!GetBuiltinClass(cx, obj, &cls)) {
    return false;
  }

  *isRegExp = cls == ESClass::RegExp;
  return true;
}

JS_PUBLIC_API RegExpFlags JS::GetRegExpFlags(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  RegExpShared* shared = RegExpToShared(cx, obj);
  if (!shared) {
    return RegExpFlag::NoFlags;
  }
  return shared->getFlags();
}

JS_PUBLIC_API JSString* JS::GetRegExpSource(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  RegExpShared* shared = RegExpToShared(cx, obj);
  if (!shared) {
    return nullptr;
  }
  return shared->getSource();
}

JS_PUBLIC_API bool JS::CheckRegExpSyntax(JSContext* cx, const char16_t* chars,
                                         size_t length, RegExpFlags flags,
                                         MutableHandleValue error) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  CompileOptions dummyOptions(cx);
  frontend::DummyTokenStream dummyTokenStream(cx, dummyOptions);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());

  mozilla::Range<const char16_t> source(chars, length);
  bool success =
      irregexp::CheckPatternSyntax(cx, dummyTokenStream, source, flags);
  error.set(UndefinedValue());
  if (!success) {
    // We can fail because of OOM or over-recursion even if the syntax is valid.
    if (cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed()) {
      return false;
    }
    if (!cx->getPendingException(error)) {
      return false;
    }
    cx->clearPendingException();
  }
  return true;
}
