/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSONParser_h
#define vm_JSONParser_h

#include "mozilla/Attributes.h"
#include "mozilla/Range.h"

#include "jspubtd.h"

#include "ds/IdValuePair.h"
#include "vm/String.h"

namespace js {

// JSONParser base class. JSONParser is templatized to work on either Latin1
// or TwoByte input strings, JSONParserBase holds all state and methods that
// can be shared between the two encodings.
class MOZ_STACK_CLASS JSONParserBase
{
  public:
    enum ErrorHandling { RaiseError, NoError };

  private:
    /* Data members */
    Value v;

  protected:
    JSContext * const cx;

    const ErrorHandling errorHandling;

    enum Token { String, Number, True, False, Null,
                 ArrayOpen, ArrayClose,
                 ObjectOpen, ObjectClose,
                 Colon, Comma,
                 OOM, Error };

    // State related to the parser's current position. At all points in the
    // parse this keeps track of the stack of arrays and objects which have
    // been started but not finished yet. The actual JS object is not
    // allocated until the literal is closed, so that the result can be sized
    // according to its contents and have its type and shape filled in using
    // caches.

    // State for an array that is currently being parsed. This includes all
    // elements that have been seen so far.
    typedef Vector<Value, 20> ElementVector;

    // State for an object that is currently being parsed. This includes all
    // the key/value pairs that have been seen so far.
    typedef Vector<IdValuePair, 10> PropertyVector;

    // Possible states the parser can be in between values.
    enum ParserState {
        // An array element has just being parsed.
        FinishArrayElement,

        // An object property has just been parsed.
        FinishObjectMember,

        // At the start of the parse, before any values have been processed.
        JSONValue
    };

    // Stack element for an in progress array or object.
    struct StackEntry {
        ElementVector& elements() {
            MOZ_ASSERT(state == FinishArrayElement);
            return * static_cast<ElementVector*>(vector);
        }

        PropertyVector& properties() {
            MOZ_ASSERT(state == FinishObjectMember);
            return * static_cast<PropertyVector*>(vector);
        }

        explicit StackEntry(ElementVector* elements)
          : state(FinishArrayElement), vector(elements)
        {}

        explicit StackEntry(PropertyVector* properties)
          : state(FinishObjectMember), vector(properties)
        {}

        ParserState state;

      private:
        void* vector;
    };

    // All in progress arrays and objects being parsed, in order from outermost
    // to innermost.
    Vector<StackEntry, 10> stack;

    // Unused element and property vectors for previous in progress arrays and
    // objects. These vectors are not freed until the end of the parse to avoid
    // unnecessary freeing and allocation.
    Vector<ElementVector*, 5> freeElements;
    Vector<PropertyVector*, 5> freeProperties;

#ifdef DEBUG
    Token lastToken;
#endif

    JSONParserBase(JSContext* cx, ErrorHandling errorHandling)
      : cx(cx),
        errorHandling(errorHandling),
        stack(cx),
        freeElements(cx),
        freeProperties(cx)
#ifdef DEBUG
      , lastToken(Error)
#endif
    {}
    ~JSONParserBase();

    // Allow move construction for use with Rooted.
    JSONParserBase(JSONParserBase&& other)
      : v(other.v),
        cx(other.cx),
        errorHandling(other.errorHandling),
        stack(mozilla::Move(other.stack)),
        freeElements(mozilla::Move(other.freeElements)),
        freeProperties(mozilla::Move(other.freeProperties))
#ifdef DEBUG
      , lastToken(mozilla::Move(other.lastToken))
#endif
    {}


    Value numberValue() const {
        MOZ_ASSERT(lastToken == Number);
        MOZ_ASSERT(v.isNumber());
        return v;
    }

    Value stringValue() const {
        MOZ_ASSERT(lastToken == String);
        MOZ_ASSERT(v.isString());
        return v;
    }

    JSAtom* atomValue() const {
        Value strval = stringValue();
        return &strval.toString()->asAtom();
    }

    Token token(Token t) {
        MOZ_ASSERT(t != String);
        MOZ_ASSERT(t != Number);
#ifdef DEBUG
        lastToken = t;
#endif
        return t;
    }

    Token stringToken(JSString* str) {
        this->v = StringValue(str);
#ifdef DEBUG
        lastToken = String;
#endif
        return String;
    }

    Token numberToken(double d) {
        this->v = NumberValue(d);
#ifdef DEBUG
        lastToken = Number;
#endif
        return Number;
    }

    enum StringType { PropertyName, LiteralValue };

    bool errorReturn();

    bool finishObject(MutableHandleValue vp, PropertyVector& properties);
    bool finishArray(MutableHandleValue vp, ElementVector& elements);

    void trace(JSTracer* trc);

  private:
    JSONParserBase(const JSONParserBase& other) = delete;
    void operator=(const JSONParserBase& other) = delete;
};

template <typename CharT>
class MOZ_STACK_CLASS JSONParser : public JSONParserBase,
                                   public JS::Traceable
{
  private:
    typedef mozilla::RangedPtr<const CharT> CharPtr;

    CharPtr current;
    const CharPtr begin, end;

  public:
    /* Public API */

    /* Create a parser for the provided JSON data. */
    JSONParser(JSContext* cx, mozilla::Range<const CharT> data,
               ErrorHandling errorHandling = RaiseError)
      : JSONParserBase(cx, errorHandling),
        current(data.start()),
        begin(current),
        end(data.end())
    {
        MOZ_ASSERT(current <= end);
    }

    /* Allow move construction for use with Rooted. */
    JSONParser(JSONParser&& other)
      : JSONParserBase(mozilla::Forward<JSONParser>(other)),
        current(other.current),
        begin(other.begin),
        end(other.end)
    {}

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
    bool parse(MutableHandleValue vp);

    static void trace(JSONParser<CharT>* parser, JSTracer* trc) { parser->trace(trc); }
    void trace(JSTracer* trc) { JSONParserBase::trace(trc); }

  private:
    template<StringType ST> Token readString();

    Token readNumber();

    Token advance();
    Token advancePropertyName();
    Token advancePropertyColon();
    Token advanceAfterProperty();
    Token advanceAfterObjectOpen();
    Token advanceAfterArrayElement();

    void error(const char* msg);

    void getTextPosition(uint32_t* column, uint32_t* line);

  private:
    JSONParser(const JSONParser& other) = delete;
    void operator=(const JSONParser& other) = delete;
};

template <typename CharT>
struct RootedBase<JSONParser<CharT>> {
    bool parse(MutableHandleValue vp) {
        return static_cast<Rooted<JSONParser<CharT>>*>(this)->get().parse(vp);
    }
};

} /* namespace js */

#endif /* vm_JSONParser_h */
