/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/JSONParser.h"

#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"

#include <ctype.h>

#include "jsarray.h"
#include "jscompartment.h"
#include "jsnum.h"
#include "jsprf.h"

#include "vm/StringBuffer.h"

#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::RangedPtr;

JSONParserBase::~JSONParserBase()
{
    for (size_t i = 0; i < stack.length(); i++) {
        if (stack[i].state == FinishArrayElement)
            js_delete(&stack[i].elements());
        else
            js_delete(&stack[i].properties());
    }

    for (size_t i = 0; i < freeElements.length(); i++)
        js_delete(freeElements[i]);

    for (size_t i = 0; i < freeProperties.length(); i++)
        js_delete(freeProperties[i]);
}

void
JSONParserBase::trace(JSTracer* trc)
{
    for (size_t i = 0; i < stack.length(); i++) {
        if (stack[i].state == FinishArrayElement) {
            ElementVector& elements = stack[i].elements();
            for (size_t j = 0; j < elements.length(); j++)
                TraceRoot(trc, &elements[j], "JSONParser element");
        } else {
            PropertyVector& properties = stack[i].properties();
            for (size_t j = 0; j < properties.length(); j++) {
                TraceRoot(trc, &properties[j].value, "JSONParser property value");
                TraceRoot(trc, &properties[j].id, "JSONParser property id");
            }
        }
    }
}

template <typename CharT>
void
JSONParser<CharT>::getTextPosition(uint32_t* column, uint32_t* line)
{
    CharPtr ptr = begin;
    uint32_t col = 1;
    uint32_t row = 1;
    for (; ptr < current; ptr++) {
        if (*ptr == '\n' || *ptr == '\r') {
            ++row;
            col = 1;
            // \r\n is treated as a single newline.
            if (ptr + 1 < current && *ptr == '\r' && *(ptr + 1) == '\n')
                ++ptr;
        } else {
            ++col;
        }
    }
    *column = col;
    *line = row;
}

template <typename CharT>
void
JSONParser<CharT>::error(const char* msg)
{
    if (errorHandling == RaiseError) {
        uint32_t column = 1, line = 1;
        getTextPosition(&column, &line);

        const size_t MaxWidth = sizeof("4294967295");
        char columnNumber[MaxWidth];
        JS_snprintf(columnNumber, sizeof columnNumber, "%lu", column);
        char lineNumber[MaxWidth];
        JS_snprintf(lineNumber, sizeof lineNumber, "%lu", line);

        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_JSON_BAD_PARSE,
                             msg, lineNumber, columnNumber);
    }
}

bool
JSONParserBase::errorReturn()
{
    return errorHandling == NoError;
}

template <typename CharT>
template <JSONParserBase::StringType ST>
JSONParserBase::Token
JSONParser<CharT>::readString()
{
    MOZ_ASSERT(current < end);
    MOZ_ASSERT(*current == '"');

    /*
     * JSONString:
     *   /^"([^\u0000-\u001F"\\]|\\(["/\\bfnrt]|u[0-9a-fA-F]{4}))*"$/
     */

    if (++current == end) {
        error("unterminated string literal");
        return token(Error);
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
            JSFlatString* str = (ST == JSONParser::PropertyName)
                                ? AtomizeChars(cx, start.get(), length)
                                : NewStringCopyN<CanGC>(cx, start.get(), length);
            if (!str)
                return token(OOM);
            return stringToken(str);
        }

        if (*current == '\\')
            break;

        if (*current <= 0x001F) {
            error("bad control character in string literal");
            return token(Error);
        }
    }

    /*
     * Slow case: string contains escaped characters.  Copy a maximal sequence
     * of unescaped characters into a temporary buffer, then an escaped
     * character, and repeat until the entire string is consumed.
     */
    StringBuffer buffer(cx);
    do {
        if (start < current && !buffer.append(start.get(), current.get()))
            return token(OOM);

        if (current >= end)
            break;

        char16_t c = *current++;
        if (c == '"') {
            JSFlatString* str = (ST == JSONParser::PropertyName)
                                ? buffer.finishAtom()
                                : buffer.finishString();
            if (!str)
                return token(OOM);
            return stringToken(str);
        }

        if (c != '\\') {
            --current;
            error("bad character in string literal");
            return token(Error);
        }

        if (current >= end)
            break;

        switch (*current++) {
          case '"':  c = '"';  break;
          case '/':  c = '/';  break;
          case '\\': c = '\\'; break;
          case 'b':  c = '\b'; break;
          case 'f':  c = '\f'; break;
          case 'n':  c = '\n'; break;
          case 'r':  c = '\r'; break;
          case 't':  c = '\t'; break;

          case 'u':
            if (end - current < 4 ||
                !(JS7_ISHEX(current[0]) &&
                  JS7_ISHEX(current[1]) &&
                  JS7_ISHEX(current[2]) &&
                  JS7_ISHEX(current[3])))
            {
                // Point to the first non-hexadecimal character (which may be
                // missing).
                if (current == end || !JS7_ISHEX(current[0]))
                    ; // already at correct location
                else if (current + 1 == end || !JS7_ISHEX(current[1]))
                    current += 1;
                else if (current + 2 == end || !JS7_ISHEX(current[2]))
                    current += 2;
                else if (current + 3 == end || !JS7_ISHEX(current[3]))
                    current += 3;
                else
                    MOZ_CRASH("logic error determining first erroneous character");

                error("bad Unicode escape");
                return token(Error);
            }
            c = (JS7_UNHEX(current[0]) << 12)
              | (JS7_UNHEX(current[1]) << 8)
              | (JS7_UNHEX(current[2]) << 4)
              | (JS7_UNHEX(current[3]));
            current += 4;
            break;

          default:
            current--;
            error("bad escaped character");
            return token(Error);
        }
        if (!buffer.append(c))
            return token(OOM);

        start = current;
        for (; current < end; current++) {
            if (*current == '"' || *current == '\\' || *current <= 0x001F)
                break;
        }
    } while (current < end);

    error("unterminated string");
    return token(Error);
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::readNumber()
{
    MOZ_ASSERT(current < end);
    MOZ_ASSERT(JS7_ISDEC(*current) || *current == '-');

    /*
     * JSONNumber:
     *   /^-?(0|[1-9][0-9]+)(\.[0-9]+)?([eE][\+\-]?[0-9]+)?$/
     */

    bool negative = *current == '-';

    /* -? */
    if (negative && ++current == end) {
        error("no number after minus sign");
        return token(Error);
    }

    const CharPtr digitStart = current;

    /* 0|[1-9][0-9]+ */
    if (!JS7_ISDEC(*current)) {
        error("unexpected non-digit");
        return token(Error);
    }
    if (*current++ != '0') {
        for (; current < end; current++) {
            if (!JS7_ISDEC(*current))
                break;
        }
    }

    /* Fast path: no fractional or exponent part. */
    if (current == end || (*current != '.' && *current != 'e' && *current != 'E')) {
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
        const CharT* dummy;
        if (!GetPrefixInteger(cx, digitStart.get(), current.get(), 10, &dummy, &d))
            return token(OOM);
        MOZ_ASSERT(current == dummy);
        return numberToken(negative ? -d : d);
    }

    /* (\.[0-9]+)? */
    if (current < end && *current == '.') {
        if (++current == end) {
            error("missing digits after decimal point");
            return token(Error);
        }
        if (!JS7_ISDEC(*current)) {
            error("unterminated fractional number");
            return token(Error);
        }
        while (++current < end) {
            if (!JS7_ISDEC(*current))
                break;
        }
    }

    /* ([eE][\+\-]?[0-9]+)? */
    if (current < end && (*current == 'e' || *current == 'E')) {
        if (++current == end) {
            error("missing digits after exponent indicator");
            return token(Error);
        }
        if (*current == '+' || *current == '-') {
            if (++current == end) {
                error("missing digits after exponent sign");
                return token(Error);
            }
        }
        if (!JS7_ISDEC(*current)) {
            error("exponent part is missing a number");
            return token(Error);
        }
        while (++current < end) {
            if (!JS7_ISDEC(*current))
                break;
        }
    }

    double d;
    const CharT* finish;
    if (!js_strtod(cx, digitStart.get(), current.get(), &finish, &d))
        return token(OOM);
    MOZ_ASSERT(current == finish);
    return numberToken(negative ? -d : d);
}

static inline bool
IsJSONWhitespace(char16_t c)
{
    return c == '\t' || c == '\r' || c == '\n' || c == ' ';
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advance()
{
    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("unexpected end of data");
        return token(Error);
    }

    switch (*current) {
      case '"':
        return readString<LiteralValue>();

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
        if (end - current < 4 || current[1] != 'r' || current[2] != 'u' || current[3] != 'e') {
            error("unexpected keyword");
            return token(Error);
        }
        current += 4;
        return token(True);

      case 'f':
        if (end - current < 5 ||
            current[1] != 'a' || current[2] != 'l' || current[3] != 's' || current[4] != 'e')
        {
            error("unexpected keyword");
            return token(Error);
        }
        current += 5;
        return token(False);

      case 'n':
        if (end - current < 4 || current[1] != 'u' || current[2] != 'l' || current[3] != 'l') {
            error("unexpected keyword");
            return token(Error);
        }
        current += 4;
        return token(Null);

      case '[':
        current++;
        return token(ArrayOpen);
      case ']':
        current++;
        return token(ArrayClose);

      case '{':
        current++;
        return token(ObjectOpen);
      case '}':
        current++;
        return token(ObjectClose);

      case ',':
        current++;
        return token(Comma);

      case ':':
        current++;
        return token(Colon);

      default:
        error("unexpected character");
        return token(Error);
    }
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advanceAfterObjectOpen()
{
    MOZ_ASSERT(current[-1] == '{');

    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("end of data while reading object contents");
        return token(Error);
    }

    if (*current == '"')
        return readString<PropertyName>();

    if (*current == '}') {
        current++;
        return token(ObjectClose);
    }

    error("expected property name or '}'");
    return token(Error);
}

template <typename CharT>
static inline void
AssertPastValue(const RangedPtr<const CharT> current)
{
    /*
     * We're past an arbitrary JSON value, so the previous character is
     * *somewhat* constrained, even if this assertion is pretty broad.  Don't
     * knock it till you tried it: this assertion *did* catch a bug once.
     */
    MOZ_ASSERT((current[-1] == 'l' &&
                current[-2] == 'l' &&
                current[-3] == 'u' &&
                current[-4] == 'n') ||
               (current[-1] == 'e' &&
                current[-2] == 'u' &&
                current[-3] == 'r' &&
                current[-4] == 't') ||
               (current[-1] == 'e' &&
                current[-2] == 's' &&
                current[-3] == 'l' &&
                current[-4] == 'a' &&
                current[-5] == 'f') ||
               current[-1] == '}' ||
               current[-1] == ']' ||
               current[-1] == '"' ||
               JS7_ISDEC(current[-1]));
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advanceAfterArrayElement()
{
    AssertPastValue(current);

    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("end of data when ',' or ']' was expected");
        return token(Error);
    }

    if (*current == ',') {
        current++;
        return token(Comma);
    }

    if (*current == ']') {
        current++;
        return token(ArrayClose);
    }

    error("expected ',' or ']' after array element");
    return token(Error);
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advancePropertyName()
{
    MOZ_ASSERT(current[-1] == ',');

    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("end of data when property name was expected");
        return token(Error);
    }

    if (*current == '"')
        return readString<PropertyName>();

    error("expected double-quoted property name");
    return token(Error);
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advancePropertyColon()
{
    MOZ_ASSERT(current[-1] == '"');

    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("end of data after property name when ':' was expected");
        return token(Error);
    }

    if (*current == ':') {
        current++;
        return token(Colon);
    }

    error("expected ':' after property name in object");
    return token(Error);
}

template <typename CharT>
JSONParserBase::Token
JSONParser<CharT>::advanceAfterProperty()
{
    AssertPastValue(current);

    while (current < end && IsJSONWhitespace(*current))
        current++;
    if (current >= end) {
        error("end of data after property value in object");
        return token(Error);
    }

    if (*current == ',') {
        current++;
        return token(Comma);
    }

    if (*current == '}') {
        current++;
        return token(ObjectClose);
    }

    error("expected ',' or '}' after property value in object");
    return token(Error);
}

inline bool
JSONParserBase::finishObject(MutableHandleValue vp, PropertyVector& properties)
{
    MOZ_ASSERT(&properties == &stack.back().properties());

    JSObject* obj = ObjectGroup::newPlainObject(cx, properties.begin(), properties.length(), GenericObject);
    if (!obj)
        return false;

    vp.setObject(*obj);
    if (!freeProperties.append(&properties))
        return false;
    stack.popBack();

    if (!stack.empty() && stack.back().state == FinishArrayElement) {
        const ElementVector& elements = stack.back().elements();
        if (!CombinePlainObjectPropertyTypes(cx, obj, elements.begin(), elements.length()))
            return false;
    }

    return true;
}

inline bool
JSONParserBase::finishArray(MutableHandleValue vp, ElementVector& elements)
{
    MOZ_ASSERT(&elements == &stack.back().elements());

    JSObject* obj = ObjectGroup::newArrayObject(cx, elements.begin(), elements.length(),
                                                GenericObject);
    if (!obj)
        return false;

    vp.setObject(*obj);
    if (!freeElements.append(&elements))
        return false;
    stack.popBack();

    if (!stack.empty() && stack.back().state == FinishArrayElement) {
        const ElementVector& elements = stack.back().elements();
        if (!CombineArrayElementTypes(cx, obj, elements.begin(), elements.length()))
            return false;
    }

    return true;
}

template <typename CharT>
bool
JSONParser<CharT>::parse(MutableHandleValue vp)
{
    RootedValue value(cx);
    MOZ_ASSERT(stack.empty());

    vp.setUndefined();

    Token token;
    ParserState state = JSONValue;
    while (true) {
        switch (state) {
          case FinishObjectMember: {
            PropertyVector& properties = stack.back().properties();
            properties.back().value = value;

            token = advanceAfterProperty();
            if (token == ObjectClose) {
                if (!finishObject(&value, properties))
                    return false;
                break;
            }
            if (token != Comma) {
                if (token == OOM)
                    return false;
                if (token != Error)
                    error("expected ',' or '}' after property-value pair in object literal");
                return errorReturn();
            }
            token = advancePropertyName();
            /* FALL THROUGH */
          }

          JSONMember:
            if (token == String) {
                jsid id = AtomToId(atomValue());
                PropertyVector& properties = stack.back().properties();
                if (!properties.append(IdValuePair(id)))
                    return false;
                token = advancePropertyColon();
                if (token != Colon) {
                    MOZ_ASSERT(token == Error);
                    return errorReturn();
                }
                goto JSONValue;
            }
            if (token == OOM)
                return false;
            if (token != Error)
                error("property names must be double-quoted strings");
            return errorReturn();

          case FinishArrayElement: {
            ElementVector& elements = stack.back().elements();
            if (!elements.append(value.get()))
                return false;
            token = advanceAfterArrayElement();
            if (token == Comma)
                goto JSONValue;
            if (token == ArrayClose) {
                if (!finishArray(&value, elements))
                    return false;
                break;
            }
            MOZ_ASSERT(token == Error);
            return errorReturn();
          }

          JSONValue:
          case JSONValue:
            token = advance();
          JSONValueSwitch:
            switch (token) {
              case String:
                value = stringValue();
                break;
              case Number:
                value = numberValue();
                break;
              case True:
                value = BooleanValue(true);
                break;
              case False:
                value = BooleanValue(false);
                break;
              case Null:
                value = NullValue();
                break;

              case ArrayOpen: {
                ElementVector* elements;
                if (!freeElements.empty()) {
                    elements = freeElements.popCopy();
                    elements->clear();
                } else {
                    elements = cx->new_<ElementVector>(cx);
                    if (!elements)
                        return false;
                }
                if (!stack.append(elements))
                    return false;

                token = advance();
                if (token == ArrayClose) {
                    if (!finishArray(&value, *elements))
                        return false;
                    break;
                }
                goto JSONValueSwitch;
              }

              case ObjectOpen: {
                PropertyVector* properties;
                if (!freeProperties.empty()) {
                    properties = freeProperties.popCopy();
                    properties->clear();
                } else {
                    properties = cx->new_<PropertyVector>(cx);
                    if (!properties)
                        return false;
                }
                if (!stack.append(properties))
                    return false;

                token = advanceAfterObjectOpen();
                if (token == ObjectClose) {
                    if (!finishObject(&value, *properties))
                        return false;
                    break;
                }
                goto JSONMember;
              }

              case ArrayClose:
              case ObjectClose:
              case Colon:
              case Comma:
                // Move the current pointer backwards so that the position
                // reported in the error message is correct.
                --current;
                error("unexpected character");
                return errorReturn();

              case OOM:
                return false;

              case Error:
                return errorReturn();
            }
            break;
        }

        if (stack.empty())
            break;
        state = stack.back().state;
    }

    for (; current < end; current++) {
        if (!IsJSONWhitespace(*current)) {
            error("unexpected non-whitespace character after JSON data");
            return errorReturn();
        }
    }

    MOZ_ASSERT(end == current);
    MOZ_ASSERT(stack.empty());

    vp.set(value);
    return true;
}

template class js::JSONParser<Latin1Char>;
template class js::JSONParser<char16_t>;
