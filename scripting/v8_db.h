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

#include "../client/dbclient.h"

namespace mongo {

    // These functions may depend on the caller creating a handle scope and context scope.
    
    v8::Handle<v8::FunctionTemplate> getMongoFunctionTemplate( bool local );
    void installDBTypes( v8::Handle<v8::ObjectTemplate>& global );
    void installDBTypes( v8::Handle<v8::Object>& global );
    
    // the actual globals
    
    mongo::DBClientBase * getConnection( const v8::Arguments& args );

    // Mongo members
    v8::Handle<v8::Value> mongoConsLocal(const v8::Arguments& args);
    v8::Handle<v8::Value> mongoConsExternal(const v8::Arguments& args);
    
    v8::Handle<v8::Value> mongoFind(const v8::Arguments& args);
    v8::Handle<v8::Value> mongoInsert(const v8::Arguments& args);
    v8::Handle<v8::Value> mongoRemove(const v8::Arguments& args);
    v8::Handle<v8::Value> mongoUpdate(const v8::Arguments& args);
    
    
    v8::Handle<v8::Value> internalCursorCons(const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorNext(const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorHasNext(const v8::Arguments& args);
    v8::Handle<v8::Value> internalCursorObjsLeftInBatch(const v8::Arguments& args);
    
    // DB members
    
    v8::Handle<v8::Value> dbInit(const v8::Arguments& args);
    v8::Handle<v8::Value> collectionInit( const v8::Arguments& args );
    v8::Handle<v8::Value> objectIdInit( const v8::Arguments& args );

    v8::Handle<v8::Value> dbRefInit( const v8::Arguments& args );
    v8::Handle<v8::Value> dbPointerInit( const v8::Arguments& args );

    v8::Handle<v8::Value> binDataInit( const v8::Arguments& args );
    v8::Handle<v8::Value> binDataToString( const v8::Arguments& args );

    v8::Handle<v8::Value> numberLongInit( const v8::Arguments& args );
    v8::Handle<v8::Value> numberLongToNumber(const v8::Arguments& args);
    v8::Handle<v8::Value> numberLongValueOf(const v8::Arguments& args);
    v8::Handle<v8::Value> numberLongToString(const v8::Arguments& args);
    
    v8::Handle<v8::Value> dbQueryInit( const v8::Arguments& args );
    v8::Handle<v8::Value> dbQueryIndexAccess( uint32_t index , const v8::AccessorInfo& info );
    
    v8::Handle<v8::Value> collectionFallback( v8::Local<v8::String> name, const v8::AccessorInfo &info);

    v8::Handle<v8::Value> bsonsize( const v8::Arguments& args );

}

template < v8::Handle< v8::Value > ( *f ) ( const v8::Arguments& ) >
v8::Handle< v8::Value > v8Callback( const v8::Arguments &args ) {
    try {
        return f( args );
    } catch( const std::exception &e ) {
        return v8::ThrowException( v8::String::New( e.what() ) );
    } catch( ... ) {
        return v8::ThrowException( v8::String::New( "unknown exception" ) );
    }
}

template < v8::Handle< v8::Value > ( *f ) ( const v8::Arguments& ) >
v8::Local< v8::FunctionTemplate > newV8Function() {
    return v8::FunctionTemplate::New( v8Callback< f > );
}
