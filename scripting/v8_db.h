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

#include "engine.h"
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
    v8::Handle<v8::Value> dbTimestampInit( const v8::Arguments& args );

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

    // call with v8 mutex:
    void enableV8Interrupt();
    void disableV8Interrupt();

    // The implementation below assumes that SERVER-1816 has been fixed - in
    // particular, interrupted() must return true if an interrupt was ever
    // sent; currently that is not the case if a new killop overwrites the data
    // for an old one
    template < v8::Handle< v8::Value > ( *f ) ( const v8::Arguments& ) >
    v8::Handle< v8::Value > v8Callback( const v8::Arguments &args ) {
        disableV8Interrupt(); // we don't want to have to audit all v8 calls for termination exceptions, so we don't allow these exceptions during the callback
        if ( globalScriptEngine->interrupted() ) {
            v8::V8::TerminateExecution(); // experimentally it seems that TerminateExecution() will override the return value
            return v8::Undefined();
        }
        v8::Handle< v8::Value > ret;
        string exception;
        try {
            ret = f( args );
        }
        catch( const std::exception &e ) {
            exception = e.what();
        }
        catch( ... ) {
            exception = "unknown exception";
        }
        enableV8Interrupt();
        if ( globalScriptEngine->interrupted() ) {
            v8::V8::TerminateExecution();
            return v8::Undefined();
        }
        if ( !exception.empty() ) {
            // technically, ThrowException is supposed to be the last v8 call before returning
            ret = v8::ThrowException( v8::String::New( exception.c_str() ) );
        }
        return ret;
    }

    template < v8::Handle< v8::Value > ( *f ) ( const v8::Arguments& ) >
    v8::Local< v8::FunctionTemplate > newV8Function() {
        return v8::FunctionTemplate::New( v8Callback< f > );
    }

    // Preemption is going to be allowed for the v8 mutex, and some of our v8
    // usage is not preemption safe.  So we are using an additional mutex that
    // will not be preempted.  The V8Lock should be used in place of v8::Locker
    // except in certain special cases involving interrupts.
    namespace v8Locks {
        // the implementations are quite simple - objects must be destroyed in
        // reverse of the order created, and should not be shared between threads
        struct RecursiveLock {
            RecursiveLock();
            ~RecursiveLock();
            bool _unlock;
        };
        struct RecursiveUnlock {
            RecursiveUnlock();
            ~RecursiveUnlock();
            bool _lock;
        };
    } // namespace v8Locks
    class V8Lock {
        v8Locks::RecursiveLock _noPreemptionLock;
        v8::Locker _preemptionLock;
    };
    struct V8Unlock {
        v8::Unlocker _preemptionUnlock;
        v8Locks::RecursiveUnlock _noPreemptionUnlock;
    };
}

