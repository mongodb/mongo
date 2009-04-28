// engine.h

#pragma once

#include "../stdafx.h"
#include "../db/jsobj.h"

namespace mongo {

    typedef unsigned long long ScriptingFunction;
    
    class Scope : boost::noncopyable {
    public:
        Scope();
        virtual ~Scope();
        
        virtual void reset() = 0;
        virtual void init( BSONObj * data ) = 0;
        void init( const char * data ){
            BSONObj o( data , 0 );
            init( &o );
        }
        
        
        virtual double getNumber( const char *field ) = 0;
        virtual string getString( const char *field ) = 0;
        virtual bool getBoolean( const char *field ) = 0;
        virtual BSONObj getObject( const char *field ) = 0;

        virtual int type( const char *field ) = 0;

        virtual void setNumber( const char *field , double val ) = 0;
        virtual void setString( const char *field , const char * val ) = 0;
        virtual void setObject( const char *field , const BSONObj& obj ) = 0;
        virtual void setBoolean( const char *field , bool val ) = 0;
        virtual void setThis( const BSONObj * obj ) = 0;
                    
        virtual ScriptingFunction createFunction( const char * code ) = 0;
        virtual int invoke( ScriptingFunction func , const BSONObj& args ) = 0;
        
        int invoke( const char* code , const BSONObj& args );
    };
    
    class ScriptEngine : boost::noncopyable {
    public:
        ScriptEngine();
        virtual ~ScriptEngine();
        
        virtual Scope * createScope() = 0;
        
        virtual void runTest() = 0;

        static void setup();
    };


    extern ScriptEngine * globalScriptEngine;
}
