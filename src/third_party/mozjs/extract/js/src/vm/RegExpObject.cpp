/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RegExpObject.h"

#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include <type_traits>

#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"  // REGEXP_*_FLAG
#include "frontend/FrontendContext.h"    // AutoReportFrontendContext
#include "frontend/TokenStream.h"
#include "gc/HashUtil.h"
#include "irregexp/RegExpAPI.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::ReportOverRecursed
#include "js/Object.h"                // JS::GetBuiltinClass
#include "js/Printer.h"               // js::GenericPrinter
#include "js/RegExp.h"
#include "js/RegExpFlags.h"  // JS::RegExpFlags
#include "util/StringBuffer.h"
#include "util/Unicode.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/MatchPairs.h"
#include "vm/PlainObject.h"
#include "vm/RegExpStatics.h"
#include "vm/StringType.h"

#include "vm/JSContext-inl.h"
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
static_assert(RegExpFlag::UnicodeSets == REGEXP_UNICODESETS_FLAG,
              "self-hosted JS and /v flag bits must agree");
static_assert(RegExpFlag::Sticky == REGEXP_STICKY_FLAG,
              "self-hosted JS and /y flag bits must agree");

RegExpObject* js::RegExpAlloc(JSContext* cx, NewObjectKind newKind,
                              HandleObject proto /* = nullptr */) {
  Rooted<RegExpObject*> regexp(
      cx, NewObjectWithClassProtoAndKind<RegExpObject>(cx, proto, newKind));
  if (!regexp) {
    return nullptr;
  }

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
  if (native == regexp_unicodeSets) {
    *mask = RegExpFlag::UnicodeSets;
    return true;
  }

  return false;
}

static bool FinishRegExpClassInit(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto) {
#ifdef DEBUG
  // Assert RegExp.prototype.exec is usually stored in a dynamic slot. The
  // optimization in InlinableNativeIRGenerator::tryAttachIntrinsicRegExpExec
  // depends on this.
  Handle<NativeObject*> nproto = proto.as<NativeObject>();
  auto prop = nproto->lookupPure(cx->names().exec);
  MOZ_ASSERT(prop->isDataProperty());
  MOZ_ASSERT(!nproto->isFixedSlot(prop->slot()));
#endif
  return true;
}

static const ClassSpec RegExpObjectClassSpec = {
    GenericCreateConstructor<js::regexp_construct, 2, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<RegExpObject>,
    nullptr,
    js::regexp_static_props,
    js::regexp_methods,
    js::regexp_properties,
    FinishRegExpClassInit};

const JSClass RegExpObject::class_ = {
    "RegExp",
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

  Rooted<JSAtom*> source(cx, AtomizeChars(cx, chars, length));
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
                                                Handle<JSAtom*> source,
                                                RegExpFlags flags,
                                                NewObjectKind newKind) {
  Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, newKind));
  if (!regexp) {
    return nullptr;
  }

  regexp->initAndZeroLastIndex(source, flags, cx);

  return regexp;
}

RegExpObject* RegExpObject::create(JSContext* cx, Handle<JSAtom*> source,
                                   RegExpFlags flags, NewObjectKind newKind) {
  Rooted<RegExpObject*> regexp(cx);
  {
    AutoReportFrontendContext fc(cx);
    CompileOptions dummyOptions(cx);
    frontend::DummyTokenStream dummyTokenStream(&fc, dummyOptions);

    LifoAllocScope allocScope(&cx->tempLifoAlloc());
    if (!irregexp::CheckPatternSyntax(cx, cx->stackLimitForCurrentPrincipal(),
                                      dummyTokenStream, source, flags)) {
      return nullptr;
    }

    regexp = RegExpAlloc(cx, newKind);
    if (!regexp) {
      return nullptr;
    }

    regexp->initAndZeroLastIndex(source, flags, cx);

    MOZ_ASSERT(!regexp->hasShared());
  }
  return regexp;
}

/* static */
RegExpShared* RegExpObject::createShared(JSContext* cx,
                                         Handle<RegExpObject*> regexp) {
  MOZ_ASSERT(!regexp->hasShared());
  Rooted<JSAtom*> source(cx, regexp->getSource());
  RegExpShared* shared =
      cx->zone()->regExps().get(cx, source, regexp->getFlags());
  if (!shared) {
    return nullptr;
  }

  regexp->setShared(shared);

  MOZ_ASSERT(regexp->hasShared());

  return shared;
}

SharedShape* RegExpObject::assignInitialShape(JSContext* cx,
                                              Handle<RegExpObject*> self) {
  MOZ_ASSERT(self->empty());

  static_assert(LAST_INDEX_SLOT == 0);

  /* The lastIndex property alone is writable but non-configurable. */
  if (!NativeObject::addPropertyInReservedSlot(cx, self, cx->names().lastIndex,
                                               LAST_INDEX_SLOT,
                                               {PropertyFlag::Writable})) {
    return nullptr;
  }

  return self->sharedShape();
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

template <typename KnownF, typename UnknownF>
void ForEachRegExpFlag(JS::RegExpFlags flags, KnownF known, UnknownF unknown) {
  uint8_t raw = flags.value();

  for (uint8_t i = 1; i; i = i << 1) {
    if (!(raw & i)) {
      continue;
    }
    switch (raw & i) {
      case RegExpFlag::HasIndices:
        known("HasIndices", "d");
        break;
      case RegExpFlag::Global:
        known("Global", "g");
        break;
      case RegExpFlag::IgnoreCase:
        known("IgnoreCase", "i");
        break;
      case RegExpFlag::Multiline:
        known("Multiline", "m");
        break;
      case RegExpFlag::DotAll:
        known("DotAll", "s");
        break;
      case RegExpFlag::Unicode:
        known("Unicode", "u");
        break;
      case RegExpFlag::Sticky:
        known("Sticky", "y");
        break;
      default:
        unknown(i);
        break;
    }
  }
}

std::ostream& JS::operator<<(std::ostream& os, RegExpFlags flags) {
  ForEachRegExpFlag(
      flags, [&](const char* name, const char* c) { os << c; },
      [&](uint8_t value) { os << '?'; });
  return os;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void RegExpObject::dumpOwnFields(js::JSONPrinter& json) const {
  {
    js::GenericPrinter& out = json.beginStringProperty("source");
    getSource()->dumpPropertyName(out);
    json.endStringProperty();
  }

  json.beginInlineListProperty("flags");
  ForEachRegExpFlag(
      getFlags(),
      [&](const char* name, const char* c) { json.value("%s", name); },
      [&](uint8_t value) { json.value("Unknown(%02x)", value); });
  json.endInlineList();

  {
    js::GenericPrinter& out = json.beginStringProperty("lastIndex");
    getLastIndex().dumpStringContent(out);
    json.endStringProperty();
  }
}

void RegExpObject::dumpOwnStringContent(js::GenericPrinter& out) const {
  out.put("/");

  getSource()->dumpCharsNoQuote(out);

  out.put("/");

  ForEachRegExpFlag(
      getFlags(), [&](const char* name, const char* c) { out.put(c); },
      [&](uint8_t value) {});
}
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

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
JSLinearString* js::EscapeRegExpPattern(JSContext* cx, Handle<JSAtom*> src) {
  // Step 2.
  if (src->length() == 0) {
    return cx->names().emptyRegExp_;
  }

  // We may never need to use |sb|. Start using it lazily.
  JSStringBuilder sb(cx);
  bool escapeFailed = false;
  if (src->hasLatin1Chars()) {
    JS::AutoCheckCannotGC nogc;
    escapeFailed =
        !::EscapeRegExpPattern(sb, src->latin1Chars(nogc), src->length());
  } else {
    JS::AutoCheckCannotGC nogc;
    escapeFailed =
        !::EscapeRegExpPattern(sb, src->twoByteChars(nogc), src->length());
  }
  if (escapeFailed) {
    return nullptr;
  }

  // Step 3.
  if (sb.empty()) {
    return src;
  }
  return sb.finishString();
}

// ES6 draft rev32 21.2.5.14. Optimized for RegExpObject.
JSLinearString* RegExpObject::toString(JSContext* cx,
                                       Handle<RegExpObject*> obj) {
  // Steps 3-4.
  Rooted<JSAtom*> src(cx, obj->getSource());
  if (!src) {
    return nullptr;
  }
  Rooted<JSLinearString*> escapedSrc(cx, EscapeRegExpPattern(cx, src));

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
  if (obj->unicodeSets() && !sb.append('v')) {
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

void RegExpShared::finalize(JS::GCContext* gcx) {
  for (auto& comp : compilationArray) {
    if (comp.byteCode) {
      size_t length = comp.byteCodeLength();
      gcx->free_(this, comp.byteCode, length, MemoryUse::RegExpSharedBytecode);
    }
  }
  if (namedCaptureIndices_) {
    size_t length = numNamedCaptures() * sizeof(uint32_t);
    gcx->free_(this, namedCaptureIndices_, length,
               MemoryUse::RegExpSharedNamedCaptureData);
  }
  if (namedCaptureSliceIndices_) {
    size_t length = numDistinctNamedCaptures() * sizeof(uint32_t);
    gcx->free_(this, namedCaptureSliceIndices_, length,
               MemoryUse::RegExpSharedNamedCaptureSliceData);
  }
  tables.~JitCodeTables();
}

/* static */
bool RegExpShared::compileIfNecessary(JSContext* cx,
                                      MutableHandleRegExpShared re,
                                      Handle<JSLinearString*> input,
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
                                      Handle<JSLinearString*> input,
                                      size_t start, VectorMatchPairs* matches) {
  MOZ_ASSERT(matches);

  // TODO: Add tracelogger support

  /* Compile the code at point-of-use. */
  if (!compileIfNecessary(cx, re, input, RegExpShared::CodeKind::Any)) {
    return RegExpRunStatus::Error;
  }

  /*
   * Ensure sufficient memory for output vector.
   * No need to initialize it. The RegExp engine fills them in on a match.
   */
  if (!matches->allocOrExpandArray(re->pairCount())) {
    ReportOutOfMemory(cx);
    return RegExpRunStatus::Error;
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
    return RegExpRunStatus::Error;
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
    if (result == RegExpRunStatus::Error) {
      /* Execute can return RegExpRunStatus::Error:
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
        return RegExpRunStatus::Error;
      }
      if (cx->hasAnyPendingInterrupt()) {
        if (!CheckForInterrupt(cx)) {
          return RegExpRunStatus::Error;
        }
        if (interruptRetries++ < maxInterruptRetries) {
          // The initial execution may have been interpreted, or the
          // interrupt may have triggered a GC that discarded jitcode.
          // To maximize the chance of succeeding before being
          // interrupted again, we want to ensure we are compiled.
          if (!compileIfNecessary(cx, re, input,
                                  RegExpShared::CodeKind::Jitcode)) {
            return RegExpRunStatus::Error;
          }
          continue;
        }
      }
      // If we have run out of retries, this regexp takes too long to execute.
      ReportOverRecursed(cx);
      return RegExpRunStatus::Error;
    }

    MOZ_ASSERT(result == RegExpRunStatus::Success ||
               result == RegExpRunStatus::Success_NotFound);

    return result;
  } while (true);

  MOZ_CRASH("Unreachable");
}

void RegExpShared::useAtomMatch(Handle<JSAtom*> pattern) {
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
void RegExpShared::InitializeNamedCaptures(JSContext* cx, HandleRegExpShared re,
                                           uint32_t numNamedCaptures,
                                           uint32_t numDistinctNamedCaptures,
                                           Handle<PlainObject*> templateObject,
                                           uint32_t* captureIndices,
                                           uint32_t* sliceIndices) {
  MOZ_ASSERT(!re->groupsTemplate_);
  MOZ_ASSERT(!re->namedCaptureIndices_);
  MOZ_ASSERT(!re->namedCaptureSliceIndices_);

  re->numNamedCaptures_ = numNamedCaptures;
  re->numDistinctNamedCaptures_ = numDistinctNamedCaptures;
  re->groupsTemplate_ = templateObject;
  re->namedCaptureIndices_ = captureIndices;
  re->namedCaptureSliceIndices_ = sliceIndices;

  uint32_t arraySize = numNamedCaptures * sizeof(uint32_t);
  js::AddCellMemory(re, arraySize, MemoryUse::RegExpSharedNamedCaptureData);

  if (sliceIndices) {
    arraySize = numDistinctNamedCaptures * sizeof(uint32_t);
    js::AddCellMemory(re, arraySize,
                      MemoryUse::RegExpSharedNamedCaptureSliceData);
  }
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

// When either unicode flag is set and if |index| points to a trail surrogate,
// step back to the corresponding lead surrogate.
static size_t StepBackToLeadSurrogate(const JSLinearString* input,
                                      size_t index) {
  // |index| must be a position within a two-byte string, otherwise it can't
  // point to the trail surrogate of a surrogate pair.
  if (index == 0 || index >= input->length() || input->hasLatin1Chars()) {
    return index;
  }

  /*
   * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad
   * 21.2.2.2 step 2.
   *   Let listIndex be the index into Input of the character that was obtained
   *   from element index of str.
   *
   * In the spec, pattern match is performed with decoded Unicode code points,
   * but our implementation performs it with UTF-16 encoded strings. In step 2,
   * we should decrement lastIndex (index) if it points to a trail surrogate
   * that has a corresponding lead surrogate.
   *
   *   var r = /\uD83D\uDC38/ug;
   *   r.lastIndex = 1;
   *   var str = "\uD83D\uDC38";
   *   var result = r.exec(str); // pattern match starts from index 0
   *   print(result.index);      // prints 0
   *
   * Note: This doesn't match the current spec text and result in different
   * values for `result.index` under certain conditions. However, the spec will
   * change to match our implementation's behavior.
   * See https://github.com/tc39/ecma262/issues/128.
   */
  JS::AutoCheckCannotGC nogc;
  const auto* chars = input->twoByteChars(nogc);
  if (unicode::IsTrailSurrogate(chars[index]) &&
      unicode::IsLeadSurrogate(chars[index - 1])) {
    index--;
  }
  return index;
}

static RegExpRunStatus ExecuteAtomImpl(RegExpShared* re, JSLinearString* input,
                                       size_t start, MatchPairs* matches) {
  MOZ_ASSERT(re->pairCount() == 1);
  size_t length = input->length();
  size_t searchLength = re->patternAtom()->length();

  if (re->unicode() || re->unicodeSets()) {
    start = StepBackToLeadSurrogate(input, start);
  }

  if (re->sticky()) {
    // First part checks size_t overflow.
    if (searchLength + start < searchLength || searchLength + start > length) {
      return RegExpRunStatus::Success_NotFound;
    }
    if (!HasSubstringAt(input, re->patternAtom(), start)) {
      return RegExpRunStatus::Success_NotFound;
    }

    (*matches)[0].start = start;
    (*matches)[0].limit = start + searchLength;
    matches->checkAgainst(input->length());
    return RegExpRunStatus::Success;
  }

  int res = StringFindPattern(input, re->patternAtom(), start);
  if (res == -1) {
    return RegExpRunStatus::Success_NotFound;
  }

  (*matches)[0].start = res;
  (*matches)[0].limit = res + searchLength;
  matches->checkAgainst(input->length());
  return RegExpRunStatus::Success;
}

RegExpRunStatus js::ExecuteRegExpAtomRaw(RegExpShared* re,
                                         JSLinearString* input, size_t start,
                                         MatchPairs* matchPairs) {
  AutoUnsafeCallWithABI unsafe;
  return ExecuteAtomImpl(re, input, start, matchPairs);
}

/* static */
RegExpRunStatus RegExpShared::executeAtom(MutableHandleRegExpShared re,
                                          Handle<JSLinearString*> input,
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
  for (auto& shape : matchResultShapes_) {
    shape = nullptr;
  }
}

SharedShape* RegExpRealm::createMatchResultShape(JSContext* cx,
                                                 ResultShapeKind kind) {
  MOZ_ASSERT(!matchResultShapes_[kind]);

  /* Create template array object */
  Rooted<ArrayObject*> templateObject(cx, NewDenseEmptyArray(cx));
  if (!templateObject) {
    return nullptr;
  }

  if (kind == ResultShapeKind::Indices) {
    /* The |indices| array only has a |groups| property. */
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().groups,
                                  UndefinedHandleValue, JSPROP_ENUMERATE)) {
      return nullptr;
    }
    MOZ_ASSERT(templateObject->getLastProperty().slot() == IndicesGroupsSlot);

    matchResultShapes_[kind].set(templateObject->sharedShape());
    return matchResultShapes_[kind];
  }

  /* Set dummy index property */
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().index,
                                UndefinedHandleValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectIndexSlot);

  /* Set dummy input property */
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().input,
                                UndefinedHandleValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectInputSlot);

  /* Set dummy groups property */
  if (!NativeDefineDataProperty(cx, templateObject, cx->names().groups,
                                UndefinedHandleValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  MOZ_ASSERT(templateObject->getLastProperty().slot() ==
             MatchResultObjectGroupsSlot);

  if (kind == ResultShapeKind::WithIndices) {
    /* Set dummy indices property */
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().indices,
                                  UndefinedHandleValue, JSPROP_ENUMERATE)) {
      return nullptr;
    }
    MOZ_ASSERT(templateObject->getLastProperty().slot() ==
               MatchResultObjectIndicesSlot);
  }

#ifdef DEBUG
  if (kind == ResultShapeKind::Normal) {
    MOZ_ASSERT(templateObject->numFixedSlots() == 0);
    MOZ_ASSERT(templateObject->numDynamicSlots() ==
               MatchResultObjectNumDynamicSlots);
    MOZ_ASSERT(templateObject->slotSpan() == MatchResultObjectSlotSpan);
  }
#endif

  matchResultShapes_[kind].set(templateObject->sharedShape());

  return matchResultShapes_[kind];
}

void RegExpRealm::trace(JSTracer* trc) {
  if (regExpStatics) {
    regExpStatics->trace(trc);
  }

  for (auto& shape : matchResultShapes_) {
    TraceNullableEdge(trc, &shape, "RegExpRealm::matchResultShapes_");
  }

  TraceNullableEdge(trc, &optimizableRegExpPrototypeShape_,
                    "RegExpRealm::optimizableRegExpPrototypeShape_");

  TraceNullableEdge(trc, &optimizableRegExpInstanceShape_,
                    "RegExpRealm::optimizableRegExpInstanceShape_");
}

RegExpShared* RegExpZone::get(JSContext* cx, Handle<JSAtom*> source,
                              RegExpFlags flags) {
  DependentAddPtr<Set> p(cx, set_, Key(source, flags));
  if (p) {
    return *p;
  }

  auto* shared = cx->newCell<RegExpShared>(source, flags);
  if (!shared) {
    return nullptr;
  }

  if (!p.add(cx, set_, Key(source, flags), shared)) {
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
  constexpr gc::AllocKind allocKind = RegExpObject::AllocKind;
  static_assert(gc::GetGCKindSlots(allocKind) == RegExpObject::RESERVED_SLOTS);
  MOZ_ASSERT(regex->asTenured().getAllocKind() == allocKind);

  Rooted<SharedShape*> shape(cx, regex->sharedShape());
  Rooted<RegExpObject*> clone(cx, NativeObject::create<RegExpObject>(
                                      cx, allocKind, gc::Heap::Default, shape));
  if (!clone) {
    return nullptr;
  }

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
    if (!JS::MaybeParseRegExpFlag(chars[i], &flag) || *flagsOut & flag) {
      *invalidFlag = chars[i];
      return false;
    }

    // /u and /v flags are mutually exclusive.
    if (((*flagsOut & RegExpFlag::Unicode) &&
         (flag & RegExpFlag::UnicodeSets)) ||
        ((*flagsOut & RegExpFlag::UnicodeSets) &&
         (flag & RegExpFlag::Unicode))) {
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

  Rooted<JSLinearString*> input(cx, NewStringCopyN<CanGC>(cx, chars, length));
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

  Rooted<JSLinearString*> input(cx, NewStringCopyN<CanGC>(cx, chars, length));
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

  AutoReportFrontendContext fc(cx);
  CompileOptions dummyOptions(cx);
  frontend::DummyTokenStream dummyTokenStream(&fc, dummyOptions);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());

  mozilla::Range<const char16_t> source(chars, length);
  bool success = irregexp::CheckPatternSyntax(
      cx->tempLifoAlloc(), cx->stackLimitForCurrentPrincipal(),
      dummyTokenStream, source, flags);
  error.set(UndefinedValue());
  if (!success) {
    if (!fc.convertToRuntimeErrorAndClear()) {
      return false;
    }
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
