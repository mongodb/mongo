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

#include <v8.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>

namespace mongo {

    v8::Handle<v8::Value> Print(const v8::Arguments& args);
    v8::Handle<v8::Value> Version(const v8::Arguments& args);
    v8::Handle<v8::Value> GCV8(const v8::Arguments& args);

    void ReportException(v8::TryCatch* handler);

#define jsassert(x,msg) assert(x)

    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::Value> & o );
    std::ostream& operator<<( std::ostream &s, const v8::Handle<v8::TryCatch> * try_catch );

    std::string toSTLString( const v8::Handle<v8::Value> & o );
    std::string toSTLString( const v8::TryCatch * try_catch );

    class V8Scope;
    void installFork( v8::Handle< v8::Object > &global, v8::Handle< v8::Context > &context );
}

