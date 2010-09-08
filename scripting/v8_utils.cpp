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

#include "v8_utils.h"
#include "v8_db.h"
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>
#include "engine_v8.h"

using namespace std;
using namespace v8;

namespace mongo {

    Handle<v8::Value> Print(const Arguments& args) {
        bool first = true;
        for (int i = 0; i < args.Length(); i++) {
            HandleScope handle_scope;
            if (first) {
                first = false;
            } else {
                printf(" ");
            }
            v8::String::Utf8Value str(args[i]);
            printf("%s", *str);
        }
        printf("\n");
        return v8::Undefined();
    }

    std::string toSTLString( const Handle<v8::Value> & o ){
        v8::String::Utf8Value str(o);    
        const char * foo = *str;
        std::string s(foo);
        return s;
    }

    std::string toSTLString( const v8::TryCatch * try_catch ){
        
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


    std::ostream& operator<<( std::ostream &s, const Handle<v8::Value> & o ){
        v8::String::Utf8Value str(o);    
        s << *str;
        return s;
    }

    std::ostream& operator<<( std::ostream &s, const v8::TryCatch * try_catch ){
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


    Handle<v8::Value> Version(const Arguments& args) {
        HandleScope handle_scope;
        return handle_scope.Close( v8::String::New(v8::V8::GetVersion()) );
    }

    void ReportException(v8::TryCatch* try_catch) {
        cout << try_catch << endl;
    }
    
    Handle< Context > baseContext_;
    
    class JSThreadConfig {
    public:
        JSThreadConfig( const Arguments &args, bool newScope = false ) : started_(), done_(), newScope_( newScope ) {
            jsassert( args.Length() > 0, "need at least one argument" );
            jsassert( args[ 0 ]->IsFunction(), "first argument must be a function" );
            Local< v8::Function > f = v8::Function::Cast( *args[ 0 ] );
            f_ = Persistent< v8::Function >::New( f );
            for( int i = 1; i < args.Length(); ++i )
                args_.push_back( Persistent< Value >::New( args[ i ] ) );
        }
        ~JSThreadConfig() {
            f_.Dispose();
            for( vector< Persistent< Value > >::iterator i = args_.begin(); i != args_.end(); ++i )
                i->Dispose();
            returnData_.Dispose();
        }
        void start() {
            jsassert( !started_, "Thread already started" );
            JSThread jt( *this );
            thread_.reset( new boost::thread( jt ) );
            started_ = true;
        }
        void join() {
            jsassert( started_ && !done_, "Thread not running" );
            Unlocker u;
            thread_->join();
            done_ = true;
        }
        Local< Value > returnData() {
            if ( !done_ )
                join();
            return Local< Value >::New( returnData_ );
        }
    private:
        class JSThread {
        public:
            JSThread( JSThreadConfig &config ) : config_( config ) {}
            void operator()() {
                Locker l;
                HandleScope handle_scope;
                Handle< Context > context;
                Handle< v8::Function > fun;
                auto_ptr< V8Scope > scope;
                if ( config_.newScope_ ) {
                    scope.reset( dynamic_cast< V8Scope * >( globalScriptEngine->newScope() ) );
                    context = scope->context();
                    // A v8::Function tracks the context in which it was created, so we have to
                    // create a new function in the new context.
                    Context::Scope baseScope( baseContext_ );
                    string fCode = toSTLString( config_.f_->ToString() );
                    Context::Scope context_scope( context );
                    fun = scope->__createFunction( fCode.c_str() );
                } else {
                    context = baseContext_;
                    Context::Scope context_scope( context );
                    fun = config_.f_;
                }
                Context::Scope context_scope( context );
                boost::scoped_array< Local< Value > > argv( new Local< Value >[ config_.args_.size() ] );
                for( unsigned int i = 0; i < config_.args_.size(); ++i )
                    argv[ i ] = Local< Value >::New( config_.args_[ i ] );
                TryCatch try_catch;
                Handle< Value > ret = fun->Call( context->Global(), config_.args_.size(), argv.get() );
                if ( ret.IsEmpty() ) {
                    string e = toSTLString( &try_catch );
                    log() << "js thread raised exception: " << e << endl;
                    // v8 probably does something sane if ret is empty, but not going to assume that for now
                    ret = v8::Undefined();
                }
                config_.returnData_ = Persistent< Value >::New( ret );
            }
        private:
            JSThreadConfig &config_;
        };
        
        bool started_;
        bool done_;
        bool newScope_;
        Persistent< v8::Function > f_;
        vector< Persistent< Value > > args_;
        auto_ptr< boost::thread > thread_;
        Persistent< Value > returnData_;
    };
    
    Handle< Value > ThreadInit( const Arguments &args ) {
        Handle<v8::Object> it = args.This();
        // NOTE I believe the passed JSThreadConfig will never be freed.  If this
        // policy is changed, JSThread may no longer be able to store JSThreadConfig
        // by reference.
        it->SetHiddenValue( v8::String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( args ) ) );
        return v8::Undefined();
    }
    
    Handle< Value > ScopedThreadInit( const Arguments &args ) {
        Handle<v8::Object> it = args.This();
        // NOTE I believe the passed JSThreadConfig will never be freed.  If this
        // policy is changed, JSThread may no longer be able to store JSThreadConfig
        // by reference.
        it->SetHiddenValue( v8::String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( args, true ) ) );
        return v8::Undefined();
    }

    JSThreadConfig *thisConfig( const Arguments &args ) {
        Local< External > c = External::Cast( *(args.This()->GetHiddenValue( v8::String::New( "_JSThreadConfig" ) ) ) );
        JSThreadConfig *config = (JSThreadConfig *)( c->Value() );
        return config;
    }
    
    Handle< Value > ThreadStart( const Arguments &args ) {
        thisConfig( args )->start();
        return v8::Undefined();
    }
    
    Handle< Value > ThreadJoin( const Arguments &args ) {
        thisConfig( args )->join();
        return v8::Undefined();
    }
    
    Handle< Value > ThreadReturnData( const Arguments &args ) {
        HandleScope handle_scope;
        return handle_scope.Close( thisConfig( args )->returnData() );
    }

    Handle< Value > ThreadInject( const Arguments &args ) {
        jsassert( args.Length() == 1 , "threadInject takes exactly 1 argument" );
        jsassert( args[0]->IsObject() , "threadInject needs to be passed a prototype" );
        
        Local<v8::Object> o = args[0]->ToObject();
        
        o->Set( v8::String::New( "init" ) , newV8Function< ThreadInit >()->GetFunction() );
        o->Set( v8::String::New( "start" ) , newV8Function< ThreadStart >()->GetFunction() );
        o->Set( v8::String::New( "join" ) , newV8Function< ThreadJoin >()->GetFunction() );
        o->Set( v8::String::New( "returnData" ) , newV8Function< ThreadReturnData >()->GetFunction() );
        
        return v8::Undefined();    
    }

    Handle< Value > ScopedThreadInject( const Arguments &args ) {
        jsassert( args.Length() == 1 , "threadInject takes exactly 1 argument" );
        jsassert( args[0]->IsObject() , "threadInject needs to be passed a prototype" );
        
        Local<v8::Object> o = args[0]->ToObject();
        
        o->Set( v8::String::New( "init" ) , newV8Function< ScopedThreadInit >()->GetFunction() );
        // inheritance takes care of other member functions
        
        return v8::Undefined();
    }
    
    void installFork( v8::Handle< v8::Object > &global, v8::Handle< v8::Context > &context ) {
        if ( baseContext_.IsEmpty() ) // if this is the shell, first call will be with shell context, otherwise don't expect to use fork() anyway
            baseContext_ = context;
        global->Set( v8::String::New( "_threadInject" ), newV8Function< ThreadInject >()->GetFunction() );
        global->Set( v8::String::New( "_scopedThreadInject" ), newV8Function< ScopedThreadInject >()->GetFunction() );
    }

    Handle<v8::Value> GCV8(const Arguments& args) {
        Locker l;
        while( V8::IdleNotification() );
        return v8::Undefined();
    }


}
