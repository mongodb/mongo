// v8_db.h

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

#include "engine_v8.h"

namespace mongo {
    class DBClientBase;
    // These functions may depend on the caller creating a handle scope and context scope.

    v8::Handle<v8::FunctionTemplate> getMongoFunctionTemplate( V8Scope * scope, bool local );
//    void installDBTypes( V8Scope * scope, v8::Handle<v8::ObjectTemplate>& global );
    void installDBTypes( V8Scope * scope, v8::Handle<v8::Object>& global );

    // the actual globals

    mongo::DBClientBase * getConnection( const v8::Arguments& args );

    // Mongo members
    v8::Handle<v8::Value> mongoConsLocal(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> mongoConsExternal(V8Scope* scope, const v8::Arguments& args);

    v8::Handle<v8::Value> mongoFind(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> mongoInsert(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> mongoRemove(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> mongoUpdate(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> mongoAuth(V8Scope* scope, const v8::Arguments& args);

    v8::Handle<v8::Value> internalCursorCons(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorNext(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorHasNext(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorObjsLeftInBatch(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorReadOnly(V8Scope* scope, const v8::Arguments& args);

    // DB members

    v8::Handle<v8::Value> dbInit(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> collectionInit(V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> objectIdInit( V8Scope* scope, const v8::Arguments& args );

    v8::Handle<v8::Value> dbRefInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> dbPointerInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> dbTimestampInit( V8Scope* scope, const v8::Arguments& args );

    v8::Handle<v8::Value> binDataInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> binDataToString( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> binDataToBase64( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> binDataToHex( V8Scope* scope, const v8::Arguments& args );

    v8::Handle<v8::Value> uuidInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> md5Init( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> hexDataInit( V8Scope* scope, const v8::Arguments& args );

    v8::Handle<v8::Value> numberLongInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> numberLongToNumber(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> numberLongValueOf(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> numberLongToString(V8Scope* scope, const v8::Arguments& args);

    v8::Handle<v8::Value> numberIntInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> numberIntToNumber(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> numberIntValueOf(V8Scope* scope, const v8::Arguments& args);
    v8::Handle<v8::Value> numberIntToString(V8Scope* scope, const v8::Arguments& args);

    v8::Handle<v8::Value> dbQueryInit( V8Scope* scope, const v8::Arguments& args );
    v8::Handle<v8::Value> dbQueryIndexAccess( ::uint32_t index , const v8::AccessorInfo& info );

    v8::Handle<v8::Value> collectionGetter( v8::Local<v8::String> name, const v8::AccessorInfo &info);
    v8::Handle<v8::Value> collectionSetter( Local<v8::String> name, Local<Value> value, const AccessorInfo& info );

    v8::Handle<v8::Value> bsonsize( V8Scope* scope, const v8::Arguments& args );

}

