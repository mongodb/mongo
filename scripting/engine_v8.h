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
        
        virtual void reset(){}
        virtual void init( BSONObj * data ){ assert(0); }

        virtual void localConnect( const char * dbName ){ assert(0); }
        virtual void externalSetup(){ assert(0); };
        
        virtual double getNumber( const char *field );
        virtual string getString( const char *field );
        virtual bool getBoolean( const char *field );
        virtual BSONObj getObject( const char *field ){ assert( false ); return BSONObj(); }
        
        virtual int type( const char *field ){ assert( false ); return 0; }

        virtual void setNumber( const char *field , double val );
        virtual void setString( const char *field , const char * val );
        virtual void setBoolean( const char *field , bool val );
        virtual void setElement( const char *field , const BSONElement& e ){ assert( 0 );} 
        virtual void setObject( const char *field , const BSONObj& obj , bool readOnly){ assert(0); }
        virtual void setThis( const BSONObj * obj ){ assert(0); }
        
        virtual ScriptingFunction _createFunction( const char * code );
        virtual int invoke( ScriptingFunction func , const BSONObj& args, int timeoutMs = 0 , bool ignoreReturn = false );
        virtual bool exec( const string& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs );
        virtual string getError(){ return _error; }
        
        virtual void injectNative( const char *field, NativeFunction func ){
            Handle< FunctionTemplate > f( v8::FunctionTemplate::New( nativeCallback ) );
            f->Set( v8::String::New( "_native_function" ), External::New( (void*)func ) );
            _global->Set( v8::String::New( field ), f->GetFunction() );
        }

        void gc(){ assert(0); }

    private:
        void _startCall();
        
        static Handle< Value > nativeCallback( const Arguments &args );

        HandleScope _handleScope;
        Handle<Context> _context;
        Context::Scope _scope;
        Handle<v8::Object> _global;

        string _error;
        vector< v8::Handle<Value> > _funcs;
        v8::Handle<v8::Object> _this;
    };
    
    class V8ScriptEngine : public ScriptEngine {
    public:
        V8ScriptEngine();
        virtual ~V8ScriptEngine();
        
        virtual Scope * createScope(){ return new V8Scope( this ); }
        
        virtual void runTest(){}

        bool utf8Ok() const { return true; }

    private:
        HandleScope _handleScope;
        Handle<ObjectTemplate> _globalTemplate;

        friend class V8Scope;
    };
    
    
    extern ScriptEngine * globalScriptEngine;
}
