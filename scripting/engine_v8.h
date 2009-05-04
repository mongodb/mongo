#pragma once

#include "engine.h"

namespace mongo {
    
    class V8Scope : public Scope {
    public:
        V8Scope() {}
        virtual ~V8Scope() {}
        
        virtual void reset() {}
        virtual void init( BSONObj * data ) {}

        virtual void localConnect( const char * dbName ) {}
        
        virtual double getNumber( const char *field ) { assert( false ); return 0; }
        virtual string getString( const char *field ) { assert( false ); return ""; }
        virtual bool getBoolean( const char *field ) { assert( false ); return false; }
        virtual BSONObj getObject( const char *field ) { assert( false ); return BSONObj(); }
        
        virtual int type( const char *field ) { assert( false ); return 0; }
        
        virtual void setNumber( const char *field , double val ) {}
        virtual void setString( const char *field , const char * val ) {}
        virtual void setObject( const char *field , const BSONObj& obj ) {}
        virtual void setBoolean( const char *field , bool val ) {}
        virtual void setThis( const BSONObj * obj ) {}
        
        virtual ScriptingFunction createFunction( const char * code ) { assert( false ); return 0; }
        virtual int invoke( ScriptingFunction func , const BSONObj& args ) { assert( false ); return 0; }
        virtual string getError() { assert( false ); return ""; }
        
    };
    
    class V8ScriptEngine : public ScriptEngine {
    public:
        V8ScriptEngine() {}
        virtual ~V8ScriptEngine() {}
        
        virtual Scope * createScope() { return new V8Scope(); }
        
        virtual void runTest() {}
    };
    
    
    extern ScriptEngine * globalScriptEngine;
}
