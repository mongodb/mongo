// javajs.h

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* this file contains code to call into java (into the 10gen sandbox) from inside the database */

#pragma once

#include "../stdafx.h"

#define J_USE_OBJ

#include <jni.h>
#include <boost/thread/tss.hpp>
#include <errno.h>
#include <sys/types.h>

#if !defined(_WIN32)
#include <dirent.h>
#endif

#include "jsobj.h"

void jasserted(const char *msg, const char *file, unsigned line);
#define jassert(_Expression) if ( ! ( _Expression ) ){ jasserted(#_Expression, __FILE__, __LINE__); }

int javajstest();

const char * findEd();
const char * findEd(const char *);

class JSObj;

class JavaJSImpl {
 public:
  JavaJSImpl();
  JavaJSImpl(const char *);
  ~JavaJSImpl();
  
  jlong scopeCreate();
  int scopeInit( jlong id , JSObj * obj );
  int scopeSetThis( jlong id , JSObj * obj );
  jboolean scopeReset( jlong id );
  void scopeFree( jlong id );

  double scopeGetNumber( jlong id , const char * field );
  string scopeGetString( jlong id , const char * field );
  jboolean scopeGetBoolean( jlong id , const char * field );
  JSObj scopeGetObject( jlong id , const char * field );
  char scopeGetType( jlong id , const char * field );

  int scopeSetNumber( jlong id , const char * field , double val );
  int scopeSetString( jlong id , const char * field , const char * val );
  int scopeSetObject( jlong id , const char * field , JSObj * obj );
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

  void detach( JNIEnv * env ){
    _jvm->DetachCurrentThread();
  }

 private:
  
  jobject create( const char * name ){
    jclass c = findClass( name );
    if ( ! c )
      return 0;
    
    jmethodID cons = _getEnv()->GetMethodID( c , "<init>" , "()V" );
    if ( ! cons )
      return 0;

    return _getEnv()->NewObject( c , cons );
  }

  jclass findClass( const char * name ){
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
class Scope { 
 public:
  Scope() { s = JavaJS->scopeCreate(); }
  ~Scope() { JavaJS->scopeFree(s); s = 0; }
  void reset() { JavaJS->scopeReset(s); }

  void init( const char * data ){
    JSObj o( data , 0 );
    JavaJS->scopeInit( s , & o );
  }
  
  double getNumber(const char *field) { return JavaJS->scopeGetNumber(s,field); }
  string getString(const char *field) { return JavaJS->scopeGetString(s,field); }
  jboolean getBoolean(const char *field) { return JavaJS->scopeGetBoolean(s,field); }
  JSObj getObject(const char *field ) { return JavaJS->scopeGetObject(s,field); }
  int type(const char *field ) { return JavaJS->scopeGetType(s,field); }
  
  void setNumber(const char *field, double val ) { JavaJS->scopeSetNumber(s,field,val); }
  void setString(const char *field, const char * val ) { JavaJS->scopeSetString(s,field,val); }
  void setObject(const char *field, JSObj& obj ) { JavaJS->scopeSetObject(s,field,&obj); }
  void setBoolean(const char *field, jboolean val ) { JavaJS->scopeSetBoolean(s,field,val); }
  
  int invoke(jlong function) { return JavaJS->invoke(s,function); }
  
  jlong s;
};

JNIEXPORT void JNICALL java_native_say(JNIEnv *, jclass, jobject outBuffer );
JNIEXPORT jint JNICALL java_native_call(JNIEnv *, jclass, jobject outBuffer , jobject inBuffer );
