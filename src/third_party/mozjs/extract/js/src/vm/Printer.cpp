/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Printer.h"

#include "mozilla/PodOperations.h"
#include "mozilla/Printf.h"
#include "mozilla/RangedPtr.h"

#include <stdarg.h>
#include <stdio.h>

#include "ds/LifoAlloc.h"
#include "js/CharacterEncoding.h"
#include "util/Memory.h"
#include "util/Text.h"
#include "util/WindowsWrapper.h"
#include "vm/StringType.h"

using mozilla::PodCopy;

namespace {

class GenericPrinterPrintfTarget : public mozilla::PrintfTarget {
 public:
  explicit GenericPrinterPrintfTarget(js::GenericPrinter& p) : printer(p) {}

  bool append(const char* sp, size_t len) override {
    printer.put(sp, len);
    return true;
  }

 private:
  js::GenericPrinter& printer;
};

}  // namespace

namespace js {

void GenericPrinter::reportOutOfMemory() {
  if (hadOOM_) {
    return;
  }
  hadOOM_ = true;
}

void GenericPrinter::put(mozilla::Span<const JS::Latin1Char> str) {
  if (!str.Length()) {
    return;
  }
  put(reinterpret_cast<const char*>(&str[0]), str.Length());
}

void GenericPrinter::put(mozilla::Span<const char16_t> str) {
  for (char16_t c : str) {
    putChar(c);
  }
}

void GenericPrinter::putString(JSContext* cx, JSString* str) {
  StringSegmentRange iter(cx);
  if (!iter.init(str)) {
    reportOutOfMemory();
    return;
  }
  JS::AutoCheckCannotGC nogc;
  while (!iter.empty()) {
    JSLinearString* linear = iter.front();
    if (linear->hasLatin1Chars()) {
      put(linear->latin1Range(nogc));
    } else {
      put(linear->twoByteRange(nogc));
    }
    if (!iter.popFront()) {
      reportOutOfMemory();
      return;
    }
  }
}

void GenericPrinter::printf(const char* fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
}

void GenericPrinter::vprintf(const char* fmt, va_list ap) {
  // Simple shortcut to avoid allocating strings.
  if (strchr(fmt, '%') == nullptr) {
    put(fmt);
    return;
  }

  GenericPrinterPrintfTarget printer(*this);
  (void)printer.vprint(fmt, ap);
}

const size_t StringPrinter::DefaultSize = 64;

bool StringPrinter::realloc_(size_t newSize) {
  MOZ_ASSERT(newSize > (size_t)offset);
  if (hadOOM_) {
    return false;
  }
  char* newBuf = (char*)js_arena_realloc(arena, base, newSize);
  if (!newBuf) {
    reportOutOfMemory();
    return false;
  }
  base = newBuf;
  size = newSize;
  base[size - 1] = '\0';
  return true;
}

StringPrinter::StringPrinter(arena_id_t arena, JSContext* maybeCx,
                             bool shouldReportOOM)
    : maybeCx(maybeCx),
#ifdef DEBUG
      initialized(false),
#endif
      shouldReportOOM(maybeCx && shouldReportOOM),
      base(nullptr),
      size(0),
      offset(0),
      arena(arena) {
}

StringPrinter::~StringPrinter() {
#ifdef DEBUG
  if (initialized) {
    checkInvariants();
  }
#endif
  js_free(base);
}

bool StringPrinter::init() {
  MOZ_ASSERT(!initialized);
  base = js_pod_arena_malloc<char>(arena, DefaultSize);
  if (!base) {
    reportOutOfMemory();
    forwardOutOfMemory();
    return false;
  }
#ifdef DEBUG
  initialized = true;
#endif
  *base = '\0';
  size = DefaultSize;
  base[size - 1] = '\0';
  return true;
}

void StringPrinter::checkInvariants() const {
  MOZ_ASSERT(initialized);
  MOZ_ASSERT((size_t)offset < size);
  MOZ_ASSERT(base[size - 1] == '\0');
}

UniqueChars StringPrinter::releaseChars() {
  if (hadOOM_) {
    forwardOutOfMemory();
    return nullptr;
  }

  checkInvariants();
  char* str = base;
  base = nullptr;
  offset = size = 0;
#ifdef DEBUG
  initialized = false;
#endif
  return UniqueChars(str);
}

JSString* StringPrinter::releaseJS(JSContext* cx) {
  if (hadOOM_) {
    MOZ_ASSERT_IF(maybeCx, maybeCx == cx);
    forwardOutOfMemory();
    return nullptr;
  }

  checkInvariants();

  // Extract StringPrinter data.
  size_t len = length();
  UniqueChars str(base);

  // Reset StringPrinter.
  base = nullptr;
  offset = 0;
  size = 0;
#ifdef DEBUG
  initialized = false;
#endif

  // Convert extracted data to a JSString, reusing the current buffer whenever
  // possible.
  JS::UTF8Chars utf8(str.get(), len);
  JS::SmallestEncoding encoding = JS::FindSmallestEncoding(utf8);
  if (encoding == JS::SmallestEncoding::ASCII) {
    UniqueLatin1Chars latin1(reinterpret_cast<JS::Latin1Char*>(str.release()));
    return js::NewString<js::CanGC>(cx, std::move(latin1), len);
  }

  size_t length;
  if (encoding == JS::SmallestEncoding::Latin1) {
    UniqueLatin1Chars latin1(
        UTF8CharsToNewLatin1CharsZ(cx, utf8, &length, js::StringBufferArena)
            .get());
    if (!latin1) {
      return nullptr;
    }

    return js::NewString<js::CanGC>(cx, std::move(latin1), length);
  }

  MOZ_ASSERT(encoding == JS::SmallestEncoding::UTF16);

  UniqueTwoByteChars utf16(
      UTF8CharsToNewTwoByteCharsZ(cx, utf8, &length, js::StringBufferArena)
          .get());
  if (!utf16) {
    return nullptr;
  }

  return js::NewString<js::CanGC>(cx, std::move(utf16), length);
}

char* StringPrinter::reserve(size_t len) {
  InvariantChecker ic(this);

  while (len + 1 > size - offset) { /* Include trailing \0 */
    if (!realloc_(size * 2)) {
      return nullptr;
    }
  }

  char* sb = base + offset;
  offset += len;
  return sb;
}

void StringPrinter::put(const char* s, size_t len) {
  InvariantChecker ic(this);

  const char* oldBase = base;
  const char* oldEnd = base + size;

  char* bp = reserve(len);
  if (!bp) {
    return;
  }

  // s is within the buffer already
  if (s >= oldBase && s < oldEnd) {
    // Update the source pointer in case of a realloc-ation.
    size_t index = s - oldBase;
    s = &base[index];
    memmove(bp, s, len);
  } else {
    js_memcpy(bp, s, len);
  }

  bp[len] = '\0';
}

void StringPrinter::putString(JSContext* cx, JSString* s) {
  MOZ_ASSERT(cx);
  InvariantChecker ic(this);

  JSLinearString* linear = s->ensureLinear(cx);
  if (!linear) {
    return;
  }

  size_t length = JS::GetDeflatedUTF8StringLength(linear);

  char* buffer = reserve(length);
  if (!buffer) {
    return;
  }

  mozilla::DebugOnly<size_t> written =
      JS::DeflateStringToUTF8Buffer(linear, mozilla::Span(buffer, length));
  MOZ_ASSERT(written == length);

  buffer[length] = '\0';
}

size_t StringPrinter::length() const { return size_t(offset); }

void StringPrinter::forwardOutOfMemory() {
  MOZ_ASSERT(hadOOM_);
  if (maybeCx && shouldReportOOM) {
    ReportOutOfMemory(maybeCx);
  }
}

const char js_EscapeMap[] = {
    // clang-format off
    '\b', 'b',
    '\f', 'f',
    '\n', 'n',
    '\r', 'r',
    '\t', 't',
    '\v', 'v',
    '"',  '"',
    '\'', '\'',
    '\\', '\\',
    '\0'
    // clang-format on
};

static const char JSONEscapeMap[] = {
    // clang-format off
    '\b', 'b',
    '\f', 'f',
    '\n', 'n',
    '\r', 'r',
    '\t', 't',
    '"',  '"',
    '\\', '\\',
    '\0'
    // clang-format on
};

template <QuoteTarget target, typename CharT>
JS_PUBLIC_API void QuoteString(Sprinter* sp,
                               const mozilla::Range<const CharT>& chars,
                               char quote) {
  MOZ_ASSERT_IF(target == QuoteTarget::JSON, quote == '\0');

  if (quote) {
    sp->putChar(quote);
  }
  if (target == QuoteTarget::String) {
    StringEscape esc(quote);
    EscapePrinter ep(*sp, esc);
    ep.put(chars);
  } else {
    MOZ_ASSERT(target == QuoteTarget::JSON);
    JSONEscape esc;
    EscapePrinter ep(*sp, esc);
    ep.put(chars);
  }
  if (quote) {
    sp->putChar(quote);
  }
}

template JS_PUBLIC_API void QuoteString<QuoteTarget::String, Latin1Char>(
    Sprinter* sp, const mozilla::Range<const Latin1Char>& chars, char quote);

template JS_PUBLIC_API void QuoteString<QuoteTarget::String, char16_t>(
    Sprinter* sp, const mozilla::Range<const char16_t>& chars, char quote);

template JS_PUBLIC_API void QuoteString<QuoteTarget::JSON, Latin1Char>(
    Sprinter* sp, const mozilla::Range<const Latin1Char>& chars, char quote);

template JS_PUBLIC_API void QuoteString<QuoteTarget::JSON, char16_t>(
    Sprinter* sp, const mozilla::Range<const char16_t>& chars, char quote);

JS_PUBLIC_API void QuoteString(Sprinter* sp, JSString* str,
                               char quote /*= '\0' */) {
  MOZ_ASSERT(sp->maybeCx);
  if (quote) {
    sp->putChar(quote);
  }
  StringEscape esc(quote);
  EscapePrinter ep(*sp, esc);
  ep.putString(sp->maybeCx, str);
  if (quote) {
    sp->putChar(quote);
  }
}

JS_PUBLIC_API UniqueChars QuoteString(JSContext* cx, JSString* str,
                                      char quote /* = '\0' */) {
  Sprinter sprinter(cx);
  if (!sprinter.init()) {
    return nullptr;
  }
  QuoteString(&sprinter, str, quote);
  return sprinter.release();
}

JS_PUBLIC_API void JSONQuoteString(StringPrinter* sp, JSString* str) {
  MOZ_ASSERT(sp->maybeCx);
  JSONEscape esc;
  EscapePrinter ep(*sp, esc);
  ep.putString(sp->maybeCx, str);
}

Fprinter::Fprinter(FILE* fp) : file_(nullptr), init_(false) { init(fp); }

#ifdef DEBUG
Fprinter::~Fprinter() { MOZ_ASSERT_IF(init_, !file_); }
#endif

bool Fprinter::init(const char* path) {
  MOZ_ASSERT(!file_);
  file_ = fopen(path, "w");
  if (!file_) {
    return false;
  }
  init_ = true;
  return true;
}

void Fprinter::init(FILE* fp) {
  MOZ_ASSERT(!file_);
  file_ = fp;
  init_ = false;
}

void Fprinter::flush() {
  MOZ_ASSERT(file_);
  fflush(file_);
}

void Fprinter::finish() {
  MOZ_ASSERT(file_);
  if (init_) {
    fclose(file_);
  }
  file_ = nullptr;
}

void Fprinter::put(const char* s, size_t len) {
  if (hadOutOfMemory()) {
    return;
  }

  MOZ_ASSERT(file_);
  int i = fwrite(s, /*size=*/1, /*nitems=*/len, file_);
  if (size_t(i) != len) {
    reportOutOfMemory();
    return;
  }
#ifdef XP_WIN
  if ((file_ == stderr) && (IsDebuggerPresent())) {
    UniqueChars buf = DuplicateString(s, len);
    if (!buf) {
      reportOutOfMemory();
      return;
    }
    OutputDebugStringA(buf.get());
  }
#endif
}

LSprinter::LSprinter(LifoAlloc* lifoAlloc)
    : alloc_(lifoAlloc), head_(nullptr), tail_(nullptr), unused_(0) {}

LSprinter::~LSprinter() {
  // This LSprinter might be allocated as part of the same LifoAlloc, so we
  // should not expect the destructor to be called.
}

void LSprinter::exportInto(GenericPrinter& out) const {
  if (!head_) {
    return;
  }

  for (Chunk* it = head_; it != tail_; it = it->next) {
    out.put(it->chars(), it->length);
  }
  out.put(tail_->chars(), tail_->length - unused_);
}

void LSprinter::clear() {
  head_ = nullptr;
  tail_ = nullptr;
  unused_ = 0;
  hadOOM_ = false;
}

void LSprinter::put(const char* s, size_t len) {
  if (hadOutOfMemory()) {
    return;
  }

  // Compute how much data will fit in the current chunk.
  size_t existingSpaceWrite = 0;
  size_t overflow = len;
  if (unused_ > 0 && tail_) {
    existingSpaceWrite = std::min(unused_, len);
    overflow = len - existingSpaceWrite;
  }

  // If necessary, allocate a new chunk for overflow data.
  size_t allocLength = 0;
  Chunk* last = nullptr;
  if (overflow > 0) {
    allocLength =
        AlignBytes(sizeof(Chunk) + overflow, js::detail::LIFO_ALLOC_ALIGN);

    LifoAlloc::AutoFallibleScope fallibleAllocator(alloc_);
    last = reinterpret_cast<Chunk*>(alloc_->alloc(allocLength));
    if (!last) {
      reportOutOfMemory();
      return;
    }
  }

  // All fallible operations complete: now fill up existing space, then
  // overflow space in any new chunk.
  MOZ_ASSERT(existingSpaceWrite + overflow == len);

  if (existingSpaceWrite > 0) {
    PodCopy(tail_->end() - unused_, s, existingSpaceWrite);
    unused_ -= existingSpaceWrite;
    s += existingSpaceWrite;
  }

  if (overflow > 0) {
    if (tail_ && reinterpret_cast<char*>(last) == tail_->end()) {
      // tail_ and last are consecutive in memory.  LifoAlloc has no
      // metadata and is just a bump allocator, so we can cheat by
      // appending the newly-allocated space to tail_.
      unused_ = allocLength;
      tail_->length += allocLength;
    } else {
      // Remove the size of the header from the allocated length.
      size_t availableSpace = allocLength - sizeof(Chunk);
      last->next = nullptr;
      last->length = availableSpace;

      unused_ = availableSpace;
      if (!head_) {
        head_ = last;
      } else {
        tail_->next = last;
      }

      tail_ = last;
    }

    PodCopy(tail_->end() - unused_, s, overflow);

    MOZ_ASSERT(unused_ >= overflow);
    unused_ -= overflow;
  }

  MOZ_ASSERT(len <= INT_MAX);
}

bool JSONEscape::isSafeChar(char16_t c) {
  return js::IsAsciiPrintable(c) && c != '"' && c != '\\';
}

void JSONEscape::convertInto(GenericPrinter& out, char16_t c) {
  const char* escape = nullptr;
  if (!(c >> 8) && c != 0 &&
      (escape = strchr(JSONEscapeMap, int(c))) != nullptr) {
    out.printf("\\%c", escape[1]);
  } else {
    out.printf("\\u%04X", c);
  }
}

bool StringEscape::isSafeChar(char16_t c) {
  return js::IsAsciiPrintable(c) && c != quote && c != '\\';
}

void StringEscape::convertInto(GenericPrinter& out, char16_t c) {
  const char* escape = nullptr;
  if (!(c >> 8) && c != 0 &&
      (escape = strchr(js_EscapeMap, int(c))) != nullptr) {
    out.printf("\\%c", escape[1]);
  } else {
    // Use \x only if the high byte is 0 and we're in a quoted string, because
    // ECMA-262 allows only \u, not \x, in Unicode identifiers (see bug 621814).
    out.printf(!(c >> 8) ? "\\x%02X" : "\\u%04X", c);
  }
}

void IndentedPrinter::putIndent() {
  // Allocate a static buffer of 16 spaces (plus null terminator) and use that
  // in batches for the total number of spaces we need to put.
  static const char spaceBuffer[17] = "                ";
  size_t remainingSpaces = indentLevel_ * indentAmount_;
  while (remainingSpaces > 16) {
    out_.put(spaceBuffer, 16);
    remainingSpaces -= 16;
  }
  if (remainingSpaces) {
    out_.put(spaceBuffer, remainingSpaces);
  }
}

void IndentedPrinter::putWithMaybeIndent(const char* s, size_t len) {
  if (len == 0) {
    return;
  }
  if (pendingIndent_) {
    putIndent();
    pendingIndent_ = false;
  }
  out_.put(s, len);
}

void IndentedPrinter::put(const char* s, size_t len) {
  const char* current = s;
  // Split the text into lines and output each with an indent
  while (const char* nextLineEnd = (const char*)memchr(current, '\n', len)) {
    // Put this line (including the new-line)
    size_t lineWithNewLineSize = nextLineEnd - current + 1;
    putWithMaybeIndent(current, lineWithNewLineSize);

    // The next put should have an indent added
    pendingIndent_ = true;

    // Advance the cursor
    current += lineWithNewLineSize;
    len -= lineWithNewLineSize;
  }

  // Put any remaining text
  putWithMaybeIndent(current, len);
}

}  // namespace js
