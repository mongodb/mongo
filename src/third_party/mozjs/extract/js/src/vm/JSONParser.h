/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSONParser_h
#define vm_JSONParser_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::{Maybe,Some}
#include "mozilla/Range.h"       // mozilla::Range
#include "mozilla/RangedPtr.h"   // mozilla::RangedPtr

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t
#include <utility>   // std::move

#include "builtin/ParseRecordObject.h"  // js::ParseRecordObject
#include "ds/IdValuePair.h"             // IdValuePair
#include "gc/GC.h"                      // AutoSelectGCHeap
#include "js/GCVector.h"                // JS::GCVector
#include "js/RootingAPI.h"  // JS::Handle, JS::MutableHandle, MutableWrappedPtrOperations
#include "js/Value.h"            // JS::Value, JS::BooleanValue, JS::NullValue
#include "js/Vector.h"           // Vector
#include "util/StringBuilder.h"  // JSStringBuilder
#include "vm/StringType.h"       // JSString, JSAtom

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

template <typename CharT, typename ParserT>
class MOZ_STACK_CLASS JSONTokenizer {
 public:
  using CharPtr = mozilla::RangedPtr<const CharT>;

  using JSONStringBuilder = typename ParserT::JSONStringBuilder;

 protected:
  CharPtr sourceStart;
  CharPtr current;
  const CharPtr begin, end;

  ParserT* parser = nullptr;

  JSONTokenizer(CharPtr sourceStart, CharPtr current, const CharPtr begin,
                const CharPtr end, ParserT* parser)
      : sourceStart(sourceStart),
        current(current),
        begin(begin),
        end(end),
        parser(parser) {
    MOZ_ASSERT(current <= end);
    MOZ_ASSERT(parser);
  }

 public:
  JSONTokenizer(CharPtr current, const CharPtr begin, const CharPtr end,
                ParserT* parser)
      : JSONTokenizer(current, current, begin, end, parser) {}

  explicit JSONTokenizer(mozilla::Range<const CharT> data, ParserT* parser)
      : JSONTokenizer(data.begin(), data.begin(), data.end(), parser) {}

  JSONTokenizer(JSONTokenizer<CharT, ParserT>&& other) noexcept
      : JSONTokenizer(other.sourceStart, other.current, other.begin, other.end,
                      other.parser) {}

  JSONTokenizer(const JSONTokenizer<CharT, ParserT>& other) = delete;
  void operator=(const JSONTokenizer<CharT, ParserT>& other) = delete;

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
  JSONToken stringToken(JSONStringBuilder& builder);

  JSONToken numberToken(double d);

  template <JSONStringType ST>
  JSONToken readString();

  JSONToken readNumber();

  void error(const char* msg);

 protected:
  inline mozilla::Span<const CharT> getSource() const {
    return mozilla::Span<const CharT>(sourceStart.get(), current.get());
  }
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
  using PropertyVector = IdValueVector;

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

    explicit StackEntry(JSContext* cx, ElementVector* elements)
        : state(JSONParserState::FinishArrayElement), vector(elements) {}

    explicit StackEntry(JSContext* cx, PropertyVector* properties)
        : state(JSONParserState::FinishObjectMember), vector(properties) {}

    JSONParserState state;

   private:
    void* vector;
  };

 public:
  /* Data members */

  JSContext* cx;

  bool reportLineNumbersFromParsedData = false;

  mozilla::Maybe<JS::ConstUTF8CharsZ> filename;

  JS::Value v;

  ParseType parseType = ParseType::JSONParse;

  AutoSelectGCHeap gcHeap;

 private:
  // Unused element and property vectors for previous in progress arrays and
  // objects. These vectors are not freed until the end of the parse to avoid
  // unnecessary freeing and allocation.
  Vector<ElementVector*, 5> freeElements;
  Vector<PropertyVector*, 5> freeProperties;

 public:
  explicit JSONFullParseHandlerAnyChar(JSContext* cx);
  ~JSONFullParseHandlerAnyChar();

  // Allow move construction for use with Rooted.
  JSONFullParseHandlerAnyChar(JSONFullParseHandlerAnyChar&& other) noexcept;

  JSONFullParseHandlerAnyChar(const JSONFullParseHandlerAnyChar& other) =
      delete;
  void operator=(const JSONFullParseHandlerAnyChar& other) = delete;

  JSContext* context() { return cx; }

  JS::Value numberValue() const {
    MOZ_ASSERT(v.isNumber());
    return v;
  }

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
  inline bool finishObjectMember(Vector<StackEntry, 10>& stack,
                                 JS::Handle<JS::Value> value,
                                 PropertyVector** properties);
  inline bool finishObject(Vector<StackEntry, 10>& stack,
                           JS::MutableHandle<JS::Value> vp,
                           PropertyVector* properties);

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements);
  inline bool arrayElement(Vector<StackEntry, 10>& stack,
                           JS::Handle<JS::Value> value,
                           ElementVector** elements);
  inline bool finishArray(Vector<StackEntry, 10>& stack,
                          JS::MutableHandle<JS::Value> vp,
                          ElementVector* elements);

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

  class JSONStringBuilder {
   public:
    JSStringBuilder buffer;

    explicit JSONStringBuilder(JSContext* cx) : buffer(cx) {}

    bool append(char16_t c);
    bool append(const CharT* begin, const CharT* end);
  };

  explicit JSONFullParseHandler(JSContext* cx) : Base(cx) {}

  JSONFullParseHandler(JSONFullParseHandler&& other) noexcept
      : Base(std::move(other)) {}

  JSONFullParseHandler(const JSONFullParseHandler& other) = delete;
  void operator=(const JSONFullParseHandler& other) = delete;

  template <JSONStringType ST>
  inline bool setStringValue(CharPtr start, size_t length,
                             mozilla::Span<const CharT>&& source);
  template <JSONStringType ST>
  inline bool setStringValue(JSONStringBuilder& builder,
                             mozilla::Span<const CharT>&& source);
  inline bool setNumberValue(double d, mozilla::Span<const CharT>&& source);
  inline bool setBooleanValue(bool value, mozilla::Span<const CharT>&& source);
  inline bool setNullValue(mozilla::Span<const CharT>&& source);

  void reportError(const char* msg, uint32_t line, uint32_t column);
};

template <typename CharT>
class MOZ_STACK_CLASS JSONReviveHandler : public JSONFullParseHandler<CharT> {
  using CharPtr = mozilla::RangedPtr<const CharT>;
  using Base = JSONFullParseHandler<CharT>;

 public:
  using SourceT = mozilla::Span<const CharT>;

  using JSONStringBuilder = typename Base::JSONStringBuilder;
  using StackEntry = typename Base::StackEntry;
  using PropertyVector = typename Base::PropertyVector;
  using ElementVector = typename Base::ElementVector;

 public:
  explicit JSONReviveHandler(JSContext* cx) : Base(cx), parseRecordStack(cx) {}

  JSONReviveHandler(JSONReviveHandler&& other) noexcept
      : Base(std::move(other)),
        parseRecordStack(std::move(other.parseRecordStack)),
        parseRecord(std::move(other.parseRecord)) {}

  JSONReviveHandler(const JSONReviveHandler& other) = delete;
  void operator=(const JSONReviveHandler& other) = delete;

  JSContext* context() { return this->cx; }

  template <JSONStringType ST>
  inline bool setStringValue(CharPtr start, size_t length, SourceT&& source) {
    if (!Base::template setStringValue<ST>(start, length,
                                           std::forward<SourceT&&>(source))) {
      return false;
    }
    return finishPrimitiveParseRecord(this->v, source);
  }

  template <JSONStringType ST>
  inline bool setStringValue(JSONStringBuilder& builder, SourceT&& source) {
    if (!Base::template setStringValue<ST>(builder,
                                           std::forward<SourceT&&>(source))) {
      return false;
    }
    return finishPrimitiveParseRecord(this->v, source);
  }

  inline bool setNumberValue(double d, SourceT&& source) {
    if (!Base::setNumberValue(d, std::forward<SourceT&&>(source))) {
      return false;
    }
    return finishPrimitiveParseRecord(this->v, source);
  }

  inline bool setBooleanValue(bool value, SourceT&& source) {
    return finishPrimitiveParseRecord(JS::BooleanValue(value), source);
  }
  inline bool setNullValue(SourceT&& source) {
    return finishPrimitiveParseRecord(JS::NullValue(), source);
  }

  inline bool objectOpen(Vector<StackEntry, 10>& stack,
                         PropertyVector** properties);
  inline bool finishObjectMember(Vector<StackEntry, 10>& stack,
                                 JS::Handle<JS::Value> value,
                                 PropertyVector** properties);
  inline bool finishObject(Vector<StackEntry, 10>& stack,
                           JS::MutableHandle<JS::Value> vp,
                           PropertyVector* properties);

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements);
  inline bool arrayElement(Vector<StackEntry, 10>& stack,
                           JS::Handle<JS::Value> value,
                           ElementVector** elements);
  inline bool finishArray(Vector<StackEntry, 10>& stack,
                          JS::MutableHandle<JS::Value> vp,
                          ElementVector* elements);

  void trace(JSTracer* trc);

 private:
  inline bool finishMemberParseRecord(
      Handle<JS::PropertyKey> key,
      Handle<ParseRecordObject::EntryMap*> parseEntry);
  inline bool finishCompoundParseRecord(
      const Value& value, Handle<ParseRecordObject::EntryMap*> parseEntry);
  inline bool finishPrimitiveParseRecord(const Value& value, SourceT source);

  GCVector<ParseRecordObject::EntryMap*, 10> parseRecordStack;

 public:
  ParseRecordObject* parseRecord = nullptr;
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

  class JSONStringBuilder {
   public:
    explicit JSONStringBuilder(FrontendContext* fc) {}

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
  inline bool setStringValue(CharPtr start, size_t length,
                             mozilla::Span<const CharT>&& source) {
    return true;
  }

  template <JSONStringType ST>
  inline bool setStringValue(JSONStringBuilder& builder,
                             mozilla::Span<const CharT>&& source) {
    return true;
  }

  inline bool setNumberValue(double d, mozilla::Span<const CharT>&& source) {
    return true;
  }
  inline bool setBooleanValue(bool value, mozilla::Span<const CharT>&& source) {
    return true;
  }
  inline bool setNullValue(mozilla::Span<const CharT>&& source) { return true; }

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
  inline bool finishObjectMember(Vector<StackEntry, 10>& stack,
                                 DummyValue& value,
                                 PropertyVector** properties) {
    return true;
  }
  inline bool finishObject(Vector<StackEntry, 10>& stack, DummyValue* vp,
                           PropertyVector* properties);

  inline bool arrayOpen(Vector<StackEntry, 10>& stack,
                        ElementVector** elements);
  inline bool arrayElement(Vector<StackEntry, 10>& stack, DummyValue& value,
                           ElementVector** elements) {
    return true;
  }
  inline bool finishArray(Vector<StackEntry, 10>& stack, DummyValue* vp,
                          ElementVector* elements);

  inline bool errorReturn() const { return false; }

  inline bool ignoreError() const { return false; }

  inline void freeStackEntry(StackEntry& entry) {}

  void reportError(const char* msg, uint32_t line, uint32_t column);
};

template <typename CharT, typename HandlerT>
class MOZ_STACK_CLASS JSONPerHandlerParser {
  using ContextT = typename HandlerT::ContextT;

  using Tokenizer = JSONTokenizer<CharT, JSONPerHandlerParser<CharT, HandlerT>>;

 public:
  using JSONStringBuilder = typename HandlerT::JSONStringBuilder;

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

  void reportLineNumbersFromParsedData(bool b) {
    this->handler.reportLineNumbersFromParsedData = b;
  }

  /**
   * Set a filename to be used in error messages.
   * This is optional and only used for error reporting.
   */
  void setFilename(JS::ConstUTF8CharsZ filename) {
    this->handler.filename = mozilla::Some(filename);
  }

  void trace(JSTracer* trc);
};

template <typename CharT>
class MOZ_STACK_CLASS JSONReviveParser
    : JSONPerHandlerParser<CharT, JSONReviveHandler<CharT>> {
  using Base = JSONPerHandlerParser<CharT, JSONReviveHandler<CharT>>;

 public:
  using ParseType = JSONFullParseHandlerAnyChar::ParseType;

  /* Public API */

  /* Create a parser for the provided JSON data. */
  JSONReviveParser(JSContext* cx, mozilla::Range<const CharT> data)
      : Base(cx, data) {}

  /* Allow move construction for use with Rooted. */
  JSONReviveParser(JSONReviveParser&& other) noexcept
      : Base(std::move(other)) {}

  JSONReviveParser(const JSONReviveParser& other) = delete;
  void operator=(const JSONReviveParser& other) = delete;

  /*
   * Parse the JSON data specified at construction time.  If it parses
   * successfully, store the prescribed value in *vp and return true.  If an
   * internal error (e.g. OOM) occurs during parsing, return false.
   * Otherwise, if invalid input was specifed but no internal error occurred,
   * behavior depends upon the error handling specified at construction: if
   * error handling is RaiseError then throw a SyntaxError and return false,
   * otherwise return true and set *vp to |undefined|.  (JSON syntax can't
   * represent |undefined|, so the JSON data couldn't have specified it.)
   *
   * If it parses successfully, parse information for calling the reviver
   * function is stored in *pro. If this function returns false, *pro will be
   * set to |undefined|.
   */
  bool parse(JS::MutableHandle<JS::Value> vp,
             JS::MutableHandle<ParseRecordObject*> pro);

  void trace(JSTracer* trc);
};

template <typename CharT, typename Wrapper>
class MutableWrappedPtrOperations<JSONParser<CharT>, Wrapper>
    : public WrappedPtrOperations<JSONParser<CharT>, Wrapper> {
 public:
  bool parse(JS::MutableHandle<JS::Value> vp) {
    return static_cast<Wrapper*>(this)->get().parse(vp);
  }
  void setFilename(JS::ConstUTF8CharsZ filename) {
    static_cast<Wrapper*>(this)->get().setFilename(filename);
  }
  void reportLineNumbersFromParsedData(bool b) {
    static_cast<Wrapper*>(this)->get().reportLineNumbersFromParsedData(b);
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
