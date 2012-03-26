// v8_wrapper.cpp

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

#include "v8_wrapper.h"
#include "v8_utils.h"
#include "v8_db.h"
#include "engine_v8.h"

#include <iostream>

using namespace std;
using namespace v8;

namespace mongo {

#define DDD(x)

    // --- object wrapper ---

    class WrapperHolder {
    public:
        WrapperHolder( V8Scope* scope, const BSONObj * o , bool readOnly , bool iDelete )
            : _scope(scope), _o(o), _readOnly( readOnly ), _iDelete( iDelete ) {
        }

        ~WrapperHolder() {
            if ( _o && _iDelete ) {
                delete _o;
            }
            _o = 0;
        }

        v8::Handle<v8::Value> get( v8::Local<v8::String> name ) {
            const string& s = toSTLString( name );
            const BSONElement& e = _o->getField( s );
            return _scope->mongoToV8Element(e);
        }

        V8Scope* _scope;
        const BSONObj * _o;
        bool _readOnly;
        bool _iDelete;
    };

    WrapperHolder * createWrapperHolder( V8Scope* scope, const BSONObj * o , bool readOnly , bool iDelete ) {
        return new WrapperHolder( scope, o , readOnly , iDelete );
    }

    WrapperHolder * getWrapper( v8::Handle<v8::Object> o ) {
        Handle<v8::Value> t = o->GetRealNamedProperty( v8::String::New( "_wrapper" ) );
        verify( t->IsExternal() );
        Local<External> c = External::Cast( *t );
        WrapperHolder * w = (WrapperHolder*)(c->Value());
        verify( w );
        return w;
    }


    Handle<Value> wrapperCons(V8Scope* scope, const Arguments& args) {
        if ( ! ( args.Length() == 1 && args[0]->IsExternal() ) )
            return v8::ThrowException( v8::String::New( "wrapperCons needs 1 External arg" ) );

        args.This()->Set( v8::String::New( "_wrapper" ) , args[0] );

        return v8::Undefined();
    }

    v8::Handle<v8::Value> wrapperGetHandler( v8::Local<v8::String> name, const v8::AccessorInfo &info) {
        return getWrapper( info.This() )->get( name );
    }

    v8::Handle<v8::FunctionTemplate> getObjectWrapperTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> t = scope->createV8Function(wrapperCons);
        t->InstanceTemplate()->SetNamedPropertyHandler( wrapperGetHandler );
        return t;
    }
}
