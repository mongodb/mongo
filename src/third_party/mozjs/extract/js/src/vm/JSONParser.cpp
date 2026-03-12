/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/JSONParser.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Range.h"       // mozilla::Range
#include "mozilla/RangedPtr.h"   // mozilla::RangedPtr

#include "mozilla/Sprintf.h"    // SprintfLiteral
#include "mozilla/TextUtils.h"  // mozilla::AsciiAlphanumericToNumber, mozilla::IsAsciiDigit, mozilla::IsAsciiHexDigit

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t
#include <utility>   // std::move

#include "jsnum.h"  // ParseDecimalNumber, GetFullInteger, FullStringToDouble

#include "builtin/Array.h"              // NewDenseCopiedArray
#include "builtin/ParseRecordObject.h"  // js::ParseRecordObject
#include "ds/IdValuePair.h"             // IdValuePair
#include "gc/GCEnum.h"                  // CanGC
#include "gc/Tracer.h"                  // JS::TraceRoot
#include "js/AllocPolicy.h"             // ReportOutOfMemory
#include "js/CharacterEncoding.h"       // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"            // JS::ColumnNumberOneOrigin
#include "js/ErrorReport.h"             // JS_ReportErrorNumberASCII
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/GCVector.h"                // JS::GCVector
#include "js/Id.h"                      // jsid
#include "js/JSON.h"                    // JS::IsValidJSON
#include "js/PropertyAndElement.h"      // JS_SetPropertyById
#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle, MutableWrappedPtrOperations
#include "js/TypeDecls.h"  // Latin1Char
#include "js/Utility.h"    // js_delete
#include "js/Value.h"  // JS::Value, JS::BooleanValue, JS::NullValue, JS::NumberValue, JS::StringValue
#include "js/Vector.h"           // Vector
#include "util/StringBuilder.h"  // JSStringBuilder
#include "vm/ArrayObject.h"      // ArrayObject
#include "vm/ErrorReporting.h"   // ReportCompileErrorLatin1, ErrorMetadata
#include "vm/JSAtomUtils.h"      // AtomizeChars
#include "vm/JSContext.h"        // JSContext
#include "vm/PlainObject.h"  // NewPlainObjectWithMaybeDuplicateKeys, NewPlainObjectWithProto
#include "vm/Realm.h"  // JS::Realm
#include "vm/StringType.h"  // JSString, JSAtom, JSLinearString, NewStringCopyN, NameToId

#include "vm/JSAtomUtils-inl.h"  // AtomToId

using namespace js;

using mozilla::AsciiAlphanumericToNumber;
using mozilla::IsAsciiDigit;
using mozilla::IsAsciiHexDigit;
using mozilla::RangedPtr;

template <typename CharT, typename ParserT>
void JSONTokenizer<CharT, ParserT>::getTextPosition(uint32_t* column,
                                                    uint32_t* line) {
  CharPtr ptr = begin;
  uint32_t col = 1;
  uint32_t row = 1;
  for (; ptr < current; ptr++) {
    if (*ptr == '\n' || *ptr == '\r') {
      ++row;
      col = 1;
      // \r\n is treated as a single newline.
      if (ptr + 1 < current && *ptr == '\r' && *(ptr + 1) == '\n') {
        ++ptr;
      }
    } else {
      ++col;
    }
  }
  *column = col;
  *line = row;
}

static inline bool IsJSONWhitespace(char16_t c) {
  return c == '\t' || c == '\r' || c == '\n' || c == ' ';
}

template <typename CharT, typename ParserT>
bool JSONTokenizer<CharT, ParserT>::consumeTrailingWhitespaces() {
  for (; current < end; current++) {
    if (!IsJSONWhitespace(*current)) {
      return false;
    }
  }
  return true;
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advance() {
  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("unexpected end of data");
    return token(JSONToken::Error);
  }

  sourceStart = current;
  switch (*current) {
    case '"':
      return readString<JSONStringType::LiteralValue>();

    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return readNumber();

    case 't':
      if (end - current < 4 || current[1] != 'r' || current[2] != 'u' ||
          current[3] != 'e') {
        error("unexpected keyword");
        return token(JSONToken::Error);
      }
      current += 4;
      if (!parser->handler.setBooleanValue(true, getSource())) {
        return token(JSONToken::OOM);
      }
      return token(JSONToken::True);

    case 'f':
      if (end - current < 5 || current[1] != 'a' || current[2] != 'l' ||
          current[3] != 's' || current[4] != 'e') {
        error("unexpected keyword");
        return token(JSONToken::Error);
      }
      current += 5;
      if (!parser->handler.setBooleanValue(false, getSource())) {
        return token(JSONToken::OOM);
      }
      return token(JSONToken::False);

    case 'n':
      if (end - current < 4 || current[1] != 'u' || current[2] != 'l' ||
          current[3] != 'l') {
        error("unexpected keyword");
        return token(JSONToken::Error);
      }
      current += 4;
      if (!parser->handler.setNullValue(getSource())) {
        return token(JSONToken::OOM);
      }
      return token(JSONToken::Null);

    case '[':
      current++;
      return token(JSONToken::ArrayOpen);
    case ']':
      current++;
      return token(JSONToken::ArrayClose);

    case '{':
      current++;
      return token(JSONToken::ObjectOpen);
    case '}':
      current++;
      return token(JSONToken::ObjectClose);

    case ',':
      current++;
      return token(JSONToken::Comma);

    case ':':
      current++;
      return token(JSONToken::Colon);

    default:
      error("unexpected character");
      return token(JSONToken::Error);
  }
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advancePropertyName() {
  MOZ_ASSERT(current[-1] == ',');

  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("end of data when property name was expected");
    return token(JSONToken::Error);
  }

  if (*current == '"') {
    return readString<JSONStringType::PropertyName>();
  }

  error("expected double-quoted property name");
  return token(JSONToken::Error);
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advancePropertyColon() {
  MOZ_ASSERT(current[-1] == '"');

  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("end of data after property name when ':' was expected");
    return token(JSONToken::Error);
  }

  if (*current == ':') {
    current++;
    return token(JSONToken::Colon);
  }

  error("expected ':' after property name in object");
  return token(JSONToken::Error);
}

template <typename CharT>
static inline void AssertPastValue(const RangedPtr<const CharT> current) {
  /*
   * We're past an arbitrary JSON value, so the previous character is
   * *somewhat* constrained, even if this assertion is pretty broad.  Don't
   * knock it till you tried it: this assertion *did* catch a bug once.
   */
  MOZ_ASSERT((current[-1] == 'l' && current[-2] == 'l' && current[-3] == 'u' &&
              current[-4] == 'n') ||
             (current[-1] == 'e' && current[-2] == 'u' && current[-3] == 'r' &&
              current[-4] == 't') ||
             (current[-1] == 'e' && current[-2] == 's' && current[-3] == 'l' &&
              current[-4] == 'a' && current[-5] == 'f') ||
             current[-1] == '}' || current[-1] == ']' || current[-1] == '"' ||
             IsAsciiDigit(current[-1]));
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advanceAfterProperty() {
  AssertPastValue(current);

  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("end of data after property value in object");
    return token(JSONToken::Error);
  }

  if (*current == ',') {
    current++;
    return token(JSONToken::Comma);
  }

  if (*current == '}') {
    current++;
    return token(JSONToken::ObjectClose);
  }

  error("expected ',' or '}' after property value in object");
  return token(JSONToken::Error);
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advanceAfterObjectOpen() {
  MOZ_ASSERT(current[-1] == '{');

  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("end of data while reading object contents");
    return token(JSONToken::Error);
  }

  if (*current == '"') {
    return readString<JSONStringType::PropertyName>();
  }

  if (*current == '}') {
    current++;
    return token(JSONToken::ObjectClose);
  }

  error("expected property name or '}'");
  return token(JSONToken::Error);
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::advanceAfterArrayElement() {
  AssertPastValue(current);

  while (current < end && IsJSONWhitespace(*current)) {
    current++;
  }
  if (current >= end) {
    error("end of data when ',' or ']' was expected");
    return token(JSONToken::Error);
  }

  if (*current == ',') {
    current++;
    return token(JSONToken::Comma);
  }

  if (*current == ']') {
    current++;
    return token(JSONToken::ArrayClose);
  }

  error("expected ',' or ']' after array element");
  return token(JSONToken::Error);
}

template <typename CharT, typename ParserT>
template <JSONStringType ST>
JSONToken JSONTokenizer<CharT, ParserT>::stringToken(const CharPtr start,
                                                     size_t length) {
  if (!parser->handler.template setStringValue<ST>(start, length,
                                                   getSource())) {
    return JSONToken::OOM;
  }
  return JSONToken::String;
}

template <typename CharT, typename ParserT>
template <JSONStringType ST>
JSONToken JSONTokenizer<CharT, ParserT>::stringToken(
    JSONStringBuilder& builder) {
  if (!parser->handler.template setStringValue<ST>(builder, getSource())) {
    return JSONToken::OOM;
  }
  return JSONToken::String;
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::numberToken(double d) {
  if (!parser->handler.setNumberValue(d, getSource())) {
    return JSONToken::OOM;
  }
  return JSONToken::Number;
}

template <typename CharT, typename ParserT>
template <JSONStringType ST>
JSONToken JSONTokenizer<CharT, ParserT>::readString() {
  MOZ_ASSERT(current < end);
  MOZ_ASSERT(*current == '"');

  /*
   * JSONString:
   *   /^"([^\u0000-\u001F"\\]|\\(["/\\bfnrt]|u[0-9a-fA-F]{4}))*"$/
   */

  if (++current == end) {
    error("unterminated string literal");
    return token(JSONToken::Error);
  }

  /*
   * Optimization: if the source contains no escaped characters, create the
   * string directly from the source text.
   */
  CharPtr start = current;
  for (; current < end; current++) {
    if (*current == '"') {
      size_t length = current - start;
      current++;
      return stringToken<ST>(start, length);
    }

    if (*current == '\\') {
      break;
    }

    if (*current <= 0x001F) {
      error("bad control character in string literal");
      return token(JSONToken::Error);
    }
  }

  /*
   * Slow case: string contains escaped characters.  Copy a maximal sequence
   * of unescaped characters into a temporary buffer, then an escaped
   * character, and repeat until the entire string is consumed.
   */
  JSONStringBuilder builder(parser->handler.context());
  do {
    if (start < current && !builder.append(start.get(), current.get())) {
      return token(JSONToken::OOM);
    }

    if (current >= end) {
      break;
    }

    char16_t c = *current++;
    if (c == '"') {
      return stringToken<ST>(builder);
    }

    if (c != '\\') {
      --current;
      error("bad character in string literal");
      return token(JSONToken::Error);
    }

    if (current >= end) {
      break;
    }

    switch (*current++) {
      case '"':
        c = '"';
        break;
      case '/':
        c = '/';
        break;
      case '\\':
        c = '\\';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;

      case 'u':
        if (end - current < 4 ||
            !(IsAsciiHexDigit(current[0]) && IsAsciiHexDigit(current[1]) &&
              IsAsciiHexDigit(current[2]) && IsAsciiHexDigit(current[3]))) {
          // Point to the first non-hexadecimal character (which may be
          // missing).
          if (current == end || !IsAsciiHexDigit(current[0])) {
            ;  // already at correct location
          } else if (current + 1 == end || !IsAsciiHexDigit(current[1])) {
            current += 1;
          } else if (current + 2 == end || !IsAsciiHexDigit(current[2])) {
            current += 2;
          } else if (current + 3 == end || !IsAsciiHexDigit(current[3])) {
            current += 3;
          } else {
            MOZ_CRASH("logic error determining first erroneous character");
          }

          error("bad Unicode escape");
          return token(JSONToken::Error);
        }
        c = (AsciiAlphanumericToNumber(current[0]) << 12) |
            (AsciiAlphanumericToNumber(current[1]) << 8) |
            (AsciiAlphanumericToNumber(current[2]) << 4) |
            (AsciiAlphanumericToNumber(current[3]));
        current += 4;
        break;

      default:
        current--;
        error("bad escaped character");
        return token(JSONToken::Error);
    }
    if (!builder.append(c)) {
      return token(JSONToken::OOM);
    }

    start = current;
    for (; current < end; current++) {
      if (*current == '"' || *current == '\\' || *current <= 0x001F) {
        break;
      }
    }
  } while (current < end);

  error("unterminated string");
  return token(JSONToken::Error);
}

template <typename CharT, typename ParserT>
JSONToken JSONTokenizer<CharT, ParserT>::readNumber() {
  MOZ_ASSERT(current < end);
  MOZ_ASSERT(IsAsciiDigit(*current) || *current == '-');

  /*
   * JSONNumber:
   *   /^-?(0|[1-9][0-9]+)(\.[0-9]+)?([eE][\+\-]?[0-9]+)?$/
   */

  bool negative = *current == '-';

  /* -? */
  if (negative && ++current == end) {
    error("no number after minus sign");
    return token(JSONToken::Error);
  }

  const CharPtr digitStart = current;

  /* 0|[1-9][0-9]+ */
  if (!IsAsciiDigit(*current)) {
    error("unexpected non-digit");
    return token(JSONToken::Error);
  }
  if (*current++ != '0') {
    for (; current < end; current++) {
      if (!IsAsciiDigit(*current)) {
        break;
      }
    }
  }

  /* Fast path: no fractional or exponent part. */
  if (current == end ||
      (*current != '.' && *current != 'e' && *current != 'E')) {
    mozilla::Range<const CharT> chars(digitStart.get(), current - digitStart);
    if (chars.length() < strlen("9007199254740992")) {
      // If the decimal number is shorter than the length of 2**53, (the
      // largest number a double can represent with integral precision),
      // parse it using a decimal-only parser.  This comparison is
      // conservative but faster than a fully-precise check.
      double d = ParseDecimalNumber(chars);
      return numberToken(negative ? -d : d);
    }

    double d;
    if (!GetFullInteger(digitStart.get(), current.get(), 10,
                        IntegerSeparatorHandling::None, &d)) {
      parser->outOfMemory();
      return token(JSONToken::OOM);
    }
    return numberToken(negative ? -d : d);
  }

  /* (\.[0-9]+)? */
  if (current < end && *current == '.') {
    if (++current == end) {
      error("missing digits after decimal point");
      return token(JSONToken::Error);
    }
    if (!IsAsciiDigit(*current)) {
      error("unterminated fractional number");
      return token(JSONToken::Error);
    }
    while (++current < end) {
      if (!IsAsciiDigit(*current)) {
        break;
      }
    }
  }

  /* ([eE][\+\-]?[0-9]+)? */
  if (current < end && (*current == 'e' || *current == 'E')) {
    if (++current == end) {
      error("missing digits after exponent indicator");
      return token(JSONToken::Error);
    }
    if (*current == '+' || *current == '-') {
      if (++current == end) {
        error("missing digits after exponent sign");
        return token(JSONToken::Error);
      }
    }
    if (!IsAsciiDigit(*current)) {
      error("exponent part is missing a number");
      return token(JSONToken::Error);
    }
    while (++current < end) {
      if (!IsAsciiDigit(*current)) {
        break;
      }
    }
  }

  double d = FullStringToDouble(digitStart.get(), current.get());
  return numberToken(negative ? -d : d);
}

template <typename CharT, typename ParserT>
void JSONTokenizer<CharT, ParserT>::error(const char* msg) {
  parser->error(msg);
}

static void ReportJSONSyntaxError(FrontendContext* fc, ErrorMetadata&& metadata,
                                  unsigned errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);

  js::ReportCompileErrorLatin1VA(fc, std::move(metadata), nullptr, errorNumber,
                                 &args);

  va_end(args);
}

// JSONFullParseHandlerAnyChar uses an AutoSelectGCHeap to switch to allocating
// in the tenured heap if we trigger more than one nursery collection.
//
// JSON parsing allocates from the leaves of the tree upwards (unlike
// structured clone deserialization which works from the root
// downwards). Because of this it doesn't necessarily make sense to stop
// nursery allocation after the first collection as this doesn't doom the
// whole data structure to being tenured. We don't know ahead of time how
// big the resulting data structure will be but after two nursery
// collections then at least half of it will end up tenured.

JSONFullParseHandlerAnyChar::JSONFullParseHandlerAnyChar(JSContext* cx)
    : cx(cx), gcHeap(cx, 1), freeElements(cx), freeProperties(cx) {}

JSONFullParseHandlerAnyChar::JSONFullParseHandlerAnyChar(
    JSONFullParseHandlerAnyChar&& other) noexcept
    : cx(other.cx),
      v(other.v),
      parseType(other.parseType),
      gcHeap(cx, 1),
      freeElements(std::move(other.freeElements)),
      freeProperties(std::move(other.freeProperties)) {}

JSONFullParseHandlerAnyChar::~JSONFullParseHandlerAnyChar() {
  for (size_t i = 0; i < freeElements.length(); i++) {
    js_delete(freeElements[i]);
  }

  for (size_t i = 0; i < freeProperties.length(); i++) {
    js_delete(freeProperties[i]);
  }
}

inline bool JSONFullParseHandlerAnyChar::objectOpen(
    Vector<StackEntry, 10>& stack, PropertyVector** properties) {
  if (!freeProperties.empty()) {
    *properties = freeProperties.popCopy();
    (*properties)->clear();
  } else {
    (*properties) = cx->new_<PropertyVector>(cx);
    if (!*properties) {
      return false;
    }
  }
  if (!stack.append(StackEntry(cx, *properties))) {
    js_delete(*properties);
    return false;
  }

  return true;
}

inline bool JSONFullParseHandlerAnyChar::objectPropertyName(
    Vector<StackEntry, 10>& stack, bool* isProtoInEval) {
  *isProtoInEval = false;
  jsid id = AtomToId(atomValue());
  if (parseType == ParseType::AttemptForEval) {
    // In |JSON.parse|, "__proto__" is a property like any other and may
    // appear multiple times. In object literal syntax, "__proto__" is
    // prototype mutation and can appear at most once. |JSONParser| only
    // supports the former semantics, so if this parse attempt is for
    // |eval|, return true (without reporting an error) to indicate the
    // JSON parse attempt was unsuccessful.
    if (id == NameToId(cx->names().proto_)) {
      *isProtoInEval = true;
      return true;
    }
  }
  PropertyVector& properties = stack.back().properties();
  if (!properties.emplaceBack(id)) {
    return false;
  }

  return true;
}

inline bool JSONFullParseHandlerAnyChar::finishObjectMember(
    Vector<StackEntry, 10>& stack, JS::Handle<JS::Value> value,
    PropertyVector** properties) {
  *properties = &stack.back().properties();
  (*properties)->back().value = value;
  return true;
}

inline bool JSONFullParseHandlerAnyChar::finishObject(
    Vector<StackEntry, 10>& stack, JS::MutableHandle<JS::Value> vp,
    PropertyVector* properties) {
  MOZ_ASSERT(properties == &stack.back().properties());

  NewObjectKind newKind = GenericObject;
  if (gcHeap == gc::Heap::Tenured) {
    newKind = TenuredObject;
  }
  // properties is traced in the parser; see JSONParser<CharT>::trace()
  JSObject* obj = NewPlainObjectWithMaybeDuplicateKeys(
      cx, Handle<IdValueVector>::fromMarkedLocation(properties), newKind);
  if (!obj) {
    return false;
  }

  vp.setObject(*obj);
  if (!freeProperties.append(properties)) {
    return false;
  }
  stack.popBack();
  return true;
}

inline bool JSONFullParseHandlerAnyChar::arrayOpen(
    Vector<StackEntry, 10>& stack, ElementVector** elements) {
  if (!freeElements.empty()) {
    *elements = freeElements.popCopy();
    (*elements)->clear();
  } else {
    (*elements) = cx->new_<ElementVector>(cx);
    if (!*elements) {
      return false;
    }
  }
  if (!stack.append(StackEntry(cx, *elements))) {
    js_delete(*elements);
    return false;
  }

  return true;
}

inline bool JSONFullParseHandlerAnyChar::arrayElement(
    Vector<StackEntry, 10>& stack, JS::Handle<JS::Value> value,
    ElementVector** elements) {
  *elements = &stack.back().elements();
  return (*elements)->append(value.get());
}

inline bool JSONFullParseHandlerAnyChar::finishArray(
    Vector<StackEntry, 10>& stack, JS::MutableHandle<JS::Value> vp,
    ElementVector* elements) {
  MOZ_ASSERT(elements == &stack.back().elements());

  NewObjectKind newKind = GenericObject;
  if (gcHeap == gc::Heap::Tenured) {
    newKind = TenuredObject;
  }
  ArrayObject* obj =
      NewDenseCopiedArray(cx, elements->length(), elements->begin(), newKind);
  if (!obj) {
    return false;
  }

  vp.setObject(*obj);
  if (!freeElements.append(elements)) {
    return false;
  }
  stack.popBack();
  return true;
}

inline void JSONFullParseHandlerAnyChar::freeStackEntry(StackEntry& entry) {
  if (entry.state == JSONParserState::FinishArrayElement) {
    js_delete(&entry.elements());
  } else {
    js_delete(&entry.properties());
  }
}

void JSONFullParseHandlerAnyChar::trace(JSTracer* trc) {
  JS::TraceRoot(trc, &v, "JSONFullParseHandlerAnyChar current value");
}

template <typename CharT>
bool JSONFullParseHandler<CharT>::JSONStringBuilder::append(char16_t c) {
  return buffer.append(c);
}

template <typename CharT>
bool JSONFullParseHandler<CharT>::JSONStringBuilder::append(const CharT* begin,
                                                            const CharT* end) {
  return buffer.append(begin, end);
}

template <typename CharT>
template <JSONStringType ST>
inline bool JSONFullParseHandler<CharT>::setStringValue(
    CharPtr start, size_t length, mozilla::Span<const CharT>&& source) {
  JSString* str;
  if constexpr (ST == JSONStringType::PropertyName) {
    str = AtomizeChars(cx, start.get(), length);
  } else {
    str = NewStringCopyN<CanGC>(cx, start.get(), length, gcHeap);
  }

  if (!str) {
    return false;
  }
  v = JS::StringValue(str);
  return true;
}

template <typename CharT>
template <JSONStringType ST>
inline bool JSONFullParseHandler<CharT>::setStringValue(
    JSONStringBuilder& builder, mozilla::Span<const CharT>&& source) {
  JSString* str;
  if constexpr (ST == JSONStringType::PropertyName) {
    str = builder.buffer.finishAtom();
  } else {
    str = builder.buffer.finishString(gcHeap);
  }

  if (!str) {
    return false;
  }
  v = JS::StringValue(str);
  return true;
}

template <typename CharT>
inline bool JSONFullParseHandler<CharT>::setNumberValue(
    double d, mozilla::Span<const CharT>&& source) {
  v = JS::NumberValue(d);
  return true;
}

template <typename CharT>
inline bool JSONFullParseHandler<CharT>::setBooleanValue(
    bool value, mozilla::Span<const CharT>&& source) {
  return true;
}

template <typename CharT>
inline bool JSONFullParseHandler<CharT>::setNullValue(
    mozilla::Span<const CharT>&& source) {
  return true;
}

template <typename CharT>
void JSONFullParseHandler<CharT>::reportError(const char* msg, uint32_t line,
                                              uint32_t column) {
  const size_t MaxWidth = sizeof("4294967295");
  char columnString[MaxWidth];
  SprintfLiteral(columnString, "%" PRIu32, column);
  char lineString[MaxWidth];
  SprintfLiteral(lineString, "%" PRIu32, line);

  if (reportLineNumbersFromParsedData) {
    AutoReportFrontendContext fc(cx);

    ErrorMetadata metadata;
    metadata.isMuted = false;
    metadata.filename = filename.valueOr(JS::ConstUTF8CharsZ(""));
    metadata.lineNumber = line;
    metadata.columnNumber = JS::ColumnNumberOneOrigin(column);

    ReportJSONSyntaxError(&fc, std::move(metadata), JSMSG_JSON_BAD_PARSE, msg,
                          lineString, columnString);
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_JSON_BAD_PARSE, msg, lineString,
                              columnString);
  }
}

template <typename CharT, typename HandlerT>
JSONPerHandlerParser<CharT, HandlerT>::~JSONPerHandlerParser() {
  for (size_t i = 0; i < stack.length(); i++) {
    handler.freeStackEntry(stack[i]);
  }
}

template <typename CharT, typename HandlerT>
template <typename TempValueT, typename ResultSetter>
bool JSONPerHandlerParser<CharT, HandlerT>::parseImpl(TempValueT& value,
                                                      ResultSetter setResult) {
  MOZ_ASSERT(stack.empty());

  JSONToken token;
  JSONParserState state = JSONParserState::JSONValue;
  while (true) {
    switch (state) {
      case JSONParserState::FinishObjectMember: {
        typename HandlerT::PropertyVector* properties;
        if (!handler.finishObjectMember(stack, value, &properties)) {
          return false;
        }

        token = tokenizer.advanceAfterProperty();
        if (token == JSONToken::ObjectClose) {
          if (!handler.finishObject(stack, &value, properties)) {
            return false;
          }
          break;
        }
        if (token != JSONToken::Comma) {
          if (token == JSONToken::OOM) {
            return false;
          }
          if (token != JSONToken::Error) {
            error(
                "expected ',' or '}' after property-value pair in object "
                "literal");
          }
          return handler.errorReturn();
        }
        token = tokenizer.advancePropertyName();
        /* FALL THROUGH */
      }

      JSONMember:
        if (token == JSONToken::String) {
          bool isProtoInEval;
          if (!handler.objectPropertyName(stack, &isProtoInEval)) {
            return false;
          }
          if (isProtoInEval) {
            // See JSONFullParseHandlerAnyChar::objectPropertyName.
            return true;
          }
          token = tokenizer.advancePropertyColon();
          if (token != JSONToken::Colon) {
            MOZ_ASSERT(token == JSONToken::Error);
            return handler.errorReturn();
          }
          goto JSONValue;
        }
        if (token == JSONToken::OOM) {
          return false;
        }
        if (token != JSONToken::Error) {
          error("property names must be double-quoted strings");
        }
        return handler.errorReturn();

      case JSONParserState::FinishArrayElement: {
        typename HandlerT::ElementVector* elements;
        if (!handler.arrayElement(stack, value, &elements)) {
          return false;
        }
        token = tokenizer.advanceAfterArrayElement();
        if (token == JSONToken::Comma) {
          goto JSONValue;
        }
        if (token == JSONToken::ArrayClose) {
          if (!handler.finishArray(stack, &value, elements)) {
            return false;
          }
          break;
        }
        MOZ_ASSERT(token == JSONToken::Error);
        return handler.errorReturn();
      }

      JSONValue:
      case JSONParserState::JSONValue:
        token = tokenizer.advance();
      JSONValueSwitch:
        switch (token) {
          case JSONToken::String:
            value = handler.stringValue();
            break;
          case JSONToken::Number:
            value = handler.numberValue();
            break;
          case JSONToken::True:
            value = handler.booleanValue(true);
            break;
          case JSONToken::False:
            value = handler.booleanValue(false);
            break;
          case JSONToken::Null:
            value = handler.nullValue();
            break;

          case JSONToken::ArrayOpen: {
            typename HandlerT::ElementVector* elements;
            if (!handler.arrayOpen(stack, &elements)) {
              return false;
            }

            token = tokenizer.advance();
            if (token == JSONToken::ArrayClose) {
              if (!handler.finishArray(stack, &value, elements)) {
                return false;
              }
              break;
            }
            goto JSONValueSwitch;
          }

          case JSONToken::ObjectOpen: {
            typename HandlerT::PropertyVector* properties;
            if (!handler.objectOpen(stack, &properties)) {
              return false;
            }

            token = tokenizer.advanceAfterObjectOpen();
            if (token == JSONToken::ObjectClose) {
              if (!handler.finishObject(stack, &value, properties)) {
                return false;
              }
              break;
            }
            goto JSONMember;
          }

          case JSONToken::ArrayClose:
          case JSONToken::ObjectClose:
          case JSONToken::Colon:
          case JSONToken::Comma:
            // Move the current pointer backwards so that the position
            // reported in the error message is correct.
            tokenizer.unget();
            error("unexpected character");
            return handler.errorReturn();

          case JSONToken::OOM:
            return false;

          case JSONToken::Error:
            return handler.errorReturn();
        }
        break;
    }

    if (stack.empty()) {
      break;
    }
    state = stack.back().state;
  }

  if (!tokenizer.consumeTrailingWhitespaces()) {
    error("unexpected non-whitespace character after JSON data");
    return handler.errorReturn();
  }

  MOZ_ASSERT(tokenizer.finished());
  MOZ_ASSERT(stack.empty());

  setResult(value);
  return true;
}

template <typename CharT, typename HandlerT>
void JSONPerHandlerParser<CharT, HandlerT>::outOfMemory() {
  ReportOutOfMemory(handler.context());
}

template <typename CharT, typename HandlerT>
void JSONPerHandlerParser<CharT, HandlerT>::error(const char* msg) {
  if (handler.ignoreError()) {
    return;
  }

  uint32_t column = 1, line = 1;
  tokenizer.getTextPosition(&column, &line);

  handler.reportError(msg, line, column);
}

template class js::JSONPerHandlerParser<Latin1Char,
                                        js::JSONFullParseHandler<Latin1Char>>;
template class js::JSONPerHandlerParser<char16_t,
                                        js::JSONFullParseHandler<char16_t>>;

template class js::JSONPerHandlerParser<Latin1Char,
                                        js::JSONReviveHandler<Latin1Char>>;
template class js::JSONPerHandlerParser<char16_t,
                                        js::JSONReviveHandler<char16_t>>;

template class js::JSONPerHandlerParser<Latin1Char,
                                        js::JSONSyntaxParseHandler<Latin1Char>>;
template class js::JSONPerHandlerParser<char16_t,
                                        js::JSONSyntaxParseHandler<char16_t>>;

template <typename CharT>
bool JSONParser<CharT>::parse(JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> tempValue(this->handler.cx);

  vp.setUndefined();

  return this->parseImpl(tempValue,
                         [&](JS::Handle<JS::Value> value) { vp.set(value); });
}

template <typename CharT>
void JSONParser<CharT>::trace(JSTracer* trc) {
  this->handler.trace(trc);

  for (auto& elem : this->stack) {
    if (elem.state == JSONParserState::FinishArrayElement) {
      elem.elements().trace(trc);
    } else {
      elem.properties().trace(trc);
    }
  }
}

template class js::JSONParser<Latin1Char>;
template class js::JSONParser<char16_t>;

template <typename CharT>
inline bool JSONReviveHandler<CharT>::objectOpen(Vector<StackEntry, 10>& stack,
                                                 PropertyVector** properties) {
  ParseRecordObject::EntryMap* newParseEntry =
      NewPlainObjectWithProto(context(), nullptr);
  if (!newParseEntry) {
    return false;
  }
  if (!parseRecordStack.append(newParseEntry)) {
    return false;
  }

  return Base::objectOpen(stack, properties);
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishObjectMember(
    Vector<StackEntry, 10>& stack, JS::Handle<JS::Value> value,
    PropertyVector** properties) {
  if (!Base::finishObjectMember(stack, value, properties)) {
    return false;
  }
  parseRecord->setValue(value);
  Rooted<JS::PropertyKey> key(context(), (*properties)->back().id);
  Rooted<ParseRecordObject::EntryMap*> parseRecordBack(context(),
                                                       parseRecordStack.back());
  return finishMemberParseRecord(key, parseRecordBack);
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishObject(
    Vector<StackEntry, 10>& stack, JS::MutableHandle<JS::Value> vp,
    PropertyVector* properties) {
  if (!Base::finishObject(stack, vp, properties)) {
    return false;
  }
  Rooted<ParseRecordObject::EntryMap*> parseRecordBack(context(),
                                                       parseRecordStack.back());
  if (!finishCompoundParseRecord(vp, parseRecordBack)) {
    return false;
  }
  parseRecordStack.popBack();

  return true;
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::arrayOpen(Vector<StackEntry, 10>& stack,
                                                ElementVector** elements) {
  ParseRecordObject::EntryMap* newParseEntry =
      NewPlainObjectWithProto(context(), nullptr);
  if (!newParseEntry) {
    return false;
  }
  if (!parseRecordStack.append(newParseEntry)) {
    return false;
  }

  return Base::arrayOpen(stack, elements);
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::arrayElement(
    Vector<StackEntry, 10>& stack, JS::Handle<JS::Value> value,
    ElementVector** elements) {
  if (!Base::arrayElement(stack, value, elements)) {
    return false;
  }
  size_t index = (*elements)->length() - 1;
  // The JSON string is limited to JS::MaxStringLength, so there should be no
  // way to get more than IntMax elements
  MOZ_ASSERT(index <= js::PropertyKey::IntMax);
  Rooted<JS::PropertyKey> key(context(), js::PropertyKey::Int(int32_t(index)));
  Rooted<ParseRecordObject::EntryMap*> parseRecordBack(context(),
                                                       parseRecordStack.back());
  return finishMemberParseRecord(key, parseRecordBack);
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishArray(
    Vector<StackEntry, 10>& stack, JS::MutableHandle<JS::Value> vp,
    ElementVector* elements) {
  if (!Base::finishArray(stack, vp, elements)) {
    return false;
  }
  Rooted<ParseRecordObject::EntryMap*> parseRecordBack(context(),
                                                       parseRecordStack.back());
  if (!finishCompoundParseRecord(vp, parseRecordBack)) {
    return false;
  }
  parseRecordStack.popBack();

  return true;
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishMemberParseRecord(
    Handle<JS::PropertyKey> key,
    Handle<ParseRecordObject::EntryMap*> parseEntry) {
  parseRecord->setKey(context(), key.get());
  Rooted<Value> pro(context(), ObjectValue(*parseRecord));
  parseRecord = nullptr;
  return JS_SetPropertyById(context(), parseEntry, key, pro);
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishCompoundParseRecord(
    const Value& value, Handle<ParseRecordObject::EntryMap*> parseEntry) {
  parseRecord = ParseRecordObject::create(context(), value);
  if (!parseRecord) {
    return false;
  }
  parseRecord->setEntries(context(), parseEntry);
  return true;
}

template <typename CharT>
inline bool JSONReviveHandler<CharT>::finishPrimitiveParseRecord(
    const Value& value, SourceT source) {
  MOZ_ASSERT(!source.IsEmpty());  // Empty source is for objects and arrays
  Rooted<JSONParseNode*> parseNode(
      context(), NewStringCopy<CanGC, CharT>(context(), source));
  if (!parseNode) {
    return false;
  }
  parseRecord = ParseRecordObject::create(context(), parseNode, value);
  return !!parseRecord;
}

template <typename CharT>
void JSONReviveHandler<CharT>::trace(JSTracer* trc) {
  Base::trace(trc);
  if (parseRecord) {
    TraceRoot(trc, &parseRecord, "parse record");
  }
  this->parseRecordStack.trace(trc);
}

template <typename CharT>
bool JSONReviveParser<CharT>::parse(JS::MutableHandle<JS::Value> vp,
                                    JS::MutableHandle<ParseRecordObject*> pro) {
  JS::Rooted<JS::Value> tempValue(this->handler.cx);

  vp.setUndefined();

  if (!this->parseImpl(tempValue,
                       [&](JS::Handle<JS::Value> value) { vp.set(value); })) {
    return false;
  }
  MOZ_ASSERT(this->handler.parseRecord);
  pro.set(this->handler.parseRecord);
  return true;
}

template <typename CharT>
void JSONReviveParser<CharT>::trace(JSTracer* trc) {
  this->handler.trace(trc);

  for (auto& elem : this->stack) {
    if (elem.state == JSONParserState::FinishArrayElement) {
      elem.elements().trace(trc);
    } else {
      elem.properties().trace(trc);
    }
  }
}

template class js::JSONReviveParser<Latin1Char>;
template class js::JSONReviveParser<char16_t>;

template <typename CharT>
inline bool JSONSyntaxParseHandler<CharT>::objectOpen(
    Vector<StackEntry, 10>& stack, PropertyVector** properties) {
  StackEntry entry{JSONParserState::FinishObjectMember};
  if (!stack.append(entry)) {
    return false;
  }
  return true;
}

template <typename CharT>
inline bool JSONSyntaxParseHandler<CharT>::finishObject(
    Vector<StackEntry, 10>& stack, DummyValue* vp, PropertyVector* properties) {
  stack.popBack();
  return true;
}

template <typename CharT>
inline bool JSONSyntaxParseHandler<CharT>::arrayOpen(
    Vector<StackEntry, 10>& stack, ElementVector** elements) {
  StackEntry entry{JSONParserState::FinishArrayElement};
  if (!stack.append(entry)) {
    return false;
  }
  return true;
}

template <typename CharT>
inline bool JSONSyntaxParseHandler<CharT>::finishArray(
    Vector<StackEntry, 10>& stack, DummyValue* vp, ElementVector* elements) {
  stack.popBack();
  return true;
}

template <typename CharT>
void JSONSyntaxParseHandler<CharT>::reportError(const char* msg, uint32_t line,
                                                uint32_t column) {
  const size_t MaxWidth = sizeof("4294967295");
  char columnString[MaxWidth];
  SprintfLiteral(columnString, "%" PRIu32, column);
  char lineString[MaxWidth];
  SprintfLiteral(lineString, "%" PRIu32, line);

  ErrorMetadata metadata;
  metadata.isMuted = false;
  metadata.filename = JS::ConstUTF8CharsZ("");
  metadata.lineNumber = 0;
  metadata.columnNumber = JS::ColumnNumberOneOrigin();

  ReportJSONSyntaxError(fc, std::move(metadata), JSMSG_JSON_BAD_PARSE, msg,
                        lineString, columnString);
}

template class js::JSONSyntaxParseHandler<Latin1Char>;
template class js::JSONSyntaxParseHandler<char16_t>;

template <typename CharT>
bool JSONSyntaxParser<CharT>::parse() {
  typename HandlerT::DummyValue unused;

  if (!this->parseImpl(unused,
                       [&](const typename HandlerT::DummyValue& unused) {})) {
    return false;
  }

  return true;
}

template class js::JSONSyntaxParser<Latin1Char>;
template class js::JSONSyntaxParser<char16_t>;

template <typename CharT>
static bool IsValidJSONImpl(const CharT* chars, uint32_t len) {
  FrontendContext fc;
  // NOTE: We don't set stack quota here because JSON parser doesn't use it.

  JSONSyntaxParser<CharT> parser(&fc, mozilla::Range(chars, len));
  if (!parser.parse()) {
    MOZ_ASSERT(fc.hadErrors());
    return false;
  }
  MOZ_ASSERT(!fc.hadErrors());

  return true;
}

JS_PUBLIC_API bool JS::IsValidJSON(const JS::Latin1Char* chars, uint32_t len) {
  return IsValidJSONImpl(chars, len);
}

JS_PUBLIC_API bool JS::IsValidJSON(const char16_t* chars, uint32_t len) {
  return IsValidJSONImpl(chars, len);
}

template <typename CharT>
class MOZ_STACK_CLASS DelegateHandler {
 private:
  using CharPtr = mozilla::RangedPtr<const CharT>;

 public:
  using ContextT = FrontendContext;

  class DummyValue {};

  struct ElementVector {};
  struct PropertyVector {};

  class JSONStringBuilder {
   public:
    StringBuilder buffer;

    explicit JSONStringBuilder(FrontendContext* fc) : buffer(fc) {}

    bool append(char16_t c) { return buffer.append(c); }
    bool append(const CharT* begin, const CharT* end) {
      return buffer.append(begin, end);
    }
  };

  struct StackEntry {
    JSONParserState state;
  };

 public:
  FrontendContext* fc;

  explicit DelegateHandler(FrontendContext* fc) : fc(fc) {}

  DelegateHandler(DelegateHandler&& other) noexcept
      : fc(other.fc), handler_(other.handler_) {}

  DelegateHandler(const DelegateHandler& other) = delete;
  void operator=(const DelegateHandler& other) = delete;

  FrontendContext* context() { return fc; }

  template <JSONStringType ST>
  inline bool setStringValue(CharPtr start, size_t length,
                             mozilla::Span<const CharT>&& source) {
    if (hadHandlerError_) {
      return false;
    }

    if constexpr (ST == JSONStringType::PropertyName) {
      return handler_->propertyName(start.get(), length);
    }

    return handler_->stringValue(start.get(), length);
  }

  template <JSONStringType ST>
  inline bool setStringValue(JSONStringBuilder& builder,
                             mozilla::Span<const CharT>&& source) {
    if (hadHandlerError_) {
      return false;
    }

    if constexpr (ST == JSONStringType::PropertyName) {
      if (builder.buffer.isUnderlyingBufferLatin1()) {
        return handler_->propertyName(builder.buffer.rawLatin1Begin(),
                                      builder.buffer.length());
      }

      return handler_->propertyName(builder.buffer.rawTwoByteBegin(),
                                    builder.buffer.length());
    }

    if (builder.buffer.isUnderlyingBufferLatin1()) {
      return handler_->stringValue(builder.buffer.rawLatin1Begin(),
                                   builder.buffer.length());
    }

    return handler_->stringValue(builder.buffer.rawTwoByteBegin(),
                                 builder.buffer.length());
  }

  inline bool setNumberValue(double d, mozilla::Span<const CharT>&& source) {
    if (hadHandlerError_) {
      return false;
    }

    if (!handler_->numberValue(d)) {
      hadHandlerError_ = true;
    }
    return !hadHandlerError_;
  }

  inline bool setBooleanValue(bool value, mozilla::Span<const CharT>&& source) {
    if (hadHandlerError_) {
      return false;
    }

    if (!handler_->booleanValue(value)) {
      hadHandlerError_ = true;
    }
    return !hadHandlerError_;
  }
  inline bool setNullValue(mozilla::Span<const CharT>&& source) {
    if (hadHandlerError_) {
      return false;
    }

    if (!handler_->nullValue()) {
      hadHandlerError_ = true;
    }
    return !hadHandlerError_;
  }

  inline DummyValue numberValue() const { return DummyValue(); }

  inline DummyValue stringValue() const { return DummyValue(); }

  inline DummyValue booleanValue(bool value) { return DummyValue(); }

  inline DummyValue nullValue() { return DummyValue(); }

  inline bool objectOpen(Vector<StackEntry, 10>& stack,
                         PropertyVector** properties) {
    if (hadHandlerError_) {
      return false;
    }

    StackEntry entry{JSONParserState::FinishObjectMember};
    if (!stack.append(entry)) {
      return false;
    }

    return handler_->startObject();
  }
  inline bool objectPropertyName(Vector<StackEntry, 10>& stack,
                                 bool* isProtoInEval) {
    *isProtoInEval = false;
    return true;
  }
  inline bool finishObjectMember(Vector<StackEntry, 10>& stack,
                                 DummyValue& value,
                                 PropertyVector** properties) {
    return true;
  }
  inline bool finishObject(Vector<StackEntry, 10>& stack, DummyValue* vp,
                           PropertyVector* properties) {
    if (hadHandlerError_) {
      return false;
    }

    stack.popBack();

    return handler_->endObject();
  }

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements) {
    if (hadHandlerError_) {
      return false;
    }

    StackEntry entry{JSONParserState::FinishArrayElement};
    if (!stack.append(entry)) {
      return false;
    }

    return handler_->startArray();
  }
  inline bool arrayElement(Vector<StackEntry, 10>& stack, DummyValue& value,
                           ElementVector** elements) {
    return true;
  }
  inline bool finishArray(Vector<StackEntry, 10>& stack, DummyValue* vp,
                          ElementVector* elements) {
    if (hadHandlerError_) {
      return false;
    }

    stack.popBack();

    return handler_->endArray();
  }

  inline bool errorReturn() const { return false; }

  inline bool ignoreError() const { return false; }

  inline void freeStackEntry(StackEntry& entry) {}

  void reportError(const char* msg, uint32_t line, uint32_t column) {
    handler_->error(msg, line, column);
  }

  void setDelegateHandler(JS::JSONParseHandler* handler) { handler_ = handler; }

 private:
  JS::JSONParseHandler* handler_ = nullptr;
  bool hadHandlerError_ = false;
};

template class DelegateHandler<Latin1Char>;
template class DelegateHandler<char16_t>;

template <typename CharT>
class MOZ_STACK_CLASS DelegateParser
    : JSONPerHandlerParser<CharT, DelegateHandler<CharT>> {
  using HandlerT = DelegateHandler<CharT>;
  using Base = JSONPerHandlerParser<CharT, HandlerT>;

 public:
  DelegateParser(FrontendContext* fc, mozilla::Range<const CharT> data,
                 JS::JSONParseHandler* handler)
      : Base(fc, data) {
    this->handler.setDelegateHandler(handler);
  }

  DelegateParser(DelegateParser<CharT>&& other) noexcept
      : Base(std::move(other)) {}

  DelegateParser(const DelegateParser& other) = delete;
  void operator=(const DelegateParser& other) = delete;

  bool parse() {
    typename HandlerT::DummyValue unused;

    if (!this->parseImpl(unused,
                         [&](const typename HandlerT::DummyValue& unused) {})) {
      return false;
    }

    return true;
  }
};

template class DelegateParser<Latin1Char>;
template class DelegateParser<char16_t>;

template <typename CharT>
static bool ParseJSONWithHandlerImpl(const CharT* chars, uint32_t len,
                                     JS::JSONParseHandler* handler) {
  FrontendContext fc;
  // NOTE: We don't set stack quota here because JSON parser doesn't use it.

  DelegateParser<CharT> parser(&fc, mozilla::Range(chars, len), handler);
  if (!parser.parse()) {
    return false;
  }
  MOZ_ASSERT(!fc.hadErrors());

  return true;
}

JS_PUBLIC_API bool JS::ParseJSONWithHandler(const JS::Latin1Char* chars,
                                            uint32_t len,
                                            JS::JSONParseHandler* handler) {
  return ParseJSONWithHandlerImpl(chars, len, handler);
}

JS_PUBLIC_API bool JS::ParseJSONWithHandler(const char16_t* chars, uint32_t len,
                                            JS::JSONParseHandler* handler) {
  return ParseJSONWithHandlerImpl(chars, len, handler);
}
