// v8_wrapper.h

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
#include "../db/jsobj.h"

namespace mongo {

    v8::Local<v8::Object> mongoToV8( const mongo::BSONObj & m , bool array = 0 , bool readOnly = false );
    mongo::BSONObj v8ToMongo( v8::Handle<v8::Object> o , int depth = 0 );

    void v8ToMongoElement( BSONObjBuilder & b , v8::Handle<v8::String> name ,
                           const string sname , v8::Handle<v8::Value> value , int depth = 0 );
    v8::Handle<v8::Value> mongoToV8Element( const BSONElement &f );

    v8::Function * getNamedCons( const char * name );
    v8::Function * getObjectIdCons();

    v8::Handle<v8::FunctionTemplate> getObjectWrapperTemplate();

    class WrapperHolder;
    WrapperHolder * createWrapperHolder( const BSONObj * o , bool readOnly , bool iDelete );

}
