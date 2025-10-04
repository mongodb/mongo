/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/StringType-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Latin1.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"
#include "mozilla/Vector.h"

#include <algorithm>    // std::{all_of,copy_n,enable_if,is_const,move}
#include <iterator>     // std::size
#include <type_traits>  // std::is_same, std::is_unsigned

#include "jsfriendapi.h"
#include "jsnum.h"

#include "builtin/Boolean.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "builtin/RecordObject.h"
#endif
#include "gc/AllocKind.h"
#include "gc/MaybeRooted.h"
#include "gc/Nursery.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printer.h"               // js::GenericPrinter
#include "js/PropertyAndElement.h"    // JS_DefineElement
#include "js/SourceText.h"            // JS::SourceText
#include "js/StableStringChars.h"
#include "js/UbiNode.h"
#include "util/Identifier.h"  // js::IsIdentifierNameOrPrivateName
#include "util/Unicode.h"
#include "vm/GeckoProfiler.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/StaticStrings.h"
#include "vm/ToSource.h"  // js::ValueToSource

#include "gc/Marking-inl.h"
#include "vm/GeckoProfiler-inl.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

using namespace js;

using mozilla::AsWritableChars;
using mozilla::ConvertLatin1toUtf16;
using mozilla::IsAsciiDigit;
using mozilla::IsUtf16Latin1;
using mozilla::LossyConvertUtf16toLatin1;
using mozilla::PodCopy;
using mozilla::RangedPtr;
using mozilla::RoundUpPow2;
using mozilla::Span;

using JS::AutoCheckCannotGC;
using JS::AutoStableStringChars;

using UniqueLatin1Chars = UniquePtr<Latin1Char[], JS::FreePolicy>;

size_t JSString::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  // JSRope: do nothing, we'll count all children chars when we hit the leaf
  // strings.
  if (isRope()) {
    return 0;
  }

  MOZ_ASSERT(isLinear());

  // JSDependentString: do nothing, we'll count the chars when we hit the base
  // string.
  if (isDependent()) {
    return 0;
  }

  // JSExternalString: Ask the embedding to tell us what's going on.
  if (isExternal()) {
    // Our callback isn't supposed to cause GC.
    JS::AutoSuppressGCAnalysis nogc;
    JSExternalString& external = asExternal();
    if (external.hasLatin1Chars()) {
      return asExternal().callbacks()->sizeOfBuffer(external.latin1Chars(),
                                                    mallocSizeOf);
    } else {
      return asExternal().callbacks()->sizeOfBuffer(external.twoByteChars(),
                                                    mallocSizeOf);
    }
  }

  // JSExtensibleString: count the full capacity, not just the used space.
  if (isExtensible()) {
    JSExtensibleString& extensible = asExtensible();
    return extensible.hasLatin1Chars()
               ? mallocSizeOf(extensible.rawLatin1Chars())
               : mallocSizeOf(extensible.rawTwoByteChars());
  }

  // JSInlineString, JSFatInlineString, js::ThinInlineAtom, js::FatInlineAtom:
  // the chars are inline.
  if (isInline()) {
    return 0;
  }

  // Chars in the nursery are owned by the nursery.
  if (!ownsMallocedChars()) {
    return 0;
  }

  // Everything else: measure the space for the chars.
  JSLinearString& linear = asLinear();
  return linear.hasLatin1Chars() ? mallocSizeOf(linear.rawLatin1Chars())
                                 : mallocSizeOf(linear.rawTwoByteChars());
}

JS::ubi::Node::Size JS::ubi::Concrete<JSString>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  JSString& str = get();
  size_t size;
  if (str.isAtom()) {
    if (str.isInline()) {
      size = str.isFatInline() ? sizeof(js::FatInlineAtom)
                               : sizeof(js::ThinInlineAtom);
    } else {
      size = sizeof(js::NormalAtom);
    }
  } else {
    size = str.isFatInline() ? sizeof(JSFatInlineString) : sizeof(JSString);
  }

  if (IsInsideNursery(&str)) {
    size += Nursery::nurseryCellHeaderSize();
  }

  size += str.sizeOfExcludingThis(mallocSizeOf);

  return size;
}

const char16_t JS::ubi::Concrete<JSString>::concreteTypeName[] = u"JSString";

mozilla::Maybe<std::tuple<size_t, size_t>> JSString::encodeUTF8Partial(
    const JS::AutoRequireNoGC& nogc, mozilla::Span<char> buffer) const {
  mozilla::Vector<const JSString*, 16, SystemAllocPolicy> stack;
  const JSString* current = this;
  char16_t pendingLeadSurrogate = 0;  // U+0000 means no pending lead surrogate
  size_t totalRead = 0;
  size_t totalWritten = 0;
  for (;;) {
    if (current->isRope()) {
      JSRope& rope = current->asRope();
      if (!stack.append(rope.rightChild())) {
        // OOM
        return mozilla::Nothing();
      }
      current = rope.leftChild();
      continue;
    }

    JSLinearString& linear = current->asLinear();
    if (MOZ_LIKELY(linear.hasLatin1Chars())) {
      if (MOZ_UNLIKELY(pendingLeadSurrogate)) {
        if (buffer.Length() < 3) {
          return mozilla::Some(std::make_tuple(totalRead, totalWritten));
        }
        buffer[0] = '\xEF';
        buffer[1] = '\xBF';
        buffer[2] = '\xBD';
        buffer = buffer.From(3);
        totalRead += 1;  // pendingLeadSurrogate
        totalWritten += 3;
        pendingLeadSurrogate = 0;
      }
      auto src = mozilla::AsChars(
          mozilla::Span(linear.latin1Chars(nogc), linear.length()));
      size_t read;
      size_t written;
      std::tie(read, written) =
          mozilla::ConvertLatin1toUtf8Partial(src, buffer);
      buffer = buffer.From(written);
      totalRead += read;
      totalWritten += written;
      if (read < src.Length()) {
        return mozilla::Some(std::make_tuple(totalRead, totalWritten));
      }
    } else {
      auto src = mozilla::Span(linear.twoByteChars(nogc), linear.length());
      if (MOZ_UNLIKELY(pendingLeadSurrogate)) {
        char16_t first = 0;
        if (!src.IsEmpty()) {
          first = src[0];
        }
        if (unicode::IsTrailSurrogate(first)) {
          // Got a surrogate pair
          if (buffer.Length() < 4) {
            return mozilla::Some(std::make_tuple(totalRead, totalWritten));
          }
          uint32_t astral = unicode::UTF16Decode(pendingLeadSurrogate, first);
          buffer[0] = char(0b1111'0000 | (astral >> 18));
          buffer[1] = char(0b1000'0000 | ((astral >> 12) & 0b11'1111));
          buffer[2] = char(0b1000'0000 | ((astral >> 6) & 0b11'1111));
          buffer[3] = char(0b1000'0000 | (astral & 0b11'1111));
          src = src.From(1);
          buffer = buffer.From(4);
          totalRead += 2;  // both pendingLeadSurrogate and first!
          totalWritten += 4;
        } else {
          // unpaired surrogate
          if (buffer.Length() < 3) {
            return mozilla::Some(std::make_tuple(totalRead, totalWritten));
          }
          buffer[0] = '\xEF';
          buffer[1] = '\xBF';
          buffer[2] = '\xBD';
          buffer = buffer.From(3);
          totalRead += 1;  // pendingLeadSurrogate
          totalWritten += 3;
        }
        pendingLeadSurrogate = 0;
      }
      if (!src.IsEmpty()) {
        char16_t last = src[src.Length() - 1];
        if (unicode::IsLeadSurrogate(last)) {
          src = src.To(src.Length() - 1);
          pendingLeadSurrogate = last;
        } else {
          MOZ_ASSERT(!pendingLeadSurrogate);
        }
        size_t read;
        size_t written;
        std::tie(read, written) =
            mozilla::ConvertUtf16toUtf8Partial(src, buffer);
        buffer = buffer.From(written);
        totalRead += read;
        totalWritten += written;
        if (read < src.Length()) {
          return mozilla::Some(std::make_tuple(totalRead, totalWritten));
        }
      }
    }
    if (stack.empty()) {
      break;
    }
    current = stack.popCopy();
  }
  if (MOZ_UNLIKELY(pendingLeadSurrogate)) {
    if (buffer.Length() < 3) {
      return mozilla::Some(std::make_tuple(totalRead, totalWritten));
    }
    buffer[0] = '\xEF';
    buffer[1] = '\xBF';
    buffer[2] = '\xBD';
    // No need to update buffer and pendingLeadSurrogate anymore
    totalRead += 1;
    totalWritten += 3;
  }
  return mozilla::Some(std::make_tuple(totalRead, totalWritten));
}

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
template <typename CharT>
/*static */
void JSString::dumpCharsNoQuote(const CharT* s, size_t n,
                                js::GenericPrinter& out) {
  for (size_t i = 0; i < n; i++) {
    char16_t c = s[i];
    if (c == '"') {
      out.put("\\\"");
    } else if (c == '\'') {
      out.put("\\'");
    } else if (c == '`') {
      out.put("\\`");
    } else if (c == '\\') {
      out.put("\\\\");
    } else if (c == '\r') {
      out.put("\\r");
    } else if (c == '\n') {
      out.put("\\n");
    } else if (c == '\t') {
      out.put("\\t");
    } else if (c >= 32 && c < 127) {
      out.putChar((char)s[i]);
    } else if (c <= 255) {
      out.printf("\\x%02x", unsigned(c));
    } else {
      out.printf("\\u%04x", unsigned(c));
    }
  }
}

/* static */
template void JSString::dumpCharsNoQuote(const Latin1Char* s, size_t n,
                                         js::GenericPrinter& out);

/* static */
template void JSString::dumpCharsNoQuote(const char16_t* s, size_t n,
                                         js::GenericPrinter& out);

void JSString::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}

void JSString::dump(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void JSString::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

const char* RepresentationToString(const JSString* s) {
  if (s->isAtom()) {
    return "JSAtom";
  }

  if (s->isLinear()) {
    if (s->isDependent()) {
      return "JSDependentString";
    }
    if (s->isExternal()) {
      return "JSExternalString";
    }
    if (s->isExtensible()) {
      return "JSExtensibleString";
    }

    if (s->isInline()) {
      if (s->isFatInline()) {
        return "JSFatInlineString";
      }
      return "JSThinInlineString";
    }

    return "JSLinearString";
  }

  if (s->isRope()) {
    return "JSRope";
  }

  return "JSString";
}

template <typename KnownF, typename UnknownF>
void ForEachStringFlag(const JSString* str, uint32_t flags, KnownF known,
                       UnknownF unknown) {
  for (uint32_t i = js::Bit(3); i < js::Bit(17); i = i << 1) {
    if (!(flags & i)) {
      continue;
    }
    switch (i) {
      case JSString::ATOM_BIT:
        known("ATOM_BIT");
        break;
      case JSString::LINEAR_BIT:
        known("LINEAR_BIT");
        break;
      case JSString::DEPENDENT_BIT:
        known("DEPENDENT_BIT");
        break;
      case JSString::INLINE_CHARS_BIT:
        known("INLINE_BIT");
        break;
      case JSString::LINEAR_IS_EXTENSIBLE_BIT:
        static_assert(JSString::LINEAR_IS_EXTENSIBLE_BIT ==
                      JSString::INLINE_IS_FAT_BIT);
        if (str->isLinear()) {
          if (str->isInline()) {
            known("FAT");
          } else if (!str->isAtom()) {
            known("EXTENSIBLE");
          } else {
            unknown(i);
          }
        } else {
          unknown(i);
        }
        break;
      case JSString::LINEAR_IS_EXTERNAL_BIT:
        static_assert(JSString::LINEAR_IS_EXTERNAL_BIT ==
                      JSString::ATOM_IS_PERMANENT_BIT);
        if (str->isAtom()) {
          known("PERMANENT");
        } else if (str->isLinear()) {
          known("EXTERNAL");
        } else {
          unknown(i);
        }
        break;
      case JSString::LATIN1_CHARS_BIT:
        known("LATIN1_CHARS_BIT");
        break;
      case JSString::ATOM_IS_INDEX_BIT:
        if (str->isAtom()) {
          known("ATOM_IS_INDEX_BIT");
        } else {
          known("ATOM_REF_BIT");
        }
        break;
      case JSString::INDEX_VALUE_BIT:
        known("INDEX_VALUE_BIT");
        break;
      case JSString::IN_STRING_TO_ATOM_CACHE:
        known("IN_STRING_TO_ATOM_CACHE");
        break;
      case JSString::FLATTEN_VISIT_RIGHT:
        if (str->isRope()) {
          known("FLATTEN_VISIT_RIGHT");
        } else {
          known("DEPENDED_ON_BIT");
        }
        break;
      case JSString::FLATTEN_FINISH_NODE:
        static_assert(JSString::FLATTEN_FINISH_NODE ==
                      JSString::PINNED_ATOM_BIT);
        if (str->isRope()) {
          known("FLATTEN_FINISH_NODE");
        } else if (str->isAtom()) {
          known("PINNED_ATOM_BIT");
        } else {
          known("NON_DEDUP_BIT");
        }
        break;
      default:
        unknown(i);
        break;
    }
  }
}

void JSString::dumpFields(js::JSONPrinter& json) const {
  dumpCommonFields(json);
  dumpCharsFields(json);
}

void JSString::dumpCommonFields(js::JSONPrinter& json) const {
  json.formatProperty("address", "(%s*)0x%p", RepresentationToString(this),
                      this);

  json.beginInlineListProperty("flags");
  ForEachStringFlag(
      this, flags(), [&](const char* name) { json.value("%s", name); },
      [&](uint32_t value) { json.value("Unknown(%08x)", value); });
  json.endInlineList();

  if (hasIndexValue()) {
    json.property("indexValue", getIndexValue());
  }

  json.boolProperty("isTenured", isTenured());

  json.property("length", length());
}

void JSString::dumpCharsFields(js::JSONPrinter& json) const {
  if (isLinear()) {
    const JSLinearString* linear = &asLinear();

    AutoCheckCannotGC nogc;
    if (hasLatin1Chars()) {
      const Latin1Char* chars = linear->latin1Chars(nogc);

      json.formatProperty("chars", "(JS::Latin1Char*)0x%p", chars);

      js::GenericPrinter& out = json.beginStringProperty("value");
      dumpCharsNoQuote(chars, length(), out);
      json.endStringProperty();
    } else {
      const char16_t* chars = linear->twoByteChars(nogc);

      json.formatProperty("chars", "(char16_t*)0x%p", chars);

      js::GenericPrinter& out = json.beginStringProperty("value");
      dumpCharsNoQuote(chars, length(), out);
      json.endStringProperty();
    }
  } else {
    js::GenericPrinter& out = json.beginStringProperty("value");
    dumpCharsNoQuote(out);
    json.endStringProperty();
  }
}

void JSString::dumpRepresentation() const {
  js::Fprinter out(stderr);
  dumpRepresentation(out);
}

void JSString::dumpRepresentation(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dumpRepresentation(json);
  out.put("\n");
}

void JSString::dumpRepresentation(js::JSONPrinter& json) const {
  json.beginObject();
  dumpRepresentationFields(json);
  json.endObject();
}

void JSString::dumpRepresentationFields(js::JSONPrinter& json) const {
  dumpCommonFields(json);

  if (isAtom()) {
    asAtom().dumpOwnRepresentationFields(json);
  } else if (isLinear()) {
    asLinear().dumpOwnRepresentationFields(json);

    if (isDependent()) {
      asDependent().dumpOwnRepresentationFields(json);
    } else if (isExternal()) {
      asExternal().dumpOwnRepresentationFields(json);
    } else if (isExtensible()) {
      asExtensible().dumpOwnRepresentationFields(json);
    } else if (isInline()) {
      asInline().dumpOwnRepresentationFields(json);
    }
  } else if (isRope()) {
    asRope().dumpOwnRepresentationFields(json);
    // Rope already shows the chars.
    return;
  }

  dumpCharsFields(json);
}

void JSString::dumpStringContent(js::GenericPrinter& out) const {
  dumpCharsSingleQuote(out);

  out.printf(" @ (%s*)0x%p", RepresentationToString(this), this);
}

void JSString::dumpPropertyName(js::GenericPrinter& out) const {
  dumpCharsNoQuote(out);
}

void JSString::dumpChars(js::GenericPrinter& out) const {
  out.putChar('"');
  dumpCharsNoQuote(out);
  out.putChar('"');
}

void JSString::dumpCharsSingleQuote(js::GenericPrinter& out) const {
  out.putChar('\'');
  dumpCharsNoQuote(out);
  out.putChar('\'');
}

void JSString::dumpCharsNoQuote(js::GenericPrinter& out) const {
  if (isLinear()) {
    const JSLinearString* linear = &asLinear();

    AutoCheckCannotGC nogc;
    if (hasLatin1Chars()) {
      dumpCharsNoQuote(linear->latin1Chars(nogc), length(), out);
    } else {
      dumpCharsNoQuote(linear->twoByteChars(nogc), length(), out);
    }
  } else if (isRope()) {
    JSRope* rope = &asRope();
    rope->leftChild()->dumpCharsNoQuote(out);
    rope->rightChild()->dumpCharsNoQuote(out);
  }
}

bool JSString::equals(const char* s) {
  JSLinearString* linear = ensureLinear(nullptr);
  if (!linear) {
    // This is DEBUG-only code.
    fprintf(stderr, "OOM in JSString::equals!\n");
    return false;
  }

  return StringEqualsAscii(linear, s);
}
#endif /* defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW) */

JSExtensibleString& JSLinearString::makeExtensible(size_t capacity) {
  MOZ_ASSERT(!isDependent());
  MOZ_ASSERT(!isInline());
  MOZ_ASSERT(!isAtom());
  MOZ_ASSERT(!isExternal());
  MOZ_ASSERT(capacity >= length());
  js::RemoveCellMemory(this, allocSize(), js::MemoryUse::StringContents);
  setLengthAndFlags(length(), flags() | EXTENSIBLE_FLAGS);
  d.s.u3.capacity = capacity;
  js::AddCellMemory(this, allocSize(), js::MemoryUse::StringContents);
  return asExtensible();
}

template <typename CharT>
static MOZ_ALWAYS_INLINE bool AllocCharsForFlatten(JSString* str, size_t length,
                                                   CharT** chars,
                                                   size_t* capacity) {
  /*
   * Grow by 12.5% if the buffer is very large. Otherwise, round up to the
   * next power of 2. This is similar to what we do with arrays; see
   * JSObject::ensureDenseArrayElements.
   */
  static const size_t DOUBLING_MAX = 1024 * 1024;
  *capacity =
      length > DOUBLING_MAX ? length + (length / 8) : RoundUpPow2(length);

  static_assert(JSString::MAX_LENGTH * sizeof(CharT) <= UINT32_MAX);
  *chars =
      str->zone()->pod_arena_malloc<CharT>(js::StringBufferArena, *capacity);
  return *chars != nullptr;
}

UniqueLatin1Chars JSRope::copyLatin1Chars(JSContext* maybecx,
                                          arena_id_t destArenaId) const {
  return copyCharsInternal<Latin1Char>(maybecx, destArenaId);
}

UniqueTwoByteChars JSRope::copyTwoByteChars(JSContext* maybecx,
                                            arena_id_t destArenaId) const {
  return copyCharsInternal<char16_t>(maybecx, destArenaId);
}

// Allocate chars for a string. If parameters and conditions allow, this will
// try to allocate in the nursery, but this may always fall back to a malloc
// allocation. The return value will record where the allocation happened.
template <typename CharT>
static MOZ_ALWAYS_INLINE JSString::OwnedChars<CharT> AllocChars(JSContext* cx,
                                                                size_t length,
                                                                gc::Heap heap) {
  if (heap == gc::Heap::Default && cx->zone()->allocNurseryStrings()) {
    MOZ_ASSERT(cx->nursery().isEnabled());
    auto [buffer, isMalloced] = cx->nursery().allocateBuffer(
        cx->zone(), length * sizeof(CharT), js::StringBufferArena);
    if (!buffer) {
      ReportOutOfMemory(cx);
      return {nullptr, 0, false, false};
    }

    return {static_cast<CharT*>(buffer), length, isMalloced, isMalloced};
  }

  auto buffer = cx->make_pod_arena_array<CharT>(js::StringBufferArena, length);
  if (!buffer) {
    ReportOutOfMemory(cx);
    return {nullptr, 0, false, false};
  }

  return {std::move(buffer), length, true};
}

template <typename CharT>
UniquePtr<CharT[], JS::FreePolicy> JSRope::copyCharsInternal(
    JSContext* maybecx, arena_id_t destArenaId) const {
  // Left-leaning ropes are far more common than right-leaning ropes, so
  // perform a non-destructive traversal of the rope, right node first,
  // splatting each node's characters into a contiguous buffer.

  size_t n = length();

  UniquePtr<CharT[], JS::FreePolicy> out;
  if (maybecx) {
    out.reset(maybecx->pod_arena_malloc<CharT>(destArenaId, n));
  } else {
    out.reset(js_pod_arena_malloc<CharT>(destArenaId, n));
  }

  if (!out) {
    return nullptr;
  }

  Vector<const JSString*, 8, SystemAllocPolicy> nodeStack;
  const JSString* str = this;
  CharT* end = out.get() + str->length();
  while (true) {
    if (str->isRope()) {
      if (!nodeStack.append(str->asRope().leftChild())) {
        if (maybecx) {
          ReportOutOfMemory(maybecx);
        }
        return nullptr;
      }
      str = str->asRope().rightChild();
    } else {
      end -= str->length();
      CopyChars(end, str->asLinear());
      if (nodeStack.empty()) {
        break;
      }
      str = nodeStack.popCopy();
    }
  }
  MOZ_ASSERT(end == out.get());

  return out;
}

template <typename CharT>
void AddStringToHash(uint32_t* hash, const CharT* chars, size_t len) {
  // It's tempting to use |HashString| instead of this loop, but that's
  // slightly different than our existing implementation for non-ropes. We
  // want to pretend we have a contiguous set of chars so we need to
  // accumulate char by char rather than generate a new hash for substring
  // and then accumulate that.
  for (size_t i = 0; i < len; i++) {
    *hash = mozilla::AddToHash(*hash, chars[i]);
  }
}

void AddStringToHash(uint32_t* hash, const JSString* str) {
  AutoCheckCannotGC nogc;
  const auto& s = str->asLinear();
  if (s.hasLatin1Chars()) {
    AddStringToHash(hash, s.latin1Chars(nogc), s.length());
  } else {
    AddStringToHash(hash, s.twoByteChars(nogc), s.length());
  }
}

bool JSRope::hash(uint32_t* outHash) const {
  Vector<const JSString*, 8, SystemAllocPolicy> nodeStack;
  const JSString* str = this;

  *outHash = 0;

  while (true) {
    if (str->isRope()) {
      if (!nodeStack.append(str->asRope().rightChild())) {
        return false;
      }
      str = str->asRope().leftChild();
    } else {
      AddStringToHash(outHash, str);
      if (nodeStack.empty()) {
        break;
      }
      str = nodeStack.popCopy();
    }
  }

  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
void JSRope::dumpOwnRepresentationFields(js::JSONPrinter& json) const {
  json.beginObjectProperty("leftChild");
  leftChild()->dumpRepresentationFields(json);
  json.endObject();

  json.beginObjectProperty("rightChild");
  rightChild()->dumpRepresentationFields(json);
  json.endObject();
}
#endif

namespace js {

template <>
void CopyChars(char16_t* dest, const JSLinearString& str) {
  AutoCheckCannotGC nogc;
  if (str.hasTwoByteChars()) {
    PodCopy(dest, str.twoByteChars(nogc), str.length());
  } else {
    CopyAndInflateChars(dest, str.latin1Chars(nogc), str.length());
  }
}

template <>
void CopyChars(Latin1Char* dest, const JSLinearString& str) {
  AutoCheckCannotGC nogc;
  if (str.hasLatin1Chars()) {
    PodCopy(dest, str.latin1Chars(nogc), str.length());
  } else {
    /*
     * When we flatten a TwoByte rope, we turn child ropes (including Latin1
     * ropes) into TwoByte dependent strings. If one of these strings is
     * also part of another Latin1 rope tree, we can have a Latin1 rope with
     * a TwoByte descendent and we end up here when we flatten it. Although
     * the chars are stored as TwoByte, we know they must be in the Latin1
     * range, so we can safely deflate here.
     */
    size_t len = str.length();
    const char16_t* chars = str.twoByteChars(nogc);
    auto src = Span(chars, len);
    MOZ_ASSERT(IsUtf16Latin1(src));
    LossyConvertUtf16toLatin1(src, AsWritableChars(Span(dest, len)));
  }
}

} /* namespace js */

template <typename CharT>
static constexpr uint32_t StringFlagsForCharType(uint32_t baseFlags) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    return baseFlags;
  }

  return baseFlags | JSString::LATIN1_CHARS_BIT;
}

static bool UpdateNurseryBuffersOnTransfer(js::Nursery& nursery, JSString* from,
                                           JSString* to, void* buffer,
                                           size_t size) {
  // Update the list of buffers associated with nursery cells when |buffer| is
  // moved from string |from| to string |to|, depending on whether those strings
  // are in the nursery or not.

  if (from->isTenured() && !to->isTenured()) {
    // Tenured leftmost child is giving its chars buffer to the
    // nursery-allocated root node.
    if (!nursery.registerMallocedBuffer(buffer, size)) {
      return false;
    }
  } else if (!from->isTenured() && to->isTenured()) {
    // Leftmost child is giving its nursery-held chars buffer to a
    // tenured string.
    nursery.removeMallocedBuffer(buffer, size);
  }

  return true;
}

static bool CanReuseLeftmostBuffer(JSString* leftmostChild, size_t wholeLength,
                                   bool hasTwoByteChars) {
  if (!leftmostChild->isExtensible()) {
    return false;
  }

  JSExtensibleString& str = leftmostChild->asExtensible();
  return str.capacity() >= wholeLength &&
         str.hasTwoByteChars() == hasTwoByteChars;
}

JSLinearString* JSRope::flatten(JSContext* maybecx) {
  mozilla::Maybe<AutoGeckoProfilerEntry> entry;
  if (maybecx) {
    entry.emplace(maybecx, "JSRope::flatten");
  }

  JSLinearString* str = flattenInternal();
  if (!str && maybecx) {
    ReportOutOfMemory(maybecx);
  }

  return str;
}

JSLinearString* JSRope::flattenInternal() {
  if (zone()->needsIncrementalBarrier()) {
    return flattenInternal<WithIncrementalBarrier>();
  }

  return flattenInternal<NoBarrier>();
}

template <JSRope::UsingBarrier usingBarrier>
JSLinearString* JSRope::flattenInternal() {
  if (hasTwoByteChars()) {
    return flattenInternal<usingBarrier, char16_t>(this);
  }

  return flattenInternal<usingBarrier, Latin1Char>(this);
}

template <JSRope::UsingBarrier usingBarrier, typename CharT>
/* static */
JSLinearString* JSRope::flattenInternal(JSRope* root) {
  /*
   * Consider the DAG of JSRopes rooted at |root|, with non-JSRopes as
   * its leaves. Mutate the root JSRope into a JSExtensibleString containing
   * the full flattened text that the root represents, and mutate all other
   * JSRopes in the interior of the DAG into JSDependentStrings that refer to
   * this new JSExtensibleString.
   *
   * If the leftmost leaf of our DAG is a JSExtensibleString, consider
   * stealing its buffer for use in our new root, and transforming it into a
   * JSDependentString too. Do not mutate any of the other leaves.
   *
   * Perform a depth-first dag traversal, splatting each node's characters
   * into a contiguous buffer. Visit each rope node three times:
   *   1. record position in the buffer and recurse into left child;
   *   2. recurse into the right child;
   *   3. transform the node into a dependent string.
   * To avoid maintaining a stack, tree nodes are mutated to indicate how many
   * times they have been visited. Since ropes can be dags, a node may be
   * encountered multiple times during traversal. However, step 3 above leaves
   * a valid dependent string, so everything works out.
   *
   * While ropes avoid all sorts of quadratic cases with string concatenation,
   * they can't help when ropes are immediately flattened. One idiomatic case
   * that we'd like to keep linear (and has traditionally been linear in SM
   * and other JS engines) is:
   *
   *   while (...) {
   *     s += ...
   *     s.flatten
   *   }
   *
   * Two behaviors accomplish this:
   *
   * - When the leftmost non-rope in the DAG we're flattening is a
   *   JSExtensibleString with sufficient capacity to hold the entire
   *   flattened string, we just flatten the DAG into its buffer. Then, when
   *   we transform the root of the DAG from a JSRope into a
   *   JSExtensibleString, we steal that buffer, and change the victim from a
   *   JSExtensibleString to a JSDependentString. In this case, the left-hand
   *   side of the string never needs to be copied.
   *
   * - Otherwise, we round up the total flattened size and create a fresh
   *   JSExtensibleString with that much capacity. If this in turn becomes the
   *   leftmost leaf of a subsequent flatten, we will hopefully be able to
   *   fill it, as in the case above.
   *
   * Note that, even though the code for creating JSDependentStrings avoids
   * creating dependents of dependents, we can create that situation here: the
   * JSExtensibleStrings we transform into JSDependentStrings might have
   * JSDependentStrings pointing to them already. Stealing the buffer doesn't
   * change its address, only its owning JSExtensibleString, so all chars()
   * pointers in the JSDependentStrings are still valid.
   *
   * This chain of dependent strings could be problematic if the base string
   * moves, either because it was initially allocated in the nursery or it
   * gets deduplicated, because you might have a dependent ->
   * tenured dependent -> nursery base string, and the store buffer would
   * only capture the latter edge. Prevent this case from happening by
   * marking the root as nondeduplicatable if the extensible string
   * optimization applied.
   */
  const size_t wholeLength = root->length();
  size_t wholeCapacity;
  CharT* wholeChars;
  uint32_t newRootFlags = 0;

  AutoCheckCannotGC nogc;

  Nursery& nursery = root->runtimeFromMainThread()->gc.nursery();

  /* Find the left most string, containing the first string. */
  JSRope* leftmostRope = root;
  while (leftmostRope->leftChild()->isRope()) {
    leftmostRope = &leftmostRope->leftChild()->asRope();
  }
  JSString* leftmostChild = leftmostRope->leftChild();

  bool reuseLeftmostBuffer = CanReuseLeftmostBuffer(
      leftmostChild, wholeLength, std::is_same_v<CharT, char16_t>);

  if (reuseLeftmostBuffer) {
    JSExtensibleString& left = leftmostChild->asExtensible();
    wholeCapacity = left.capacity();
    wholeChars = const_cast<CharT*>(left.nonInlineChars<CharT>(nogc));

    // Nursery::registerMallocedBuffer is fallible, so attempt it first before
    // doing anything irreversible.
    if (!UpdateNurseryBuffersOnTransfer(nursery, &left, root, wholeChars,
                                        wholeCapacity * sizeof(CharT))) {
      return nullptr;
    }
  } else {
    // If we can't reuse the leftmost child's buffer, allocate a new one.
    if (!AllocCharsForFlatten(root, wholeLength, &wholeChars, &wholeCapacity)) {
      return nullptr;
    }

    if (!root->isTenured()) {
      if (!nursery.registerMallocedBuffer(wholeChars,
                                          wholeCapacity * sizeof(CharT))) {
        js_free(wholeChars);
        return nullptr;
      }
    }
  }

  JSRope* str = root;
  CharT* pos = wholeChars;

  JSRope* parent = nullptr;
  uint32_t parentFlag = 0;

first_visit_node: {
  MOZ_ASSERT_IF(str != root, parent && parentFlag);
  MOZ_ASSERT(!str->asRope().isBeingFlattened());

  ropeBarrierDuringFlattening<usingBarrier>(str);

  JSString& left = *str->d.s.u2.left;
  str->d.s.u2.parent = parent;
  str->setFlagBit(parentFlag);
  parent = nullptr;
  parentFlag = 0;

  if (left.isRope()) {
    /* Return to this node when 'left' done, then goto visit_right_child. */
    parent = str;
    parentFlag = FLATTEN_VISIT_RIGHT;
    str = &left.asRope();
    goto first_visit_node;
  }
  if (!(reuseLeftmostBuffer && pos == wholeChars)) {
    CopyChars(pos, left.asLinear());
  }
  pos += left.length();
}

visit_right_child: {
  JSString& right = *str->d.s.u3.right;
  if (right.isRope()) {
    /* Return to this node when 'right' done, then goto finish_node. */
    parent = str;
    parentFlag = FLATTEN_FINISH_NODE;
    str = &right.asRope();
    goto first_visit_node;
  }
  CopyChars(pos, right.asLinear());
  pos += right.length();
}

finish_node: {
  if (str == root) {
    goto finish_root;
  }

  MOZ_ASSERT(pos >= wholeChars);
  CharT* chars = pos - str->length();
  JSRope* strParent = str->d.s.u2.parent;
  str->setNonInlineChars(chars);

  MOZ_ASSERT(str->asRope().isBeingFlattened());
  mozilla::DebugOnly<bool> visitRight = str->flags() & FLATTEN_VISIT_RIGHT;
  bool finishNode = str->flags() & FLATTEN_FINISH_NODE;
  MOZ_ASSERT(visitRight != finishNode);

  // This also clears the flags related to flattening.
  str->setLengthAndFlags(str->length(),
                         StringFlagsForCharType<CharT>(INIT_DEPENDENT_FLAGS));
  str->d.s.u3.base =
      reinterpret_cast<JSLinearString*>(root); /* will be true on exit */
  newRootFlags |= DEPENDED_ON_BIT;

  // Every interior (rope) node in the rope's tree will be visited during
  // the traversal and post-barriered here, so earlier additions of
  // dependent.base -> root pointers are handled by this barrier as well.
  //
  // The only time post-barriers need do anything is when the root is in
  // the nursery. Note that the root was a rope but will be an extensible
  // string when we return, so it will not point to any strings and need
  // not be barriered.
  if (str->isTenured() && !root->isTenured()) {
    root->storeBuffer()->putWholeCell(str);
  }

  str = strParent;
  if (finishNode) {
    goto finish_node;
  }
  MOZ_ASSERT(visitRight);
  goto visit_right_child;
}

finish_root:
  // We traversed all the way back up to the root so we're finished.
  MOZ_ASSERT(str == root);
  MOZ_ASSERT(pos == wholeChars + wholeLength);

  root->setLengthAndFlags(wholeLength,
                          StringFlagsForCharType<CharT>(EXTENSIBLE_FLAGS));
  root->setNonInlineChars(wholeChars);
  root->d.s.u3.capacity = wholeCapacity;
  AddCellMemory(root, root->asLinear().allocSize(), MemoryUse::StringContents);

  if (reuseLeftmostBuffer) {
    // Remove memory association for left node we're about to make into a
    // dependent string.
    JSString& left = *leftmostChild;
    RemoveCellMemory(&left, left.allocSize(), MemoryUse::StringContents);

    // Inherit NON_DEDUP_BIT from the leftmost string.
    newRootFlags |= left.flags() & NON_DEDUP_BIT;

    // Set root's DEPENDED_ON_BIT because the leftmost string is now a
    // dependent.
    newRootFlags |= DEPENDED_ON_BIT;

    uint32_t flags = INIT_DEPENDENT_FLAGS;
    if (left.inStringToAtomCache()) {
      flags |= IN_STRING_TO_ATOM_CACHE;
    }
    // If left was depended on, we need to make sure we preserve that. Even
    // though the string that depended on left's buffer will now depend on
    // root's buffer, if left is the only edge to root, replacing left with an
    // atom ref would break that edge and allow root's buffer to be freed.
    if (left.isDependedOn()) {
      flags |= DEPENDED_ON_BIT;
    }
    left.setLengthAndFlags(left.length(), StringFlagsForCharType<CharT>(flags));
    left.d.s.u3.base = &root->asLinear();
    if (left.isTenured() && !root->isTenured()) {
      // leftmost child -> root is a tenured -> nursery edge. Put the leftmost
      // child in the store buffer and prevent the root's chars from moving or
      // being freed (because the leftmost child may have a tenured dependent
      // string that cannot be updated.)
      root->storeBuffer()->putWholeCell(&left);
      newRootFlags |= NON_DEDUP_BIT;
    }
  }

  root->setHeaderFlagBit(newRootFlags);

  return &root->asLinear();
}

template <JSRope::UsingBarrier usingBarrier>
/* static */
inline void JSRope::ropeBarrierDuringFlattening(JSRope* rope) {
  MOZ_ASSERT(!rope->isBeingFlattened());
  if constexpr (usingBarrier) {
    gc::PreWriteBarrierDuringFlattening(rope->leftChild());
    gc::PreWriteBarrierDuringFlattening(rope->rightChild());
  }
}

template <AllowGC allowGC>
static JSLinearString* EnsureLinear(
    JSContext* cx,
    typename MaybeRooted<JSString*, allowGC>::HandleType string) {
  JSLinearString* linear = string->ensureLinear(cx);
  // Don't report an exception if GC is not allowed, just return nullptr.
  if (!linear && !allowGC) {
    cx->recoverFromOutOfMemory();
  }
  return linear;
}

template <AllowGC allowGC>
JSString* js::ConcatStrings(
    JSContext* cx, typename MaybeRooted<JSString*, allowGC>::HandleType left,
    typename MaybeRooted<JSString*, allowGC>::HandleType right, gc::Heap heap) {
  MOZ_ASSERT_IF(!left->isAtom(), cx->isInsideCurrentZone(left));
  MOZ_ASSERT_IF(!right->isAtom(), cx->isInsideCurrentZone(right));

  size_t leftLen = left->length();
  if (leftLen == 0) {
    return right;
  }

  size_t rightLen = right->length();
  if (rightLen == 0) {
    return left;
  }

  size_t wholeLength = leftLen + rightLen;
  if (MOZ_UNLIKELY(wholeLength > JSString::MAX_LENGTH)) {
    // Don't report an exception if GC is not allowed, just return nullptr.
    if (allowGC) {
      js::ReportOversizedAllocation(cx, JSMSG_ALLOC_OVERFLOW);
    }
    return nullptr;
  }

  bool isLatin1 = left->hasLatin1Chars() && right->hasLatin1Chars();
  bool canUseInline = isLatin1
                          ? JSInlineString::lengthFits<Latin1Char>(wholeLength)
                          : JSInlineString::lengthFits<char16_t>(wholeLength);
  if (canUseInline) {
    Latin1Char* latin1Buf = nullptr;  // initialize to silence GCC warning
    char16_t* twoByteBuf = nullptr;   // initialize to silence GCC warning
    JSInlineString* str =
        isLatin1
            ? AllocateInlineString<allowGC>(cx, wholeLength, &latin1Buf, heap)
            : AllocateInlineString<allowGC>(cx, wholeLength, &twoByteBuf, heap);
    if (!str) {
      return nullptr;
    }

    AutoCheckCannotGC nogc;
    JSLinearString* leftLinear = EnsureLinear<allowGC>(cx, left);
    if (!leftLinear) {
      return nullptr;
    }
    JSLinearString* rightLinear = EnsureLinear<allowGC>(cx, right);
    if (!rightLinear) {
      return nullptr;
    }

    if (isLatin1) {
      PodCopy(latin1Buf, leftLinear->latin1Chars(nogc), leftLen);
      PodCopy(latin1Buf + leftLen, rightLinear->latin1Chars(nogc), rightLen);
    } else {
      if (leftLinear->hasTwoByteChars()) {
        PodCopy(twoByteBuf, leftLinear->twoByteChars(nogc), leftLen);
      } else {
        CopyAndInflateChars(twoByteBuf, leftLinear->latin1Chars(nogc), leftLen);
      }
      if (rightLinear->hasTwoByteChars()) {
        PodCopy(twoByteBuf + leftLen, rightLinear->twoByteChars(nogc),
                rightLen);
      } else {
        CopyAndInflateChars(twoByteBuf + leftLen,
                            rightLinear->latin1Chars(nogc), rightLen);
      }
    }

    return str;
  }

  return JSRope::new_<allowGC>(cx, left, right, wholeLength, heap);
}

template JSString* js::ConcatStrings<CanGC>(JSContext* cx, HandleString left,
                                            HandleString right, gc::Heap heap);

template JSString* js::ConcatStrings<NoGC>(JSContext* cx, JSString* const& left,
                                           JSString* const& right,
                                           gc::Heap heap);

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
void JSDependentString::dumpOwnRepresentationFields(
    js::JSONPrinter& json) const {
  json.property("baseOffset", baseOffset());
  json.beginObjectProperty("base");
  base()->dumpRepresentationFields(json);
  json.endObject();
}
#endif

bool js::EqualChars(const JSLinearString* str1, const JSLinearString* str2) {
  // Assert this isn't called for strings the caller should handle with a fast
  // path.
  MOZ_ASSERT(str1->length() == str2->length());
  MOZ_ASSERT(str1 != str2);
  MOZ_ASSERT(!str1->isAtom() || !str2->isAtom());

  size_t len = str1->length();

  AutoCheckCannotGC nogc;
  if (str1->hasTwoByteChars()) {
    if (str2->hasTwoByteChars()) {
      return EqualChars(str1->twoByteChars(nogc), str2->twoByteChars(nogc),
                        len);
    }

    return EqualChars(str2->latin1Chars(nogc), str1->twoByteChars(nogc), len);
  }

  if (str2->hasLatin1Chars()) {
    return EqualChars(str1->latin1Chars(nogc), str2->latin1Chars(nogc), len);
  }

  return EqualChars(str1->latin1Chars(nogc), str2->twoByteChars(nogc), len);
}

bool js::HasSubstringAt(JSLinearString* text, JSLinearString* pat,
                        size_t start) {
  MOZ_ASSERT(start + pat->length() <= text->length());

  size_t patLen = pat->length();

  AutoCheckCannotGC nogc;
  if (text->hasLatin1Chars()) {
    const Latin1Char* textChars = text->latin1Chars(nogc) + start;
    if (pat->hasLatin1Chars()) {
      return EqualChars(textChars, pat->latin1Chars(nogc), patLen);
    }

    return EqualChars(textChars, pat->twoByteChars(nogc), patLen);
  }

  const char16_t* textChars = text->twoByteChars(nogc) + start;
  if (pat->hasTwoByteChars()) {
    return EqualChars(textChars, pat->twoByteChars(nogc), patLen);
  }

  return EqualChars(pat->latin1Chars(nogc), textChars, patLen);
}

bool js::EqualStrings(JSContext* cx, JSString* str1, JSString* str2,
                      bool* result) {
  if (str1 == str2) {
    *result = true;
    return true;
  }
  if (str1->length() != str2->length()) {
    *result = false;
    return true;
  }
  if (str1->isAtom() && str2->isAtom()) {
    *result = false;
    return true;
  }

  JSLinearString* linear1 = str1->ensureLinear(cx);
  if (!linear1) {
    return false;
  }
  JSLinearString* linear2 = str2->ensureLinear(cx);
  if (!linear2) {
    return false;
  }

  *result = EqualChars(linear1, linear2);
  return true;
}

bool js::EqualStrings(const JSLinearString* str1, const JSLinearString* str2) {
  if (str1 == str2) {
    return true;
  }
  if (str1->length() != str2->length()) {
    return false;
  }
  if (str1->isAtom() && str2->isAtom()) {
    return false;
  }
  return EqualChars(str1, str2);
}

int32_t js::CompareChars(const char16_t* s1, size_t len1, JSLinearString* s2) {
  AutoCheckCannotGC nogc;
  return s2->hasLatin1Chars()
             ? CompareChars(s1, len1, s2->latin1Chars(nogc), s2->length())
             : CompareChars(s1, len1, s2->twoByteChars(nogc), s2->length());
}

static int32_t CompareStringsImpl(const JSLinearString* str1,
                                  const JSLinearString* str2) {
  size_t len1 = str1->length();
  size_t len2 = str2->length();

  AutoCheckCannotGC nogc;
  if (str1->hasLatin1Chars()) {
    const Latin1Char* chars1 = str1->latin1Chars(nogc);
    return str2->hasLatin1Chars()
               ? CompareChars(chars1, len1, str2->latin1Chars(nogc), len2)
               : CompareChars(chars1, len1, str2->twoByteChars(nogc), len2);
  }

  const char16_t* chars1 = str1->twoByteChars(nogc);
  return str2->hasLatin1Chars()
             ? CompareChars(chars1, len1, str2->latin1Chars(nogc), len2)
             : CompareChars(chars1, len1, str2->twoByteChars(nogc), len2);
}

bool js::CompareStrings(JSContext* cx, JSString* str1, JSString* str2,
                        int32_t* result) {
  MOZ_ASSERT(str1);
  MOZ_ASSERT(str2);

  if (str1 == str2) {
    *result = 0;
    return true;
  }

  JSLinearString* linear1 = str1->ensureLinear(cx);
  if (!linear1) {
    return false;
  }

  JSLinearString* linear2 = str2->ensureLinear(cx);
  if (!linear2) {
    return false;
  }

  *result = CompareStringsImpl(linear1, linear2);
  return true;
}

int32_t js::CompareStrings(const JSLinearString* str1,
                           const JSLinearString* str2) {
  MOZ_ASSERT(str1);
  MOZ_ASSERT(str2);

  if (str1 == str2) {
    return 0;
  }
  return CompareStringsImpl(str1, str2);
}

bool js::StringIsAscii(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return mozilla::IsAscii(
        AsChars(Span(str->latin1Chars(nogc), str->length())));
  }
  return mozilla::IsAscii(Span(str->twoByteChars(nogc), str->length()));
}

bool js::StringEqualsAscii(JSLinearString* str, const char* asciiBytes) {
  return StringEqualsAscii(str, asciiBytes, strlen(asciiBytes));
}

bool js::StringEqualsAscii(JSLinearString* str, const char* asciiBytes,
                           size_t length) {
  MOZ_ASSERT(JS::StringIsASCII(Span(asciiBytes, length)));

  if (length != str->length()) {
    return false;
  }

  const Latin1Char* latin1 = reinterpret_cast<const Latin1Char*>(asciiBytes);

  AutoCheckCannotGC nogc;
  return str->hasLatin1Chars()
             ? EqualChars(latin1, str->latin1Chars(nogc), length)
             : EqualChars(latin1, str->twoByteChars(nogc), length);
}

template <typename CharT>
bool js::CheckStringIsIndex(const CharT* s, size_t length, uint32_t* indexp) {
  MOZ_ASSERT(length > 0);
  MOZ_ASSERT(length <= UINT32_CHAR_BUFFER_LENGTH);
  MOZ_ASSERT(IsAsciiDigit(*s),
             "caller's fast path must have checked first char");

  RangedPtr<const CharT> cp(s, length);
  const RangedPtr<const CharT> end(s + length, s, length);

  uint32_t index = AsciiDigitToNumber(*cp++);
  uint32_t oldIndex = 0;
  uint32_t c = 0;

  if (index != 0) {
    // Consume remaining characters only if the first character isn't '0'.
    while (cp < end && IsAsciiDigit(*cp)) {
      oldIndex = index;
      c = AsciiDigitToNumber(*cp);
      index = 10 * index + c;
      cp++;
    }
  }

  // It's not an integer index if there are characters after the number.
  if (cp != end) {
    return false;
  }

  // Look out for "4294967295" and larger-number strings that fit in
  // UINT32_CHAR_BUFFER_LENGTH: only unsigned 32-bit integers less than or equal
  // to MAX_ARRAY_INDEX shall pass.
  if (oldIndex < MAX_ARRAY_INDEX / 10 ||
      (oldIndex == MAX_ARRAY_INDEX / 10 && c <= (MAX_ARRAY_INDEX % 10))) {
    MOZ_ASSERT(index <= MAX_ARRAY_INDEX);
    *indexp = index;
    return true;
  }

  return false;
}

template bool js::CheckStringIsIndex(const Latin1Char* s, size_t length,
                                     uint32_t* indexp);
template bool js::CheckStringIsIndex(const char16_t* s, size_t length,
                                     uint32_t* indexp);

template <typename CharT>
static uint32_t AtomCharsToIndex(const CharT* s, size_t length) {
  // Chars are known to be a valid index value (as determined by
  // CheckStringIsIndex) that didn't fit in the "index value" bits in the
  // header.

  MOZ_ASSERT(length > 0);
  MOZ_ASSERT(length <= UINT32_CHAR_BUFFER_LENGTH);

  RangedPtr<const CharT> cp(s, length);
  const RangedPtr<const CharT> end(s + length, s, length);

  MOZ_ASSERT(IsAsciiDigit(*cp));
  uint32_t index = AsciiDigitToNumber(*cp++);
  MOZ_ASSERT(index != 0);

  while (cp < end) {
    MOZ_ASSERT(IsAsciiDigit(*cp));
    index = 10 * index + AsciiDigitToNumber(*cp);
    cp++;
  }

  MOZ_ASSERT(index <= MAX_ARRAY_INDEX);
  return index;
}

uint32_t JSAtom::getIndexSlow() const {
  MOZ_ASSERT(isIndex());
  MOZ_ASSERT(!hasIndexValue());

  size_t len = length();

  AutoCheckCannotGC nogc;
  return hasLatin1Chars() ? AtomCharsToIndex(latin1Chars(nogc), len)
                          : AtomCharsToIndex(twoByteChars(nogc), len);
}

// Ensure that the incoming s.chars pointer is stable, as in, it cannot be
// changed even across a GC. That requires that the string that owns the chars
// not be collected or deduplicated.
void AutoStableStringChars::holdStableChars(JSLinearString* s) {
  while (s->hasBase()) {
    s = s->base();
  }
  if (!s->isTenured()) {
    s->setNonDeduplicatable();
  }
  s_ = s;
}

bool AutoStableStringChars::init(JSContext* cx, JSString* s) {
  Rooted<JSLinearString*> linearString(cx, s->ensureLinear(cx));
  if (!linearString) {
    return false;
  }

  linearString->setDependedOn();

  MOZ_ASSERT(state_ == Uninitialized);
  length_ = linearString->length();

  // Inline and nursery-allocated chars may move during a GC, so copy them
  // out into a temporary malloced buffer. Note that we cannot update the
  // string itself with a malloced buffer, because there may be dependent
  // strings that are using the original chars.
  if (linearString->hasMovableChars()) {
    return linearString->hasTwoByteChars() ? copyTwoByteChars(cx, linearString)
                                           : copyLatin1Chars(cx, linearString);
  }

  if (linearString->hasLatin1Chars()) {
    state_ = Latin1;
    latin1Chars_ = linearString->rawLatin1Chars();
  } else {
    state_ = TwoByte;
    twoByteChars_ = linearString->rawTwoByteChars();
  }

  holdStableChars(linearString);
  return true;
}

bool AutoStableStringChars::initTwoByte(JSContext* cx, JSString* s) {
  Rooted<JSLinearString*> linearString(cx, s->ensureLinear(cx));
  if (!linearString) {
    return false;
  }

  linearString->setDependedOn();

  MOZ_ASSERT(state_ == Uninitialized);
  length_ = linearString->length();

  if (linearString->hasLatin1Chars()) {
    return copyAndInflateLatin1Chars(cx, linearString);
  }

  // Copy movable chars since they may be moved by GC (see above).
  if (linearString->hasMovableChars()) {
    return copyTwoByteChars(cx, linearString);
  }

  state_ = TwoByte;
  twoByteChars_ = linearString->rawTwoByteChars();

  holdStableChars(linearString);
  return true;
}

template <typename T>
T* AutoStableStringChars::allocOwnChars(JSContext* cx, size_t count) {
  static_assert(
      InlineCapacity >=
              sizeof(JS::Latin1Char) * JSFatInlineString::MAX_LENGTH_LATIN1 &&
          InlineCapacity >=
              sizeof(char16_t) * JSFatInlineString::MAX_LENGTH_TWO_BYTE,
      "InlineCapacity too small to hold fat inline strings");

  static_assert((JSString::MAX_LENGTH &
                 mozilla::tl::MulOverflowMask<sizeof(T)>::value) == 0,
                "Size calculation can overflow");
  MOZ_ASSERT(count <= JSString::MAX_LENGTH);
  size_t size = sizeof(T) * count;

  ownChars_.emplace(cx);
  if (!ownChars_->resize(size)) {
    ownChars_.reset();
    return nullptr;
  }

  return reinterpret_cast<T*>(ownChars_->begin());
}

bool AutoStableStringChars::copyAndInflateLatin1Chars(
    JSContext* cx, Handle<JSLinearString*> linearString) {
  MOZ_ASSERT(state_ == Uninitialized);
  MOZ_ASSERT(s_ == nullptr);

  char16_t* chars = allocOwnChars<char16_t>(cx, length_);
  if (!chars) {
    return false;
  }

  // Copy |src[0..length]| to |dest[0..length]| when copying doesn't narrow and
  // therefore can't lose information.
  auto src = AsChars(Span(linearString->rawLatin1Chars(), length_));
  auto dest = Span(chars, length_);
  ConvertLatin1toUtf16(src, dest);

  state_ = TwoByte;
  twoByteChars_ = chars;
  s_ = linearString;
  return true;
}

bool AutoStableStringChars::copyLatin1Chars(
    JSContext* cx, Handle<JSLinearString*> linearString) {
  MOZ_ASSERT(state_ == Uninitialized);
  MOZ_ASSERT(s_ == nullptr);

  JS::Latin1Char* chars = allocOwnChars<JS::Latin1Char>(cx, length_);
  if (!chars) {
    return false;
  }

  PodCopy(chars, linearString->rawLatin1Chars(), length_);

  state_ = Latin1;
  latin1Chars_ = chars;
  s_ = linearString;
  return true;
}

bool AutoStableStringChars::copyTwoByteChars(
    JSContext* cx, Handle<JSLinearString*> linearString) {
  MOZ_ASSERT(state_ == Uninitialized);
  MOZ_ASSERT(s_ == nullptr);

  char16_t* chars = allocOwnChars<char16_t>(cx, length_);
  if (!chars) {
    return false;
  }

  PodCopy(chars, linearString->rawTwoByteChars(), length_);

  state_ = TwoByte;
  twoByteChars_ = chars;
  s_ = linearString;
  return true;
}

template <>
bool JS::SourceText<char16_t>::initMaybeBorrowed(
    JSContext* cx, JS::AutoStableStringChars& linearChars) {
  MOZ_ASSERT(linearChars.isTwoByte(),
             "AutoStableStringChars must be initialized with char16_t");

  const char16_t* chars = linearChars.twoByteChars();
  size_t length = linearChars.length();
  JS::SourceOwnership ownership = linearChars.maybeGiveOwnershipToCaller()
                                      ? JS::SourceOwnership::TakeOwnership
                                      : JS::SourceOwnership::Borrowed;
  return initImpl(cx, chars, length, ownership);
}

template <>
bool JS::SourceText<char16_t>::initMaybeBorrowed(
    JS::FrontendContext* fc, JS::AutoStableStringChars& linearChars) {
  MOZ_ASSERT(linearChars.isTwoByte(),
             "AutoStableStringChars must be initialized with char16_t");

  const char16_t* chars = linearChars.twoByteChars();
  size_t length = linearChars.length();
  JS::SourceOwnership ownership = linearChars.maybeGiveOwnershipToCaller()
                                      ? JS::SourceOwnership::TakeOwnership
                                      : JS::SourceOwnership::Borrowed;
  return initImpl(fc, chars, length, ownership);
}

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
void JSAtom::dump(js::GenericPrinter& out) {
  out.printf("JSAtom* (%p) = ", (void*)this);
  this->JSString::dump(out);
}

void JSAtom::dump() {
  Fprinter out(stderr);
  dump(out);
}

void JSExternalString::dumpOwnRepresentationFields(
    js::JSONPrinter& json) const {
  json.formatProperty("callbacks", "(JSExternalStringCallbacks*)0x%p",
                      callbacks());
}
#endif /* defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW) */

JSLinearString* js::NewDependentString(JSContext* cx, JSString* baseArg,
                                       size_t start, size_t length,
                                       gc::Heap heap) {
  if (length == 0) {
    return cx->emptyString();
  }

  JSLinearString* base = baseArg->ensureLinear(cx);
  if (!base) {
    return nullptr;
  }

  if (start == 0 && length == base->length()) {
    return base;
  }

  bool useInline;
  if (base->hasTwoByteChars()) {
    AutoCheckCannotGC nogc;
    const char16_t* chars = base->twoByteChars(nogc) + start;
    if (JSLinearString* staticStr = cx->staticStrings().lookup(chars, length)) {
      return staticStr;
    }
    useInline = JSInlineString::lengthFits<char16_t>(length);
  } else {
    AutoCheckCannotGC nogc;
    const Latin1Char* chars = base->latin1Chars(nogc) + start;
    if (JSLinearString* staticStr = cx->staticStrings().lookup(chars, length)) {
      return staticStr;
    }
    useInline = JSInlineString::lengthFits<Latin1Char>(length);
  }

  if (useInline) {
    Rooted<JSLinearString*> rootedBase(cx, base);

    // Do not create a dependent string that would fit into an inline string.
    // First, that could create a string dependent on an inline base string's
    // chars, which would be an awkward moving-GC hazard. Second, this makes
    // it more likely to have a very short string keep a very long string alive.
    if (base->hasTwoByteChars()) {
      return NewInlineString<char16_t>(cx, rootedBase, start, length, heap);
    }
    return NewInlineString<Latin1Char>(cx, rootedBase, start, length, heap);
  }

  return JSDependentString::new_(cx, base, start, length, heap);
}

static constexpr bool CanStoreCharsAsLatin1(const JS::Latin1Char* s,
                                            size_t length) {
  return true;
}

static inline bool CanStoreCharsAsLatin1(const char16_t* s, size_t length) {
  return IsUtf16Latin1(Span(s, length));
}

/**
 * Copy |src[0..length]| to |dest[0..length]| when copying *does* narrow, but
 * the user guarantees every runtime |src[i]| value can be stored without change
 * of value in |dest[i]|.
 */
static inline void FillFromCompatible(unsigned char* dest, const char16_t* src,
                                      size_t length) {
  LossyConvertUtf16toLatin1(Span(src, length),
                            AsWritableChars(Span(dest, length)));
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineStringDeflated(
    JSContext* cx, const mozilla::Range<const char16_t>& chars,
    gc::Heap heap = gc::Heap::Default) {
  size_t len = chars.length();
  Latin1Char* storage;
  JSInlineString* str = AllocateInlineString<allowGC>(cx, len, &storage, heap);
  if (!str) {
    return nullptr;
  }

  MOZ_ASSERT(CanStoreCharsAsLatin1(chars.begin().get(), len));
  FillFromCompatible(storage, chars.begin().get(), len);
  return str;
}

template <AllowGC allowGC>
static JSLinearString* NewStringDeflated(JSContext* cx, const char16_t* s,
                                         size_t n, gc::Heap heap) {
  if (JSLinearString* str = TryEmptyOrStaticString(cx, s, n)) {
    return str;
  }

  if (JSInlineString::lengthFits<Latin1Char>(n)) {
    return NewInlineStringDeflated<allowGC>(
        cx, mozilla::Range<const char16_t>(s, n), heap);
  }

  JS::Rooted<JSString::OwnedChars<Latin1Char>> news(
      cx, AllocChars<Latin1Char>(cx, n, heap));
  if (!news) {
    if (!allowGC) {
      cx->recoverFromOutOfMemory();
    }
    return nullptr;
  }

  MOZ_ASSERT(CanStoreCharsAsLatin1(s, n));
  FillFromCompatible(news.data(), s, n);

  return JSLinearString::new_<allowGC, Latin1Char>(cx, &news, heap);
}

static MOZ_ALWAYS_INLINE JSAtom* NewInlineAtomDeflated(JSContext* cx,
                                                       const char16_t* chars,
                                                       size_t length,
                                                       js::HashNumber hash) {
  Latin1Char* storage;
  JSAtom* str = AllocateInlineAtom(cx, length, &storage, hash);
  if (!str) {
    return nullptr;
  }

  MOZ_ASSERT(CanStoreCharsAsLatin1(chars, length));
  FillFromCompatible(storage, chars, length);
  return str;
}

static JSAtom* NewAtomDeflatedValidLength(JSContext* cx, const char16_t* s,
                                          size_t n, js::HashNumber hash) {
  if (JSAtom::lengthFitsInline<Latin1Char>(n)) {
    return NewInlineAtomDeflated(cx, s, n, hash);
  }

  auto news = cx->make_pod_arena_array<Latin1Char>(js::StringBufferArena, n);
  if (!news) {
    cx->recoverFromOutOfMemory();
    return nullptr;
  }

  MOZ_ASSERT(CanStoreCharsAsLatin1(s, n));
  FillFromCompatible(news.get(), s, n);

  return JSAtom::newValidLength(cx, std::move(news), n, hash);
}

template <AllowGC allowGC, typename CharT>
JSLinearString* js::NewStringDontDeflate(
    JSContext* cx, UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    gc::Heap heap) {
  if (JSLinearString* str = TryEmptyOrStaticString(cx, chars.get(), length)) {
    return str;
  }

  if (JSInlineString::lengthFits<CharT>(length)) {
    // |chars.get()| is safe because 1) |NewInlineString| necessarily *copies*,
    // and 2) |chars| frees its contents only when this function returns.
    return NewInlineString<allowGC>(
        cx, mozilla::Range<const CharT>(chars.get(), length), heap);
  }

  JS::Rooted<JSString::OwnedChars<CharT>> ownedChars(cx, std::move(chars),
                                                     length, true);
  return JSLinearString::new_<allowGC, CharT>(cx, &ownedChars, heap);
}

template JSLinearString* js::NewStringDontDeflate<CanGC>(
    JSContext* cx, UniqueTwoByteChars chars, size_t length, gc::Heap heap);

template JSLinearString* js::NewStringDontDeflate<NoGC>(
    JSContext* cx, UniqueTwoByteChars chars, size_t length, gc::Heap heap);

template JSLinearString* js::NewStringDontDeflate<CanGC>(
    JSContext* cx, UniqueLatin1Chars chars, size_t length, gc::Heap heap);

template JSLinearString* js::NewStringDontDeflate<NoGC>(JSContext* cx,
                                                        UniqueLatin1Chars chars,
                                                        size_t length,
                                                        gc::Heap heap);

template <AllowGC allowGC, typename CharT>
JSLinearString* js::NewString(JSContext* cx,
                              UniquePtr<CharT[], JS::FreePolicy> chars,
                              size_t length, gc::Heap heap) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    if (CanStoreCharsAsLatin1(chars.get(), length)) {
      // Deflating copies from |chars.get()| and lets |chars| be freed on
      // return.
      return NewStringDeflated<allowGC>(cx, chars.get(), length, heap);
    }
  }

  return NewStringDontDeflate<allowGC>(cx, std::move(chars), length, heap);
}

template JSLinearString* js::NewString<CanGC>(JSContext* cx,
                                              UniqueTwoByteChars chars,
                                              size_t length, gc::Heap heap);

template JSLinearString* js::NewString<NoGC>(JSContext* cx,
                                             UniqueTwoByteChars chars,
                                             size_t length, gc::Heap heap);

template JSLinearString* js::NewString<CanGC>(JSContext* cx,
                                              UniqueLatin1Chars chars,
                                              size_t length, gc::Heap heap);

template JSLinearString* js::NewString<NoGC>(JSContext* cx,
                                             UniqueLatin1Chars chars,
                                             size_t length, gc::Heap heap);

namespace js {

template <AllowGC allowGC, typename CharT>
JSLinearString* NewStringCopyNDontDeflateNonStaticValidLength(JSContext* cx,
                                                              const CharT* s,
                                                              size_t n,
                                                              gc::Heap heap) {
  if (JSInlineString::lengthFits<CharT>(n)) {
    return NewInlineString<allowGC>(cx, mozilla::Range<const CharT>(s, n),
                                    heap);
  }

  Rooted<JSString::OwnedChars<CharT>> news(cx,
                                           ::AllocChars<CharT>(cx, n, heap));
  if (!news) {
    if (!allowGC) {
      cx->recoverFromOutOfMemory();
    }
    return nullptr;
  }

  PodCopy(news.data(), s, n);

  return JSLinearString::newValidLength<allowGC, CharT>(cx, &news, heap);
}

template JSLinearString* NewStringCopyNDontDeflateNonStaticValidLength<CanGC>(
    JSContext* cx, const char16_t* s, size_t n, gc::Heap heap);

template JSLinearString* NewStringCopyNDontDeflateNonStaticValidLength<CanGC>(
    JSContext* cx, const Latin1Char* s, size_t n, gc::Heap heap);

template <AllowGC allowGC, typename CharT>
JSLinearString* NewStringCopyNDontDeflate(JSContext* cx, const CharT* s,
                                          size_t n, gc::Heap heap) {
  if (JSLinearString* str = TryEmptyOrStaticString(cx, s, n)) {
    return str;
  }

  if (MOZ_UNLIKELY(!JSLinearString::validateLength(cx, n))) {
    return nullptr;
  }

  return NewStringCopyNDontDeflateNonStaticValidLength<allowGC>(cx, s, n, heap);
}

template JSLinearString* NewStringCopyNDontDeflate<CanGC>(JSContext* cx,
                                                          const char16_t* s,
                                                          size_t n,
                                                          gc::Heap heap);

template JSLinearString* NewStringCopyNDontDeflate<NoGC>(JSContext* cx,
                                                         const char16_t* s,
                                                         size_t n,
                                                         gc::Heap heap);

template JSLinearString* NewStringCopyNDontDeflate<CanGC>(JSContext* cx,
                                                          const Latin1Char* s,
                                                          size_t n,
                                                          gc::Heap heap);

template JSLinearString* NewStringCopyNDontDeflate<NoGC>(JSContext* cx,
                                                         const Latin1Char* s,
                                                         size_t n,
                                                         gc::Heap heap);

JSLinearString* NewLatin1StringZ(JSContext* cx, UniqueChars chars,
                                 gc::Heap heap) {
  size_t length = strlen(chars.get());
  UniqueLatin1Chars latin1(reinterpret_cast<Latin1Char*>(chars.release()));
  return NewString<CanGC>(cx, std::move(latin1), length, heap);
}

template <AllowGC allowGC, typename CharT>
JSLinearString* NewStringCopyN(JSContext* cx, const CharT* s, size_t n,
                               gc::Heap heap) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    if (CanStoreCharsAsLatin1(s, n)) {
      return NewStringDeflated<allowGC>(cx, s, n, heap);
    }
  }

  return NewStringCopyNDontDeflate<allowGC>(cx, s, n, heap);
}

template JSLinearString* NewStringCopyN<CanGC>(JSContext* cx, const char16_t* s,
                                               size_t n, gc::Heap heap);

template JSLinearString* NewStringCopyN<NoGC>(JSContext* cx, const char16_t* s,
                                              size_t n, gc::Heap heap);

template JSLinearString* NewStringCopyN<CanGC>(JSContext* cx,
                                               const Latin1Char* s, size_t n,
                                               gc::Heap heap);

template JSLinearString* NewStringCopyN<NoGC>(JSContext* cx,
                                              const Latin1Char* s, size_t n,
                                              gc::Heap heap);

template <typename CharT>
JSAtom* NewAtomCopyNDontDeflateValidLength(JSContext* cx, const CharT* s,
                                           size_t n, js::HashNumber hash) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    MOZ_ASSERT(!CanStoreCharsAsLatin1(s, n));
  }

  if (JSAtom::lengthFitsInline<CharT>(n)) {
    return NewInlineAtom(cx, s, n, hash);
  }

  auto news = cx->make_pod_arena_array<CharT>(js::StringBufferArena, n);
  if (!news) {
    cx->recoverFromOutOfMemory();
    return nullptr;
  }

  PodCopy(news.get(), s, n);

  return JSAtom::newValidLength(cx, std::move(news), n, hash);
}

template JSAtom* NewAtomCopyNDontDeflateValidLength(JSContext* cx,
                                                    const char16_t* s, size_t n,
                                                    js::HashNumber hash);

template JSAtom* NewAtomCopyNDontDeflateValidLength(JSContext* cx,
                                                    const Latin1Char* s,
                                                    size_t n,
                                                    js::HashNumber hash);

template <typename CharT>
JSAtom* NewAtomCopyNMaybeDeflateValidLength(JSContext* cx, const CharT* s,
                                            size_t n, js::HashNumber hash) {
  if constexpr (std::is_same_v<CharT, char16_t>) {
    if (CanStoreCharsAsLatin1(s, n)) {
      return NewAtomDeflatedValidLength(cx, s, n, hash);
    }
  }

  return NewAtomCopyNDontDeflateValidLength(cx, s, n, hash);
}

template JSAtom* NewAtomCopyNMaybeDeflateValidLength(JSContext* cx,
                                                     const char16_t* s,
                                                     size_t n,
                                                     js::HashNumber hash);

template JSAtom* NewAtomCopyNMaybeDeflateValidLength(JSContext* cx,
                                                     const Latin1Char* s,
                                                     size_t n,
                                                     js::HashNumber hash);

JSLinearString* NewStringCopyUTF8N(JSContext* cx, const JS::UTF8Chars& utf8,
                                   JS::SmallestEncoding encoding,
                                   gc::Heap heap) {
  if (encoding == JS::SmallestEncoding::ASCII) {
    return NewStringCopyN<js::CanGC>(cx, utf8.begin().get(), utf8.length(),
                                     heap);
  }

  size_t length;
  if (encoding == JS::SmallestEncoding::Latin1) {
    UniqueLatin1Chars latin1(
        UTF8CharsToNewLatin1CharsZ(cx, utf8, &length, js::StringBufferArena)
            .get());
    if (!latin1) {
      return nullptr;
    }

    return NewString<js::CanGC>(cx, std::move(latin1), length, heap);
  }

  MOZ_ASSERT(encoding == JS::SmallestEncoding::UTF16);

  UniqueTwoByteChars utf16(
      UTF8CharsToNewTwoByteCharsZ(cx, utf8, &length, js::StringBufferArena)
          .get());
  if (!utf16) {
    return nullptr;
  }

  return NewString<js::CanGC>(cx, std::move(utf16), length, heap);
}

JSLinearString* NewStringCopyUTF8N(JSContext* cx, const JS::UTF8Chars& utf8,
                                   gc::Heap heap) {
  JS::SmallestEncoding encoding = JS::FindSmallestEncoding(utf8);
  return NewStringCopyUTF8N(cx, utf8, encoding, heap);
}

template <typename CharT>
MOZ_ALWAYS_INLINE JSExternalString* ExternalStringCache::lookupExternalImpl(
    const CharT* chars, size_t len) const {
  AutoCheckCannotGC nogc;

  for (size_t i = 0; i < NumEntries; i++) {
    JSExternalString* str = externalEntries_[i];
    if (!str || str->length() != len) {
      continue;
    }

    if constexpr (std::is_same_v<CharT, JS::Latin1Char>) {
      if (!str->hasLatin1Chars()) {
        continue;
      }
    } else {
      if (!str->hasTwoByteChars()) {
        continue;
      }
    }

    const CharT* strChars = str->nonInlineChars<CharT>(nogc);
    if (chars == strChars) {
      // Note that we don't need an incremental barrier here or below.
      // The cache is purged on GC so any string we get from the cache
      // must have been allocated after the GC started.
      return str;
    }

    // Compare the chars. Don't do this for long strings as it will be
    // faster to allocate a new external string.
    static const size_t MaxLengthForCharComparison = 100;
    if (len <= MaxLengthForCharComparison && EqualChars(chars, strChars, len)) {
      return str;
    }
  }

  return nullptr;
}

MOZ_ALWAYS_INLINE JSExternalString* ExternalStringCache::lookupExternal(
    const JS::Latin1Char* chars, size_t len) const {
  return lookupExternalImpl(chars, len);
}
MOZ_ALWAYS_INLINE JSExternalString* ExternalStringCache::lookupExternal(
    const char16_t* chars, size_t len) const {
  return lookupExternalImpl(chars, len);
}

MOZ_ALWAYS_INLINE void ExternalStringCache::putExternal(JSExternalString* str) {
  for (size_t i = NumEntries - 1; i > 0; i--) {
    externalEntries_[i] = externalEntries_[i - 1];
  }
  externalEntries_[0] = str;
}

template <typename CharT>
MOZ_ALWAYS_INLINE JSInlineString* ExternalStringCache::lookupInlineImpl(
    const CharT* chars, size_t len) const {
  MOZ_ASSERT(CanStoreCharsAsLatin1(chars, len));
  MOZ_ASSERT(JSThinInlineString::lengthFits<Latin1Char>(len));

  AutoCheckCannotGC nogc;

  for (size_t i = 0; i < NumEntries; i++) {
    JSInlineString* str = inlineEntries_[i];
    if (!str || str->length() != len) {
      continue;
    }

    const JS::Latin1Char* strChars = str->latin1Chars(nogc);
    if (EqualChars(chars, strChars, len)) {
      return str;
    }
  }

  return nullptr;
}

MOZ_ALWAYS_INLINE JSInlineString* ExternalStringCache::lookupInline(
    const JS::Latin1Char* chars, size_t len) const {
  return lookupInlineImpl(chars, len);
}
MOZ_ALWAYS_INLINE JSInlineString* ExternalStringCache::lookupInline(
    const char16_t* chars, size_t len) const {
  return lookupInlineImpl(chars, len);
}

MOZ_ALWAYS_INLINE void ExternalStringCache::putInline(JSInlineString* str) {
  MOZ_ASSERT(str->hasLatin1Chars());

  for (size_t i = NumEntries - 1; i > 0; i--) {
    inlineEntries_[i] = inlineEntries_[i - 1];
  }
  inlineEntries_[0] = str;
}

} /* namespace js */

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineStringMaybeDeflated(
    JSContext* cx, const mozilla::Range<const JS::Latin1Char>& chars,
    gc::Heap heap = gc::Heap::Default) {
  return NewInlineString<allowGC>(cx, chars, heap);
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSInlineString* NewInlineStringMaybeDeflated(
    JSContext* cx, const mozilla::Range<const char16_t>& chars,
    gc::Heap heap = gc::Heap::Default) {
  return NewInlineStringDeflated<allowGC>(cx, chars, heap);
}

namespace js {

template <typename CharT>
JSString* NewMaybeExternalString(JSContext* cx, const CharT* s, size_t n,
                                 const JSExternalStringCallbacks* callbacks,
                                 bool* allocatedExternal, gc::Heap heap) {
  if (JSString* str = TryEmptyOrStaticString(cx, s, n)) {
    *allocatedExternal = false;
    return str;
  }

  ExternalStringCache& cache = cx->zone()->externalStringCache();

  if (JSThinInlineString::lengthFits<Latin1Char>(n) &&
      CanStoreCharsAsLatin1(s, n)) {
    *allocatedExternal = false;
    if (JSInlineString* str = cache.lookupInline(s, n)) {
      return str;
    }
    JSInlineString* str = NewInlineStringMaybeDeflated<AllowGC::CanGC>(
        cx, mozilla::Range<const CharT>(s, n), heap);
    if (!str) {
      return nullptr;
    }
    cache.putInline(str);
    return str;
  }

  if (JSExternalString* str = cache.lookupExternal(s, n)) {
    *allocatedExternal = false;
    return str;
  }

  JSExternalString* str = JSExternalString::new_(cx, s, n, callbacks);
  if (!str) {
    return nullptr;
  }

  *allocatedExternal = true;
  cache.putExternal(str);
  return str;
}

template JSString* NewMaybeExternalString(
    JSContext* cx, const JS::Latin1Char* s, size_t n,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal,
    gc::Heap heap);

template JSString* NewMaybeExternalString(
    JSContext* cx, const char16_t* s, size_t n,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal,
    gc::Heap heap);

} /* namespace js */

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
void JSExtensibleString::dumpOwnRepresentationFields(
    js::JSONPrinter& json) const {
  json.property("capacity", capacity());
}

void JSInlineString::dumpOwnRepresentationFields(js::JSONPrinter& json) const {}

void JSLinearString::dumpOwnRepresentationFields(js::JSONPrinter& json) const {
  if (!isInline()) {
    // Include whether the chars are in the nursery even for tenured
    // strings, which should always be false. For investigating bugs, it's
    // better to not assume that.
    js::Nursery& nursery = runtimeFromMainThread()->gc.nursery();
    bool inNursery = nursery.isInside(nonInlineCharsRaw());
    json.boolProperty("charsInNursery", inNursery);
  }
}
#endif

struct RepresentativeExternalString : public JSExternalStringCallbacks {
  void finalize(JS::Latin1Char* chars) const override {
    // Constant chars, nothing to do.
  }
  void finalize(char16_t* chars) const override {
    // Constant chars, nothing to do.
  }
  size_t sizeOfBuffer(const JS::Latin1Char* chars,
                      mozilla::MallocSizeOf mallocSizeOf) const override {
    // This string's buffer is not heap-allocated, so its malloc size is 0.
    return 0;
  }
  size_t sizeOfBuffer(const char16_t* chars,
                      mozilla::MallocSizeOf mallocSizeOf) const override {
    // This string's buffer is not heap-allocated, so its malloc size is 0.
    return 0;
  }
};

static const RepresentativeExternalString RepresentativeExternalStringCallbacks;

template <typename CheckString, typename CharT>
static bool FillWithRepresentatives(JSContext* cx, Handle<ArrayObject*> array,
                                    uint32_t* index, const CharT* chars,
                                    size_t len, size_t inlineStringMaxLength,
                                    size_t inlineAtomMaxLength,
                                    const CheckString& check, gc::Heap heap) {
  auto AppendString = [&check](JSContext* cx, Handle<ArrayObject*> array,
                               uint32_t* index, HandleString s) {
    MOZ_ASSERT(check(s));
    (void)check;  // silence clang -Wunused-lambda-capture in opt builds
    RootedValue val(cx, StringValue(s));
    return JS_DefineElement(cx, array, (*index)++, val, 0);
  };

  MOZ_ASSERT(len > inlineStringMaxLength);
  MOZ_ASSERT(len > inlineAtomMaxLength);

  // Normal atom.
  RootedString atom1(cx, AtomizeChars(cx, chars, len));
  if (!atom1 || !AppendString(cx, array, index, atom1)) {
    return false;
  }
  MOZ_ASSERT(atom1->isAtom());

  // Thin inline atom.
  RootedString atom2(cx, AtomizeChars(cx, chars, 2));
  if (!atom2 || !AppendString(cx, array, index, atom2)) {
    return false;
  }
  MOZ_ASSERT(atom2->isAtom());
  MOZ_ASSERT(atom2->isInline());

  // Fat inline atom.
  RootedString atom3(cx, AtomizeChars(cx, chars, inlineAtomMaxLength));
  if (!atom3 || !AppendString(cx, array, index, atom3)) {
    return false;
  }
  MOZ_ASSERT(atom3->isAtom());
  MOZ_ASSERT_IF(inlineStringMaxLength < inlineAtomMaxLength,
                atom3->isFatInline());

  // Normal linear string; maybe nursery.
  RootedString linear1(cx, NewStringCopyN<CanGC>(cx, chars, len, heap));
  if (!linear1 || !AppendString(cx, array, index, linear1)) {
    return false;
  }
  MOZ_ASSERT(linear1->isLinear());

  // Inline string; maybe nursery.
  RootedString linear2(cx, NewStringCopyN<CanGC>(cx, chars, 3, heap));
  if (!linear2 || !AppendString(cx, array, index, linear2)) {
    return false;
  }
  MOZ_ASSERT(linear2->isLinear());
  MOZ_ASSERT(linear2->isInline());

  // Fat inline string; maybe nursery.
  RootedString linear3(
      cx, NewStringCopyN<CanGC>(cx, chars, inlineStringMaxLength, heap));
  if (!linear3 || !AppendString(cx, array, index, linear3)) {
    return false;
  }
  MOZ_ASSERT(linear3->isLinear());
  MOZ_ASSERT(linear3->isFatInline());

  // Rope; maybe nursery.
  RootedString rope(cx, ConcatStrings<CanGC>(cx, atom1, atom3, heap));
  if (!rope || !AppendString(cx, array, index, rope)) {
    return false;
  }
  MOZ_ASSERT(rope->isRope());

  // Dependent; maybe nursery.
  RootedString dep(cx, NewDependentString(cx, atom1, 0, len - 2, heap));
  if (!dep || !AppendString(cx, array, index, dep)) {
    return false;
  }
  MOZ_ASSERT(dep->isDependent());

  // Extensible; maybe nursery.
  RootedString temp1(cx, NewStringCopyN<CanGC>(cx, chars, len, heap));
  if (!temp1) {
    return false;
  }
  RootedString extensible(cx, ConcatStrings<CanGC>(cx, temp1, atom3, heap));
  if (!extensible || !extensible->ensureLinear(cx)) {
    return false;
  }
  if (!AppendString(cx, array, index, extensible)) {
    return false;
  }
  MOZ_ASSERT(extensible->isExtensible());

  RootedString external1(cx), external2(cx);
  if constexpr (std::is_same_v<CharT, char16_t>) {
    external1 = JS_NewExternalUCString(cx, (const char16_t*)chars, len,
                                       &RepresentativeExternalStringCallbacks);
    if (!external1 || !AppendString(cx, array, index, external1)) {
      return false;
    }
    MOZ_ASSERT(external1->isExternal());

    external2 = JS_NewExternalUCString(cx, (const char16_t*)chars, 2,
                                       &RepresentativeExternalStringCallbacks);
    if (!external2 || !AppendString(cx, array, index, external2)) {
      return false;
    }
    MOZ_ASSERT(external2->isExternal());
  } else {
    external1 =
        JS_NewExternalStringLatin1(cx, (const Latin1Char*)chars, len,
                                   &RepresentativeExternalStringCallbacks);
    if (!external1 || !AppendString(cx, array, index, external1)) {
      return false;
    }
    MOZ_ASSERT(external1->isExternal());

    external2 =
        JS_NewExternalStringLatin1(cx, (const Latin1Char*)chars, 2,
                                   &RepresentativeExternalStringCallbacks);
    if (!external2 || !AppendString(cx, array, index, external2)) {
      return false;
    }
    MOZ_ASSERT(external2->isExternal());
  }

  // Assert the strings still have the types we expect after creating the
  // other strings.

  MOZ_ASSERT(atom1->isAtom());
  MOZ_ASSERT(atom2->isAtom());
  MOZ_ASSERT(atom3->isAtom());
  MOZ_ASSERT(atom2->isInline());
  MOZ_ASSERT_IF(inlineStringMaxLength < inlineAtomMaxLength,
                atom3->isFatInline());

  MOZ_ASSERT(linear1->isLinear());
  MOZ_ASSERT(linear2->isLinear());
  MOZ_ASSERT(linear3->isLinear());
  MOZ_ASSERT(linear2->isInline());
  MOZ_ASSERT(linear3->isFatInline());

  MOZ_ASSERT(rope->isRope());
  MOZ_ASSERT(dep->isDependent());
  MOZ_ASSERT(extensible->isExtensible());
  MOZ_ASSERT(external1->isExternal());
  MOZ_ASSERT(external2->isExternal());
  return true;
}

/* static */
bool JSString::fillWithRepresentatives(JSContext* cx,
                                       Handle<ArrayObject*> array) {
  uint32_t index = 0;

  auto CheckTwoByte = [](JSString* str) { return str->hasTwoByteChars(); };
  auto CheckLatin1 = [](JSString* str) { return str->hasLatin1Chars(); };

  static const char16_t twoByteChars[] =
      u"\u1234abc\0def\u5678ghijklmasdfa\0xyz0123456789";
  static const Latin1Char latin1Chars[] = "abc\0defghijklmasdfa\0xyz0123456789";

  // Create strings using both the default heap and forcing the tenured heap. If
  // nursery strings are available, this is a best effort at creating them in
  // the default heap case. Since nursery strings may be disabled or a GC may
  // occur during this process, there may be duplicate representatives in the
  // final list.

  if (!FillWithRepresentatives(cx, array, &index, twoByteChars,
                               std::size(twoByteChars) - 1,
                               JSFatInlineString::MAX_LENGTH_TWO_BYTE,
                               js::FatInlineAtom::MAX_LENGTH_TWO_BYTE,
                               CheckTwoByte, gc::Heap::Tenured)) {
    return false;
  }
  if (!FillWithRepresentatives(cx, array, &index, latin1Chars,
                               std::size(latin1Chars) - 1,
                               JSFatInlineString::MAX_LENGTH_LATIN1,
                               js::FatInlineAtom::MAX_LENGTH_LATIN1,
                               CheckLatin1, gc::Heap::Tenured)) {
    return false;
  }
  if (!FillWithRepresentatives(cx, array, &index, twoByteChars,
                               std::size(twoByteChars) - 1,
                               JSFatInlineString::MAX_LENGTH_TWO_BYTE,
                               js::FatInlineAtom::MAX_LENGTH_TWO_BYTE,
                               CheckTwoByte, gc::Heap::Default)) {
    return false;
  }
  if (!FillWithRepresentatives(cx, array, &index, latin1Chars,
                               std::size(latin1Chars) - 1,
                               JSFatInlineString::MAX_LENGTH_LATIN1,
                               js::FatInlineAtom::MAX_LENGTH_LATIN1,
                               CheckLatin1, gc::Heap::Default)) {
    return false;
  }

#ifdef DEBUG
  //  * Normal atom
  //  * Inline atom.
  //  * Fat inline atom.
  //  * Normal linear string
  //  * Inline string
  //  * Fat inline string
  //  * Rope; maybe nursery.
  //  * Dependent
  //  * Extensible
  //  * External with original len
  //  * External with len==2
  static constexpr uint32_t StringTypes = 11;
  //  * Latin1
  //  * TwoByte
  static constexpr uint32_t CharTypes = 2;
  //  * Tenured
  //  * Default
  static constexpr uint32_t HeapType = 2;
  MOZ_ASSERT(index == StringTypes * CharTypes * HeapType);
#endif

  return true;
}

bool JSString::tryReplaceWithAtomRef(JSAtom* atom) {
  MOZ_ASSERT(!isAtomRef());

  if (isDependedOn() || isInline() || isExternal()) {
    return false;
  }

  AutoCheckCannotGC nogc;
  if (hasOutOfLineChars()) {
    void* buffer = asLinear().nonInlineCharsRaw();
    // This is a little cheeky and so deserves a comment. If the string is
    // not tenured, then either its buffer lives purely in the nursery, in
    // which case it will just be forgotten and blown away in the next
    // minor GC, or it is tracked in the nursery's mallocedBuffers hashtable,
    // in which case it will be freed for us in the next minor GC. We opt
    // to let the GC take care of it since there's a chance it will run
    // during idle time.
    if (isTenured()) {
      RemoveCellMemory(this, allocSize(), MemoryUse::StringContents);
      js_free(buffer);
    }
  }

  // Pre-barrier for d.s.u3 which is overwritten and d.s.u2 which is ignored
  // for atom refs.
  MOZ_ASSERT(isRope() || isLinear());
  if (isRope()) {
    PreWriteBarrier(d.s.u2.left);
    PreWriteBarrier(d.s.u3.right);
  } else if (isDependent()) {
    PreWriteBarrier(d.s.u3.base);
  }

  uint32_t flags = INIT_ATOM_REF_FLAGS;
  d.s.u3.atom = atom;
  if (atom->hasLatin1Chars()) {
    flags |= LATIN1_CHARS_BIT;
    setLengthAndFlags(length(), flags);
    setNonInlineChars(atom->chars<Latin1Char>(nogc));
  } else {
    setLengthAndFlags(length(), flags);
    setNonInlineChars(atom->chars<char16_t>(nogc));
  }
  // Redundant, but just a reminder that this needs to be true or else we need
  // to check and conditionally put ourselves in the store buffer
  MOZ_ASSERT(atom->isTenured());
  return true;
}

/*** Conversions ************************************************************/

UniqueChars js::EncodeLatin1(JSContext* cx, JSString* str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  JS::AutoCheckCannotGC nogc;
  if (linear->hasTwoByteChars()) {
    JS::Latin1CharsZ chars =
        JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, linear->twoByteRange(nogc));
    return UniqueChars(chars.c_str());
  }

  size_t len = str->length();
  Latin1Char* buf = cx->pod_malloc<Latin1Char>(len + 1);
  if (!buf) {
    return nullptr;
  }

  PodCopy(buf, linear->latin1Chars(nogc), len);
  buf[len] = '\0';

  return UniqueChars(reinterpret_cast<char*>(buf));
}

UniqueChars js::EncodeAscii(JSContext* cx, JSString* str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  MOZ_ASSERT(StringIsAscii(linear));
  return EncodeLatin1(cx, linear);
}

UniqueChars js::IdToPrintableUTF8(JSContext* cx, HandleId id,
                                  IdToPrintableBehavior behavior) {
  // ToString(<symbol>) throws a TypeError, therefore require that callers
  // request source representation when |id| is a property key.
  MOZ_ASSERT_IF(behavior == IdToPrintableBehavior::IdIsIdentifier,
                id.isAtom() && IsIdentifierNameOrPrivateName(id.toAtom()));

  RootedValue v(cx, IdToValue(id));
  JSString* str;
  if (behavior == IdToPrintableBehavior::IdIsPropertyKey) {
    str = ValueToSource(cx, v);
  } else {
    str = ToString<CanGC>(cx, v);
  }
  if (!str) {
    return nullptr;
  }
  return StringToNewUTF8CharsZ(cx, *str);
}

template <AllowGC allowGC>
JSString* js::ToStringSlow(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg) {
  /* As with ToObjectSlow, callers must verify that |arg| isn't a string. */
  MOZ_ASSERT(!arg.isString());

  Value v = arg;
  if (!v.isPrimitive()) {
    if (!allowGC) {
      return nullptr;
    }
    RootedValue v2(cx, v);
    if (!ToPrimitive(cx, JSTYPE_STRING, &v2)) {
      return nullptr;
    }
    v = v2;
  }

  JSString* str;
  if (v.isString()) {
    str = v.toString();
  } else if (v.isInt32()) {
    str = Int32ToString<allowGC>(cx, v.toInt32());
  } else if (v.isDouble()) {
    str = NumberToString<allowGC>(cx, v.toDouble());
  } else if (v.isBoolean()) {
    str = BooleanToString(cx, v.toBoolean());
  } else if (v.isNull()) {
    str = cx->names().null;
  } else if (v.isSymbol()) {
    if (allowGC) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_SYMBOL_TO_STRING);
    }
    return nullptr;
  } else if (v.isBigInt()) {
    if (!allowGC) {
      return nullptr;
    }
    RootedBigInt i(cx, v.toBigInt());
    str = BigInt::toString<CanGC>(cx, i, 10);
  }
#ifdef ENABLE_RECORD_TUPLE
  else if (v.isExtendedPrimitive()) {
    if (!allowGC) {
      return nullptr;
    }
    if (IsTuple(v)) {
      Rooted<TupleType*> tup(cx, &TupleType::thisTupleValue(v));
      return TupleToSource(cx, tup);
    }
    Rooted<RecordType*> rec(cx);
    MOZ_ALWAYS_TRUE(RecordObject::maybeUnbox(&v.getObjectPayload(), &rec));
    return RecordToSource(cx, rec);
  }
#endif
  else {
    MOZ_ASSERT(v.isUndefined());
    str = cx->names().undefined;
  }
  return str;
}

template JSString* js::ToStringSlow<CanGC>(JSContext* cx, HandleValue arg);

template JSString* js::ToStringSlow<NoGC>(JSContext* cx, const Value& arg);

JS_PUBLIC_API JSString* js::ToStringSlow(JSContext* cx, HandleValue v) {
  return ToStringSlow<CanGC>(cx, v);
}
