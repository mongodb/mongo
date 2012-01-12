// v8_utils.cpp

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

#if defined(_WIN32)
/** this is a hack - v8stdint.h defined uint16_t etc. on _WIN32 only, and that collides with 
    our usage of boost */
#include "boost/cstdint.hpp"
using namespace boost;
#define V8STDINT_H_
#endif

#include "v8_utils.h"
#include "v8_db.h"
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>
#include "engine_v8.h"

using namespace std;
using namespace v8;

namespace mongo {

    std::string toSTLString( const Handle<v8::Value> & o ) {
        v8::String::Utf8Value str(o);
        const char * foo = *str;
        std::string s(foo);
        return s;
    }

    std::string toSTLString( const v8::TryCatch * try_catch ) {

        stringstream ss;

        //while ( try_catch ){ // disabled for v8 bleeding edge

        v8::String::Utf8Value exception(try_catch->Exception());
        Handle<v8::Message> message = try_catch->Message();

        if (message.IsEmpty()) {
            ss << *exception << endl;
        }
        else {

            v8::String::Utf8Value filename(message->GetScriptResourceName());
            int linenum = message->GetLineNumber();
            ss << *filename << ":" << linenum << " " << *exception << endl;

            v8::String::Utf8Value sourceline(message->GetSourceLine());
            ss << *sourceline << endl;

            int start = message->GetStartColumn();
            for (int i = 0; i < start; i++)
                ss << " ";

            int end = message->GetEndColumn();
            for (int i = start; i < end; i++)
                ss << "^";

            ss << endl;
        }

        //try_catch = try_catch->next_;
        //}

        return ss.str();
    }


    std::ostream& operator<<( std::ostream &s, const Handle<v8::Value> & o ) {
        v8::String::Utf8Value str(o);
        s << *str;
        return s;
    }

    std::ostream& operator<<( std::ostream &s, const v8::TryCatch * try_catch ) {
        HandleScope handle_scope;
        v8::String::Utf8Value exception(try_catch->Exception());
        Handle<v8::Message> message = try_catch->Message();

        if (message.IsEmpty()) {
            s << *exception << endl;
        }
        else {

            v8::String::Utf8Value filename(message->GetScriptResourceName());
            int linenum = message->GetLineNumber();
            cout << *filename << ":" << linenum << " " << *exception << endl;

            v8::String::Utf8Value sourceline(message->GetSourceLine());
            cout << *sourceline << endl;

            int start = message->GetStartColumn();
            for (int i = 0; i < start; i++)
                cout << " ";

            int end = message->GetEndColumn();
            for (int i = start; i < end; i++)
                cout << "^";

            cout << endl;
        }

        //if ( try_catch->next_ ) // disabled for v8 bleeding edge
        //    s << try_catch->next_;

        return s;
    }

    void ReportException(v8::TryCatch* try_catch) {
        cout << try_catch << endl;
    }

    Handle< Context > baseContext_;

    class JSThreadConfig {
    public:
        JSThreadConfig( V8Scope* scope, const Arguments &args, bool newScope = false ) : started_(), done_(), newScope_( newScope ) {
            jsassert( args.Length() > 0, "need at least one argument" );
            jsassert( args[ 0 ]->IsFunction(), "first argument must be a function" );

            // arguments need to be copied into the isolate, go through bson
            BSONObjBuilder b;
            for( int i = 0; i < args.Length(); ++i ) {
                scope->v8ToMongoElement(b, "arg" + i, args[i]);
            }
            args_ = b.obj();
        }

        ~JSThreadConfig() {
        }

        void start() {
            jsassert( !started_, "Thread already started" );
            // obtain own scope for execution
            // do it here, not in constructor, otherwise it creates an infinite recursion from ScopedThread
            _scope.reset( dynamic_cast< V8Scope * >( globalScriptEngine->newScope() ) );

            JSThread jt( *this );
            thread_.reset( new boost::thread( jt ) );
            started_ = true;
        }
        void join() {
            jsassert( started_ && !done_, "Thread not running" );
            thread_->join();
            done_ = true;
        }

        BSONObj returnData() {
            if ( !done_ )
                join();
            return returnData_;
        }

    private:
        class JSThread {
        public:
            JSThread( JSThreadConfig &config ) : config_( config ) {}

            void operator()() {
                V8Scope* scope = config_._scope.get();
                v8::Isolate::Scope iscope(scope->getIsolate());
                v8::Locker l(scope->getIsolate());
                HandleScope handle_scope;
                Context::Scope context_scope( scope->getContext() );

                BSONObj args = config_.args_;
                Local< v8::Function > f = v8::Function::Cast( *(scope->mongoToV8Element(args.firstElement(), true)) );
                int argc = args.nFields() - 1;

                boost::scoped_array< Local< Value > > argv( new Local< Value >[ argc ] );
                BSONObjIterator it(args);
                it.next();
                for( int i = 0; i < argc; ++i ) {
                    argv[ i ] = Local< Value >::New( scope->mongoToV8Element(*it, true) );
                    it.next();
                }
                TryCatch try_catch;
                Handle< Value > ret = f->Call( scope->getContext()->Global(), argc, argv.get() );
                if ( ret.IsEmpty() ) {
                    string e = toSTLString( &try_catch );
                    log() << "js thread raised exception: " << e << endl;
                    // v8 probably does something sane if ret is empty, but not going to assume that for now
                    ret = v8::Undefined();
                }
                // ret is translated to BSON to switch isolate
                BSONObjBuilder b;
                scope->v8ToMongoElement(b, "ret", ret);
                config_.returnData_ = b.obj();
            }

        private:
            JSThreadConfig &config_;
        };

        bool started_;
        bool done_;
        bool newScope_;
        BSONObj args_;
        scoped_ptr< boost::thread > thread_;
        scoped_ptr< V8Scope > _scope;
        BSONObj returnData_;
    };

    Handle< Value > ThreadInit( V8Scope* scope, const Arguments &args ) {
        Handle<v8::Object> it = args.This();
        // NOTE I believe the passed JSThreadConfig will never be freed.  If this
        // policy is changed, JSThread may no longer be able to store JSThreadConfig
        // by reference.
        it->SetHiddenValue( v8::String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( scope, args ) ) );
        return v8::Undefined();
    }

    Handle< Value > ScopedThreadInit( V8Scope* scope, const Arguments &args ) {
        Handle<v8::Object> it = args.This();
        // NOTE I believe the passed JSThreadConfig will never be freed.  If this
        // policy is changed, JSThread may no longer be able to store JSThreadConfig
        // by reference.
        it->SetHiddenValue( v8::String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( scope, args, true ) ) );
        return v8::Undefined();
    }

    JSThreadConfig *thisConfig( V8Scope* scope, const Arguments &args ) {
        Local< External > c = External::Cast( *(args.This()->GetHiddenValue( v8::String::New( "_JSThreadConfig" ) ) ) );
        JSThreadConfig *config = (JSThreadConfig *)( c->Value() );
        return config;
    }

    Handle< Value > ThreadStart( V8Scope* scope, const Arguments &args ) {
        thisConfig( scope, args )->start();
        return v8::Undefined();
    }

    Handle< Value > ThreadJoin( V8Scope* scope, const Arguments &args ) {
        thisConfig( scope, args )->join();
        return v8::Undefined();
    }

    Handle< Value > ThreadReturnData( V8Scope* scope, const Arguments &args ) {
        BSONObj data = thisConfig( scope, args )->returnData();
        return scope->mongoToV8Element(data.firstElement(), true);
    }

    Handle< Value > ThreadInject( V8Scope* scope, const Arguments &args ) {
        jsassert( args.Length() == 1 , "threadInject takes exactly 1 argument" );
        jsassert( args[0]->IsObject() , "threadInject needs to be passed a prototype" );

        Local<v8::Object> o = args[0]->ToObject();

        // install method on the Thread object
        scope->injectV8Function("init", ThreadInit, o);
        scope->injectV8Function("start", ThreadStart, o);
        scope->injectV8Function("join", ThreadJoin, o);
        scope->injectV8Function("returnData", ThreadReturnData, o);
        return v8::Undefined();
    }

    Handle< Value > ScopedThreadInject( V8Scope* scope, const Arguments &args ) {
        jsassert( args.Length() == 1 , "threadInject takes exactly 1 argument" );
        jsassert( args[0]->IsObject() , "threadInject needs to be passed a prototype" );

        Local<v8::Object> o = args[0]->ToObject();

        scope->injectV8Function("init", ScopedThreadInit, o);
        // inheritance takes care of other member functions

        return v8::Undefined();
    }

    void installFork( V8Scope* scope, v8::Handle< v8::Object > &global, v8::Handle< v8::Context > &context ) {
        if ( baseContext_.IsEmpty() ) // if this is the shell, first call will be with shell context, otherwise don't expect to use fork() anyway
            baseContext_ = context;
        scope->injectV8Function("_threadInject", ThreadInject, global);
        scope->injectV8Function("_scopedThreadInject", ScopedThreadInject, global);
    }

}
