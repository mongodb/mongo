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

