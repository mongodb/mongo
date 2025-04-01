/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JSON serialization and deserialization operations.
 */

#ifndef js_JSON_h
#define js_JSON_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

using JSONWriteCallback = bool (*)(const char16_t* buf, uint32_t len,
                                   void* data);

/**
 * Performs the JSON.stringify operation, as specified by ECMAScript, except
 * writing stringified data by exactly one call of |callback|, passing |data| as
 * argument.
 *
 * In cases where JSON.stringify would return undefined, this function calls
 * |callback| with the string "null".
 */
extern JS_PUBLIC_API bool JS_Stringify(JSContext* cx,
                                       JS::MutableHandle<JS::Value> value,
                                       JS::Handle<JSObject*> replacer,
                                       JS::Handle<JS::Value> space,
                                       JSONWriteCallback callback, void* data);

namespace JS {

/**
 * An API akin to JS_Stringify but with the goal of not having observable
 * side-effects when the stringification is performed.  This means it does not
 * allow a replacer or a custom space and has the following constraints on its
 * input:
 *
 * 1) The input must be a plain object or array, not an abitrary value.
 * 2) Every value in the graph reached by the algorithm starting with this
 *    object must be one of the following: null, undefined, a string (NOT a
 *    string object!), a boolean, a finite number (i.e. no NaN or Infinity or
 *    -Infinity), a plain object with no accessor properties, or an Array with
 *    no holes.
 *
 * The actual behavior differs from JS_Stringify only in asserting the above and
 * NOT attempting to get the "toJSON" property from things, since that could
 * clearly have side-effects.
 */
extern JS_PUBLIC_API bool ToJSONMaybeSafely(JSContext* cx,
                                            JS::Handle<JSObject*> input,
                                            JSONWriteCallback callback,
                                            void* data);

/**
 * Performs the JSON.stringify operation, as specified by ECMAScript, except
 * writing stringified data by one call of |callback|, passing |data| as
 * argument.
 *
 * In cases where JSON.stringify would return undefined, this function does not
 * call |callback| at all.
 */
extern JS_PUBLIC_API bool ToJSON(JSContext* cx, Handle<Value> value,
                                 Handle<JSObject*> replacer,
                                 Handle<Value> space,
                                 JSONWriteCallback callback, void* data);

} /* namespace JS */

/**
 * Performs the JSON.parse operation as specified by ECMAScript.
 */
extern JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, const char16_t* chars,
                                       uint32_t len,
                                       JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript.
 */
extern JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, JS::Handle<JSString*> str,
                                       JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript.
 */
extern JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx,
                                       const JS::Latin1Char* chars,
                                       uint32_t len,
                                       JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript, using the
 * given |reviver| argument as the corresponding optional argument to that
 * function.
 */
extern JS_PUBLIC_API bool JS_ParseJSONWithReviver(
    JSContext* cx, const char16_t* chars, uint32_t len,
    JS::Handle<JS::Value> reviver, JS::MutableHandle<JS::Value> vp);

/**
 * Performs the JSON.parse operation as specified by ECMAScript, using the
 * given |reviver| argument as the corresponding optional argument to that
 * function.
 */
extern JS_PUBLIC_API bool JS_ParseJSONWithReviver(
    JSContext* cx, JS::Handle<JSString*> str, JS::Handle<JS::Value> reviver,
    JS::MutableHandle<JS::Value> vp);

namespace JS {

/**
 * Returns true if the given text is valid JSON.
 */
extern JS_PUBLIC_API bool IsValidJSON(const JS::Latin1Char* chars,
                                      uint32_t len);
extern JS_PUBLIC_API bool IsValidJSON(const char16_t* chars, uint32_t len);

/**
 * Handler with callbacks for JS::ParseJSONWithHandler.
 *
 * Each method is called during parsing the JSON string. If the method returns
 * true, the parsing keeps going.  If the method returns false, the parsing
 * stops and fails.
 *
 * The error method is called when syntax error happens while parsing the input.
 * This method is not called when handler's method returns false.
 */
class JSONParseHandler {
 public:
  JSONParseHandler() {}
  virtual ~JSONParseHandler() {}

  // Called when '{' is found for an object.
  virtual bool startObject() = 0;

  // Called when a property name is found for an object.
  // The character type depends on the input type and also the content of the
  // property name. The consumer should implement both methods.
  virtual bool propertyName(const JS::Latin1Char* name, size_t length) = 0;
  virtual bool propertyName(const char16_t* name, size_t length) = 0;

  // Called when '}' is found for an object.
  virtual bool endObject() = 0;

  // Called when '[' is found for an array.
  virtual bool startArray() = 0;

  // Called when ']' is found for an array.
  virtual bool endArray() = 0;

  // Called when a string is found.
  // The character type depends on the input type and also the content of the
  // string. The consumer should implement both methods.
  virtual bool stringValue(const JS::Latin1Char* str, size_t length) = 0;
  virtual bool stringValue(const char16_t* str, size_t length) = 0;

  // Called when a number is found.
  virtual bool numberValue(double d) = 0;

  // Called when a boolean is found.
  virtual bool booleanValue(bool v) = 0;

  // Called when null is found.
  virtual bool nullValue() = 0;

  // Called when syntax error happens.
  virtual void error(const char* msg, uint32_t line, uint32_t column) = 0;
};

/**
 * Performs the JSON.parse operation as specified by ECMAScript, and call
 * callbacks defined by the handler.
 */
extern JS_PUBLIC_API bool ParseJSONWithHandler(const JS::Latin1Char* chars,
                                               uint32_t len,
                                               JSONParseHandler* handler);
extern JS_PUBLIC_API bool ParseJSONWithHandler(const char16_t* chars,
                                               uint32_t len,
                                               JSONParseHandler* handler);

}  // namespace JS

#endif /* js_JSON_h */
