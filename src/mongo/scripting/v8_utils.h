// v8_utils.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <v8.h>

#include <mongo/base/string_data.h>
#include <mongo/util/assert_util.h>

namespace mongo {

#define jsassert(x,msg) uassert(16664, (msg), (x))

#define argumentCheck(mustBeTrue, errorMessage)         \
    if (!(mustBeTrue)) {                                \
        return v8AssertionException((errorMessage));    \
    }

    std::ostream& operator<<(std::ostream& s, const v8::Handle<v8::Value>& o);
    std::ostream& operator<<(std::ostream& s, const v8::Handle<v8::TryCatch>* try_catch);

    /** Simple v8 object to string conversion helper */
    std::string toSTLString(const v8::Handle<v8::Value>& o);

    /** Like toSTLString but doesn't allocate a new std::string
     *
     *  This owns the string's memory so you need to be careful not to let the
     *  converted StringDatas outlive the V8Scope object. These rules are the
     *  same as converting from a std::string into a StringData.
     *
     *  Safe:
     *      void someFunction(StringData argument);
     *      v8::Handle<v8::String> aString;
     *
     *      someFunction(V8String(aString)); // passing down stack as temporary
     *
     *      V8String named (aString);
     *      someFunction(named); // passing up stack as named value
     *
     *      StringData sd = named; // scope of sd is less than named
     *
     *  Unsafe:
     *      StringData _member;
     *
     *      StringData returningFunction() {
     *          StringData sd = V8String(aString); // sd outlives the temporary
     *
     *          V8String named(aString)
     *          _member = named; // _member outlives named scope
     *
     *          return V8String(aString); // passing up stack
     *      }
     */
    class V8String {
    public:
        explicit V8String(const v8::Handle<v8::Value>& o) :_str(o) {
            massert(16686, "error converting js type to Utf8Value", *_str);
        }
        operator StringData () const { return StringData(*_str, _str.length()); }
    private:
        v8::String::Utf8Value _str;
    };

    /** Get the properties of an object (and it's prototype) as a comma-delimited string */
    std::string v8ObjectToString(const v8::Handle<v8::Object>& o);

    class V8Scope;
    void installFork(V8Scope* scope,
                     v8::Handle<v8::Object>& global,
                     v8::Handle<v8::Context>& context);

    /** Throw a V8 exception from Mongo callback code; message text will be preceded by "Error: ".
     *   Note: this function should be used for text that did not originate from the JavaScript
     *         engine.  Errors from the JavaScript engine will already have a prefix such as
     *         ReferenceError, TypeError or SyntaxError.
     *   Note: call only from a native function called from JavaScript (a callback).
     *         The V8 ThrowException routine will note a JavaScript exception that will be
     *         "thrown" in JavaScript when we return from the native function.
     *   Note: it's required to return immediately to V8's execution control without calling any
     *         V8 API functions.  In this state, an empty handle may (will) be returned.
     *  @param   errorMessage Error message text.
     *  @return  Empty handle to be returned from callback function.
     */
    v8::Handle<v8::Value> v8AssertionException(const char* errorMessage);
    v8::Handle<v8::Value> v8AssertionException(const std::string& errorMessage);
}

