// engine_java.h

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

/* this file contains code to call into java (into the 10gen sandbox) from inside the database */

#pragma once

#include "../pch.h"

#include <jni.h>
#include <errno.h>
#include <sys/types.h>

#if !defined(_WIN32)
#include <dirent.h>
#endif

#include "../db/jsobj.h"

#include "engine.h"

namespace mongo {

    void jasserted(const char *msg, const char *file, unsigned line);
#define jassert(_Expression) if ( ! ( _Expression ) ){ jasserted(#_Expression, __FILE__, __LINE__); }

    const char * findEd();
    const char * findEd(const char *);
    const string findJars();

    class BSONObj;

    class JavaJSImpl : public ScriptEngine {
    public:
        JavaJSImpl(const char * = 0);
        ~JavaJSImpl();

        jlong scopeCreate();
        int scopeInit( jlong id , const BSONObj * obj );
        int scopeSetThis( jlong id , const BSONObj * obj );
        jboolean scopeReset( jlong id );
        void scopeFree( jlong id );

        double scopeGetNumber( jlong id , const char * field );
        string scopeGetString( jlong id , const char * field );
        jboolean scopeGetBoolean( jlong id , const char * field );
        BSONObj scopeGetObject( jlong id , const char * field );
        char scopeGetType( jlong id , const char * field );

        int scopeSetNumber( jlong id , const char * field , double val );
        int scopeSetString( jlong id , const char * field , const char * val );
        int scopeSetObject( jlong id , const char * field , const BSONObj * obj );
        int scopeSetBoolean( jlong id , const char * field , jboolean val );

        jlong functionCreate( const char * code );

        /* return values:
           public static final int NO_SCOPE = -1;
           public static final int NO_FUNCTION = -2;
           public static final int INVOKE_ERROR = -3;
           public static final int INVOKE_SUCCESS = 0;
          */
        int invoke( jlong scope , jlong function );

        void printException();

        void run( const char * js );

        void detach( JNIEnv * env ) {
            _jvm->DetachCurrentThread();
        }

        Scope * createScope();

        void runTest();
    private:

        jobject create( const char * name ) {
            jclass c = findClass( name );
            if ( ! c )
                return 0;

            jmethodID cons = _getEnv()->GetMethodID( c , "<init>" , "()V" );
            if ( ! cons )
                return 0;

            return _getEnv()->NewObject( c , cons );
        }

        jclass findClass( const char * name ) {
            return _getEnv()->FindClass( name );
        }


    private:

        JNIEnv * _getEnv();

        JavaVM * _jvm;
        JNIEnv * _mainEnv;
        JavaVMInitArgs * _vmArgs;

        boost::thread_specific_ptr<JNIEnv> * _envs;

        jclass _dbhook;
        jclass _dbjni;

        jmethodID _scopeCreate;
        jmethodID _scopeInit;
        jmethodID _scopeSetThis;
        jmethodID _scopeReset;
        jmethodID _scopeFree;

        jmethodID _scopeGetNumber;
        jmethodID _scopeGetString;
        jmethodID _scopeGetObject;
        jmethodID _scopeGetBoolean;
        jmethodID _scopeGuessObjectSize;
        jmethodID _scopeGetType;

        jmethodID _scopeSetNumber;
        jmethodID _scopeSetString;
        jmethodID _scopeSetObject;
        jmethodID _scopeSetBoolean;

        jmethodID _functionCreate;

        jmethodID _invoke;

    };

    extern JavaJSImpl *JavaJS;

// a javascript "scope"
    class JavaScope : public Scope {
    public:
        JavaScope() {
            s = JavaJS->scopeCreate();
        }
        virtual ~JavaScope() {
            JavaJS->scopeFree(s);
            s = 0;
        }
        void reset() {
            JavaJS->scopeReset(s);
        }

        void init( BSONObj * o ) {
            JavaJS->scopeInit( s , o );
        }

        void localConnect( const char * dbName ) {
            setString("$client", dbName );
        }

        double getNumber(const char *field) {
            return JavaJS->scopeGetNumber(s,field);
        }
        string getString(const char *field) {
            return JavaJS->scopeGetString(s,field);
        }
        bool getBoolean(const char *field) {
            return JavaJS->scopeGetBoolean(s,field);
        }
        BSONObj getObject(const char *field ) {
            return JavaJS->scopeGetObject(s,field);
        }
        int type(const char *field ) {
            return JavaJS->scopeGetType(s,field);
        }

        void setThis( const BSONObj * obj ) {
            JavaJS->scopeSetThis( s , obj );
        }

        void setNumber(const char *field, double val ) {
            JavaJS->scopeSetNumber(s,field,val);
        }
        void setString(const char *field, const char * val ) {
            JavaJS->scopeSetString(s,field,val);
        }
        void setObject(const char *field, const BSONObj& obj , bool readOnly ) {
            uassert( 10211 ,  "only readOnly setObject supported in java" , readOnly );
            JavaJS->scopeSetObject(s,field,&obj);
        }
        void setBoolean(const char *field, bool val ) {
            JavaJS->scopeSetBoolean(s,field,val);
        }

        ScriptingFunction createFunction( const char * code ) {
            return JavaJS->functionCreate( code );
        }

        int invoke( ScriptingFunction function , const BSONObj& args ) {
            setObject( "args" , args , true );
            return JavaJS->invoke(s,function);
        }

        string getError() {
            return getString( "error" );
        }

        jlong s;
    };

    JNIEXPORT void JNICALL java_native_say(JNIEnv *, jclass, jobject outBuffer );
    JNIEXPORT jint JNICALL java_native_call(JNIEnv *, jclass, jobject outBuffer , jobject inBuffer );

} // namespace mongo
