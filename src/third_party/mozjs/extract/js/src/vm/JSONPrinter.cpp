/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/JSONPrinter.h"

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/IntegerPrintfMacros.h"

#include <stdarg.h>

#include "jsnum.h"

using namespace js;

void JSONPrinter::indent() {
  MOZ_ASSERT(indentLevel_ >= 0);

  if (inlineLevel_ > 0) {
    out_.putChar(' ');
    return;
  }

  if (indent_) {
    out_.putChar('\n');
    for (int i = 0; i < indentLevel_; i++) {
      out_.put("  ");
    }
  }
}

void JSONPrinter::beforeValue() {
  if (!first_) {
    out_.putChar(',');
  }
  indent();
}

void JSONPrinter::propertyName(const char* name) {
  beforeValue();
  out_.printf("\"%s\":", name);
  if (indent_) {
    out_.put(" ");
  }
  first_ = false;
}

void JSONPrinter::beginObject() {
  beforeValue();
  out_.putChar('{');
  indentLevel_++;
  first_ = true;
}

void JSONPrinter::beginList() {
  beforeValue();
  out_.putChar('[');
  indentLevel_++;
  first_ = true;
}

void JSONPrinter::beginObjectProperty(const char* name) {
  propertyName(name);
  out_.putChar('{');
  indentLevel_++;
  first_ = true;
}

void JSONPrinter::beginListProperty(const char* name) {
  propertyName(name);
  out_.putChar('[');
  indentLevel_++;
  first_ = true;
}

void JSONPrinter::beginInlineListProperty(const char* name) {
  beginListProperty(name);
  beginInline();
}

GenericPrinter& JSONPrinter::beginStringProperty(const char* name) {
  propertyName(name);
  out_.putChar('"');
  return out_;
}

void JSONPrinter::endStringProperty() {
  endString();
  first_ = false;
}

GenericPrinter& JSONPrinter::beginString() {
  beforeValue();
  out_.putChar('"');
  return out_;
}

void JSONPrinter::endString() { out_.putChar('"'); }

void JSONPrinter::boolProperty(const char* name, bool value) {
  propertyName(name);
  out_.put(value ? "true" : "false");
}

template <typename CharT>
static void JSONString(GenericPrinter& out, const CharT* s, size_t length) {
  const CharT* end = s + length;
  for (const CharT* t = s; t < end; s = ++t) {
    // This quote implementation is probably correct,
    // but uses \u even when not strictly necessary.
    char16_t c = *t;
    if (c == '"' || c == '\\') {
      out.printf("\\");
      out.printf("%c", char(c));
    } else if (!IsAsciiPrintable(c)) {
      out.printf("\\u%04x", c);
    } else {
      out.printf("%c", char(c));
    }
  }
}

void JSONPrinter::property(const char* name, const JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  beginStringProperty(name);

  // Limit the string length to reduce the JSON file size.
  size_t length = std::min(str->length(), size_t(128));
  if (str->hasLatin1Chars()) {
    JSONString(out_, str->latin1Chars(nogc), length);
  } else {
    JSONString(out_, str->twoByteChars(nogc), length);
  }
  endStringProperty();
}

void JSONPrinter::property(const char* name, const char* value) {
  beginStringProperty(name);
  out_.put(value);
  endStringProperty();
}

void JSONPrinter::formatProperty(const char* name, const char* format, ...) {
  va_list ap;
  va_start(ap, format);

  beginStringProperty(name);
  out_.vprintf(format, ap);
  endStringProperty();

  va_end(ap);
}

void JSONPrinter::formatPropertyVA(const char* name, const char* format,
                                   va_list ap) {
  beginStringProperty(name);
  out_.vprintf(format, ap);
  endStringProperty();
}

void JSONPrinter::value(const char* format, ...) {
  va_list ap;
  va_start(ap, format);

  beforeValue();
  out_.putChar('"');
  out_.vprintf(format, ap);
  out_.putChar('"');

  va_end(ap);
  first_ = false;
}

void JSONPrinter::property(const char* name, int32_t value) {
  propertyName(name);
  out_.printf("%" PRId32, value);
}

void JSONPrinter::value(int val) {
  beforeValue();
  out_.printf("%d", val);
  first_ = false;
}

void JSONPrinter::property(const char* name, uint32_t value) {
  propertyName(name);
  out_.printf("%" PRIu32, value);
}

void JSONPrinter::property(const char* name, int64_t value) {
  propertyName(name);
  out_.printf("%" PRId64, value);
}

void JSONPrinter::property(const char* name, uint64_t value) {
  propertyName(name);
  out_.printf("%" PRIu64, value);
}

#if defined(XP_DARWIN) || defined(__OpenBSD__) || defined(__wasi__)
void JSONPrinter::property(const char* name, size_t value) {
  propertyName(name);
  out_.printf("%zu", value);
}
#endif

void JSONPrinter::floatProperty(const char* name, double value,
                                size_t precision) {
  if (!std::isfinite(value)) {
    propertyName(name);
    out_.put("null");
    return;
  }

  ToCStringBuf cbuf;
  const char* str = NumberToCString(&cbuf, value);
  MOZ_ASSERT(str);

  property(name, str);
}

void JSONPrinter::property(const char* name, const mozilla::TimeDuration& dur,
                           TimePrecision precision) {
  if (precision == MICROSECONDS) {
    property(name, static_cast<int64_t>(dur.ToMicroseconds()));
    return;
  }

  propertyName(name);
  lldiv_t split;
  switch (precision) {
    case SECONDS:
      split = lldiv(static_cast<int64_t>(dur.ToMilliseconds()), 1000);
      break;
    case MILLISECONDS:
      split = lldiv(static_cast<int64_t>(dur.ToMicroseconds()), 1000);
      break;
    case MICROSECONDS:
      MOZ_ASSERT_UNREACHABLE("");
  };
  out_.printf("%lld.%03lld", split.quot, split.rem);
}

void JSONPrinter::nullProperty(const char* name) {
  propertyName(name);
  out_.put("null");
}

void JSONPrinter::nullValue() {
  beforeValue();
  out_.put("null");
  first_ = false;
}

void JSONPrinter::endObject() {
  indentLevel_--;
  indent();
  out_.putChar('}');
  first_ = false;
}

void JSONPrinter::endList() {
  indentLevel_--;
  indent();
  out_.putChar(']');
  first_ = false;
}

void JSONPrinter::endInlineList() {
  endList();
  endInline();
}

void JSONPrinter::beginInline() { inlineLevel_++; }

void JSONPrinter::endInline() { inlineLevel_--; }
