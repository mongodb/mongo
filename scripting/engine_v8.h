//engine_v8.h

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

#include <vector>
#include "engine.h"
#include <v8.h>

using namespace v8;

namespace mongo {

    class V8ScriptEngine;
    
    class V8Scope : public Scope {
    public:
        
        V8Scope( V8ScriptEngine * engine );
        ~V8Scope();
        
        virtual void reset();
        virtual void init( BSONObj * data );

        virtual void localConnect( const char * dbName );
        virtual void externalSetup();
        
        v8::Handle<v8::Value> get( const char * field ); // caller must create context and handle scopes
        virtual double getNumber( const char *field );
        virtual int getNumberInt( const char *field );
        virtual long long getNumberLongLong( const char *field );
        virtual string getString( const char *field );
        virtual bool getBoolean( const char *field );
        virtual BSONObj getObject( const char *field );
        
        virtual int type( const char *field );

        virtual void setNumber( const char *field , double val );
        virtual void setString( const char *field , const char * val );
        virtual void setBoolean( const char *field , bool val );
        virtual void setElement( const char *field , const BSONElement& e );
        virtual void setObject( const char *field , const BSONObj& obj , bool readOnly);
        virtual void setThis( const BSONObj * obj );

        virtual void rename( const char * from , const char * to );
        
        virtual ScriptingFunction _createFunction( const char * code );
        Local< v8::Function > __createFunction( const char * code );
        virtual int invoke( ScriptingFunction func , const BSONObj& args, int timeoutMs = 0 , bool ignoreReturn = false );
        virtual bool exec( const StringData& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs );
        virtual string getError(){ return _error; }
        
        virtual void injectNative( const char *field, NativeFunction func );

        void gc();

        Handle< Context > context() const { return _context; }
        
    private:
        void _startCall();
        
        static Handle< Value > nativeCallback( const Arguments &args );

        static Handle< Value > loadCallback( const Arguments &args );

        V8ScriptEngine * _engine;

        Persistent<Context> _context;
        Persistent<v8::Object> _global;

        string _error;
        vector< Persistent<Value> > _funcs;
        v8::Persistent<v8::Object> _this;

        v8::Persistent<v8::Function> _wrapper;

        enum ConnectState { NOT , LOCAL , EXTERNAL };
        ConnectState _connectState;
    };
    
    class V8ScriptEngine : public ScriptEngine {
    public:
        V8ScriptEngine();
        virtual ~V8ScriptEngine();
        
        virtual Scope * createScope(){ return new V8Scope( this ); }
        
        virtual void runTest(){}

        bool utf8Ok() const { return true; }

        class V8Unlocker : public Unlocker {
            v8::Unlocker u_;
        };
        
        virtual auto_ptr<Unlocker> newThreadUnlocker() { return auto_ptr< Unlocker >( new V8Unlocker ); }
        
    private:
        friend class V8Scope;
    };
    
    
    extern ScriptEngine * globalScriptEngine;
}
