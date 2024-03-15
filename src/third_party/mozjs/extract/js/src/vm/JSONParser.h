/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSONParser_h
#define vm_JSONParser_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Range.h"       // mozilla::Range
#include "mozilla/RangedPtr.h"   // mozilla::RangedPtr

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t
#include <utility>   // std::move

#include "ds/IdValuePair.h"  // IdValuePair
#include "js/GCVector.h"     // JS::GCVector
#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle, MutableWrappedPtrOperations
#include "js/Value.h"           // JS::Value, JS::BooleanValue, JS::NullValue
#include "js/Vector.h"          // Vector
#include "util/StringBuffer.h"  // JSStringBuilder
#include "vm/StringType.h"      // JSString, JSAtom

struct JSContext;
class JSTracer;

namespace js {

class FrontendContext;

enum class JSONToken {
  String,
  Number,
  True,
  False,
  Null,
  ArrayOpen,
  ArrayClose,
  ObjectOpen,
  ObjectClose,
  Colon,
  Comma,
  OOM,
  Error
};

enum class JSONStringType { PropertyName, LiteralValue };

template <typename CharT, typename ParserT, typename StringBuilderT>
class MOZ_STACK_CLASS JSONTokenizer {
 public:
  using CharPtr = mozilla::RangedPtr<const CharT>;

 protected:
  CharPtr current;
  const CharPtr begin, end;

  ParserT* parser = nullptr;

 public:
  JSONTokenizer(CharPtr current, const CharPtr begin, const CharPtr end,
                ParserT* parser)
      : current(current), begin(begin), end(end), parser(parser) {
    MOZ_ASSERT(current <= end);
    MOZ_ASSERT(parser);
  }

  explicit JSONTokenizer(mozilla::Range<const CharT> data, ParserT* parser)
      : JSONTokenizer(data.begin(), data.begin(), data.end(), parser) {}

  JSONTokenizer(JSONTokenizer<CharT, ParserT, StringBuilderT>&& other) noexcept
      : JSONTokenizer(other.current, other.begin, other.end, other.parser) {}

  JSONTokenizer(const JSONTokenizer<CharT, ParserT, StringBuilderT>& other) =
      delete;
  void operator=(const JSONTokenizer<CharT, ParserT, StringBuilderT>& other) =
      delete;

  void fixupParser(ParserT* newParser) { parser = newParser; }

  void getTextPosition(uint32_t* column, uint32_t* line);

  bool consumeTrailingWhitespaces();

  JSONToken advance();
  JSONToken advancePropertyName();
  JSONToken advancePropertyColon();
  JSONToken advanceAfterProperty();
  JSONToken advanceAfterObjectOpen();
  JSONToken advanceAfterArrayElement();

  void unget() { --current; }

#ifdef DEBUG
  bool finished() { return end == current; }
#endif

  JSONToken token(JSONToken t) {
    MOZ_ASSERT(t != JSONToken::String);
    MOZ_ASSERT(t != JSONToken::Number);
    return t;
  }

  template <JSONStringType ST>
  JSONToken stringToken(const CharPtr start, size_t length);
  template <JSONStringType ST>
  JSONToken stringToken(StringBuilderT& builder);

  JSONToken numberToken(double d);

  template <JSONStringType ST>
  JSONToken readString();

  JSONToken readNumber();

  void error(const char* msg);
};

// Possible states the parser can be in between values.
enum class JSONParserState {
  // An array element has just being parsed.
  FinishArrayElement,

  // An object property has just been parsed.
  FinishObjectMember,

  // At the start of the parse, before any values have been processed.
  JSONValue
};

// Character-type-agnostic base class for JSONFullParseHandler.
// JSONParser is templatized to work on either Latin1
// or TwoByte input strings, JSONFullParseHandlerAnyChar holds all state and
// methods that can be shared between the two encodings.
class MOZ_STACK_CLASS JSONFullParseHandlerAnyChar {
 public:
  // State related to the parser's current position. At all points in the
  // parse this keeps track of the stack of arrays and objects which have
  // been started but not finished yet. The actual JS object is not
  // allocated until the literal is closed, so that the result can be sized
  // according to its contents and have its type and shape filled in using
  // caches.

  // State for an array that is currently being parsed. This includes all
  // elements that have been seen so far.
  using ElementVector = JS::GCVector<JS::Value, 20>;

  // State for an object that is currently being parsed. This includes all
  // the key/value pairs that have been seen so far.
  using PropertyVector = JS::GCVector<IdValuePair, 10>;

  enum class ParseType {
    // Parsing a string as if by JSON.parse.
    JSONParse,
    // Parsing what may or may not be JSON in a string of eval code.
    // In this case, a failure to parse indicates either syntax that isn't JSON,
    // or syntax that has different semantics in eval code than in JSON.
    AttemptForEval,
  };

  // Stack element for an in progress array or object.
  struct StackEntry {
    ElementVector& elements() {
      MOZ_ASSERT(state == JSONParserState::FinishArrayElement);
      return *static_cast<ElementVector*>(vector);
    }

    PropertyVector& properties() {
      MOZ_ASSERT(state == JSONParserState::FinishObjectMember);
      return *static_cast<PropertyVector*>(vector);
    }

    explicit StackEntry(ElementVector* elements)
        : state(JSONParserState::FinishArrayElement), vector(elements) {}

    explicit StackEntry(PropertyVector* properties)
        : state(JSONParserState::FinishObjectMember), vector(properties) {}

    JSONParserState state;

   private:
    void* vector;
  };

 public:
  /* Data members */

  JSContext* cx;

  JS::Value v;

  ParseType parseType = ParseType::JSONParse;

 private:
  // Unused element and property vectors for previous in progress arrays and
  // objects. These vectors are not freed until the end of the parse to avoid
  // unnecessary freeing and allocation.
  Vector<ElementVector*, 5> freeElements;
  Vector<PropertyVector*, 5> freeProperties;

 public:
  explicit JSONFullParseHandlerAnyChar(JSContext* cx)
      : cx(cx), freeElements(cx), freeProperties(cx) {}
  ~JSONFullParseHandlerAnyChar();

  // Allow move construction for use with Rooted.
  JSONFullParseHandlerAnyChar(JSONFullParseHandlerAnyChar&& other) noexcept
      : cx(other.cx),
        v(other.v),
        parseType(other.parseType),
        freeElements(std::move(other.freeElements)),
        freeProperties(std::move(other.freeProperties)) {}

  JSONFullParseHandlerAnyChar(const JSONFullParseHandlerAnyChar& other) =
      delete;
  void operator=(const JSONFullParseHandlerAnyChar& other) = delete;

  JSContext* context() { return cx; }

  JS::Value numberValue() const {
    MOZ_ASSERT(v.isNumber());
    return v;
  }

  inline void setNumberValue(double d);

  JS::Value stringValue() const {
    MOZ_ASSERT(v.isString());
    return v;
  }

  JSAtom* atomValue() const {
    JS::Value strval = stringValue();
    return &strval.toString()->asAtom();
  }

  inline JS::Value booleanValue(bool value) { return JS::BooleanValue(value); }
  inline JS::Value nullValue() { return JS::NullValue(); }

  inline bool objectOpen(Vector<StackEntry, 10>& stack,
                         PropertyVector** properties);
  inline bool objectPropertyName(Vector<StackEntry, 10>& stack,
                                 bool* isProtoInEval);
  inline void finishObjectMember(Vector<StackEntry, 10>& stack,
                                 JS::Handle<JS::Value> value,
                                 PropertyVector** properties);
  inline bool finishObject(Vector<StackEntry, 10>& stack,
                           JS::MutableHandle<JS::Value> vp,
                           PropertyVector& properties);

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements);
  inline bool arrayElement(Vector<StackEntry, 10>& stack,
                           JS::Handle<JS::Value> value,
                           ElementVector** elements);
  inline bool finishArray(Vector<StackEntry, 10>& stack,
                          JS::MutableHandle<JS::Value> vp,
                          ElementVector& elements);

  inline bool errorReturn() const {
    return parseType == ParseType::AttemptForEval;
  }

  inline bool ignoreError() const {
    return parseType == ParseType::AttemptForEval;
  }

  inline void freeStackEntry(StackEntry& entry);

  void trace(JSTracer* trc);
};

template <typename CharT>
class MOZ_STACK_CLASS JSONFullParseHandler
    : public JSONFullParseHandlerAnyChar {
  using Base = JSONFullParseHandlerAnyChar;
  using CharPtr = mozilla::RangedPtr<const CharT>;

 public:
  using ContextT = JSContext;

  class StringBuilder {
   public:
    JSStringBuilder buffer;

    explicit StringBuilder(JSContext* cx) : buffer(cx) {}

    bool append(char16_t c);
    bool append(const CharT* begin, const CharT* end);
  };

  explicit JSONFullParseHandler(JSContext* cx) : Base(cx) {}

  JSONFullParseHandler(JSONFullParseHandler&& other) noexcept
      : Base(std::move(other)) {}

  JSONFullParseHandler(const JSONFullParseHandler& other) = delete;
  void operator=(const JSONFullParseHandler& other) = delete;

  template <JSONStringType ST>
  inline bool setStringValue(CharPtr start, size_t length);
  template <JSONStringType ST>
  inline bool setStringValue(StringBuilder& builder);

  void reportError(const char* msg, const char* lineString,
                   const char* columnString);
};

template <typename CharT>
class MOZ_STACK_CLASS JSONSyntaxParseHandler {
 private:
  using CharPtr = mozilla::RangedPtr<const CharT>;

 public:
  /* Types for templatized parser. */

  using ContextT = FrontendContext;

  class DummyValue {};

  struct ElementVector {};
  struct PropertyVector {};

  class StringBuilder {
   public:
    explicit StringBuilder(FrontendContext* fc) {}

    bool append(char16_t c) { return true; }
    bool append(const CharT* begin, const CharT* end) { return true; }
  };

  struct StackEntry {
    JSONParserState state;
  };

 public:
  FrontendContext* fc;

  /* Public API */

  /* Create a parser for the provided JSON data. */
  explicit JSONSyntaxParseHandler(FrontendContext* fc) : fc(fc) {}

  JSONSyntaxParseHandler(JSONSyntaxParseHandler&& other) noexcept
      : fc(other.fc) {}

  JSONSyntaxParseHandler(const JSONSyntaxParseHandler& other) = delete;
  void operator=(const JSONSyntaxParseHandler& other) = delete;

  FrontendContext* context() { return fc; }

  template <JSONStringType ST>
  inline bool setStringValue(CharPtr start, size_t length) {
    return true;
  }

  template <JSONStringType ST>
  inline bool setStringValue(StringBuilder& builder) {
    return true;
  }

  inline void setNumberValue(double d) {}

  inline DummyValue numberValue() const { return DummyValue(); }

  inline DummyValue stringValue() const { return DummyValue(); }

  inline DummyValue booleanValue(bool value) { return DummyValue(); }
  inline DummyValue nullValue() { return DummyValue(); }

  inline bool objectOpen(Vector<StackEntry, 10>& stack,
                         PropertyVector** properties);
  inline bool objectPropertyName(Vector<StackEntry, 10>& stack,
                                 bool* isProtoInEval) {
    *isProtoInEval = false;
    return true;
  }
  inline void finishObjectMember(Vector<StackEntry, 10>& stack,
                                 DummyValue& value,
                                 PropertyVector** properties) {}
  inline bool finishObject(Vector<StackEntry, 10>& stack, DummyValue* vp,
                           PropertyVector& properties);

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements);
  inline bool arrayElement(Vector<StackEntry, 10>& stack, DummyValue& value,
                           ElementVector** elements) {
    return true;
  }
  inline bool finishArray(Vector<StackEntry, 10>& stack, DummyValue* vp,
                          ElementVector& elements);

  inline bool errorReturn() const { return false; }

  inline bool ignoreError() const { return false; }

  inline void freeStackEntry(StackEntry& entry) {}

  void reportError(const char* msg, const char* lineString,
                   const char* columnString);
};

template <typename CharT, typename HandlerT>
class MOZ_STACK_CLASS JSONPerHandlerParser {
  using ContextT = typename HandlerT::ContextT;

  using Tokenizer = JSONTokenizer<CharT, JSONPerHandlerParser<CharT, HandlerT>,
                                  typename HandlerT::StringBuilder>;

 public:
  using StringBuilder = typename HandlerT::StringBuilder;

 public:
  HandlerT handler;
  Tokenizer tokenizer;

  // All in progress arrays and objects being parsed, in order from outermost
  // to innermost.
  Vector<typename HandlerT::StackEntry, 10> stack;

 public:
  JSONPerHandlerParser(ContextT* context, mozilla::Range<const CharT> data)
      : handler(context), tokenizer(data, this), stack(context) {}

  JSONPerHandlerParser(JSONPerHandlerParser&& other) noexcept
      : handler(std::move(other.handler)),
        tokenizer(std::move(other.tokenizer)),
        stack(handler.context()) {
    tokenizer.fixupParser(this);
  }

  ~JSONPerHandlerParser();

  JSONPerHandlerParser(const JSONPerHandlerParser<CharT, HandlerT>& other) =
      delete;
  void operator=(const JSONPerHandlerParser<CharT, HandlerT>& other) = delete;

  template <typename TempValueT, typename ResultSetter>
  inline bool parseImpl(TempValueT& value, ResultSetter setResult);

  void outOfMemory();

  void error(const char* msg);
};

template <typename CharT>
class MOZ_STACK_CLASS JSONParser
    : JSONPerHandlerParser<CharT, JSONFullParseHandler<CharT>> {
  using Base = JSONPerHandlerParser<CharT, JSONFullParseHandler<CharT>>;

 public:
  using ParseType = JSONFullParseHandlerAnyChar::ParseType;

  /* Public API */

  /* Create a parser for the provided JSON data. */
  JSONParser(JSContext* cx, mozilla::Range<const CharT> data,
             ParseType parseType)
      : Base(cx, data) {
    this->handler.parseType = parseType;
  }

  /* Allow move construction for use with Rooted. */
  JSONParser(JSONParser&& other) noexcept : Base(std::move(other)) {}

  JSONParser(const JSONParser& other) = delete;
  void operator=(const JSONParser& other) = delete;

  /*
   * Parse the JSON data specified at construction time.  If it parses
   * successfully, store the prescribed value in *vp and return true.  If an
   * internal error (e.g. OOM) occurs during parsing, return false.
   * Otherwise, if invalid input was specifed but no internal error occurred,
   * behavior depends upon the error handling specified at construction: if
   * error handling is RaiseError then throw a SyntaxError and return false,
   * otherwise return true and set *vp to |undefined|.  (JSON syntax can't
   * represent |undefined|, so the JSON data couldn't have specified it.)
   */
  bool parse(JS::MutableHandle<JS::Value> vp);

  void trace(JSTracer* trc);
};

template <typename CharT, typename Wrapper>
class MutableWrappedPtrOperations<JSONParser<CharT>, Wrapper>
    : public WrappedPtrOperations<JSONParser<CharT>, Wrapper> {
 public:
  bool parse(JS::MutableHandle<JS::Value> vp) {
    return static_cast<Wrapper*>(this)->get().parse(vp);
  }
};

template <typename CharT>
class MOZ_STACK_CLASS JSONSyntaxParser
    : JSONPerHandlerParser<CharT, JSONSyntaxParseHandler<CharT>> {
  using HandlerT = JSONSyntaxParseHandler<CharT>;
  using Base = JSONPerHandlerParser<CharT, HandlerT>;

 public:
  JSONSyntaxParser(FrontendContext* fc, mozilla::Range<const CharT> data)
      : Base(fc, data) {}

  JSONSyntaxParser(JSONSyntaxParser<CharT>&& other) noexcept
      : Base(std::move(other)) {}

  JSONSyntaxParser(const JSONSyntaxParser& other) = delete;
  void operator=(const JSONSyntaxParser& other) = delete;

  bool parse();
};

} /* namespace js */

#endif /* vm_JSONParser_h */
