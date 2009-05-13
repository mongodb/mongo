// engine.h

#pragma once

#include "../stdafx.h"
#include "../db/jsobj.h"

extern const char * jsconcatcode; // TODO: change name to mongoJSCode

namespace mongo {

    typedef unsigned long long ScriptingFunction;
    typedef BSONObj (*NativeFunction) ( const BSONObj &args );
    
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
        
        virtual void localConnect( const char * dbName ) = 0;
        
        virtual double getNumber( const char *field ) = 0;
        virtual string getString( const char *field ) = 0;
        virtual bool getBoolean( const char *field ) = 0;
        virtual BSONObj getObject( const char *field ) = 0;

        virtual int type( const char *field ) = 0;

        virtual void setNumber( const char *field , double val ) = 0;
        virtual void setString( const char *field , const char * val ) = 0;
        virtual void setObject( const char *field , const BSONObj& obj , bool readOnly=true ) = 0;
        virtual void setBoolean( const char *field , bool val ) = 0;
        virtual void setThis( const BSONObj * obj ) = 0;
                    
        virtual ScriptingFunction createFunction( const char * code ) = 0;

        /**
         * @return 0 on success
         */
        virtual int invoke( ScriptingFunction func , const BSONObj& args ) = 0;
        virtual string getError() = 0;
        
        int invoke( const char* code , const BSONObj& args );
        
        virtual bool exec( const string& code , const string& name , bool printResult , bool reportError , bool assertOnError ) = 0;
        virtual bool execFile( const string& filename , bool printResult , bool reportError , bool assertOnError );
        
        virtual void injectNative( const char *field, NativeFunction func ) {}
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
