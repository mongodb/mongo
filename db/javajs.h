// java.h

#pragma once

#include <jni.h>

#include "../stdafx.h"
//#include "jsobj.h"

#include <sys/types.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <errno.h>

void jasserted(const char *msg, const char *file, unsigned line);
#define jassert(_Expression) (void)( (!!(_Expression)) || (jasserted(#_Expression, __FILE__, __LINE__), 0) )

int javajstest();

char * findEd();

class JSObj;
class JavaJSImpl {
 public:
  JavaJSImpl();
  ~JavaJSImpl();
  
  jlong scopeCreate();
  jboolean scopeReset( jlong id );
  void scopeFree( jlong id );

  double scopeGetNumber( jlong id , char * field );
  char * scopeGetString( jlong id , char * field );
  JSObj * scopeGetObject( jlong id , char * field );

  bool scopeSetNumber( jlong id , char * field , double val );
  bool scopeSetString( jlong id , char * field , char * val );
  bool scopeSetObject( jlong id , char * field , JSObj * obj );
  
  jlong functionCreate( const char * code );
 
  /* return values:
     public static final int NO_SCOPE = -1;
	 public static final int NO_FUNCTION = -2;
	 public static final int INVOKE_ERROR = -3;
	 public static final int INVOKE_SUCCESS = 0;
	*/
  int invoke( jlong scope , jlong function );

  void printException();
 
 private:

  void run( char * js );
  
  jobject create( const char * name ){
    jclass c = findClass( name );
    if ( ! c )
      return 0;
    
    jmethodID cons = _env->GetMethodID( c , "<init>" , "()V" );
    if ( ! cons )
      return 0;

    return _env->NewObject( c , cons );
  }

  jclass findClass( const char * name ){
    return _env->FindClass( name );
  }


 private:
  JavaVM * _jvm;
  JNIEnv * _env;

  jclass _dbhook;
  
  jmethodID _scopeCreate;
  jmethodID _scopeReset;
  jmethodID _scopeFree;

  jmethodID _scopeGetNumber;
  jmethodID _scopeGetString;
  jmethodID _scopeGetObject;
  jmethodID _scopeGuessObjectSize;

  jmethodID _scopeSetNumber;
  jmethodID _scopeSetString;
  jmethodID _scopeSetObject;

  jmethodID _functionCreate;
  
  jmethodID _invoke;

};

extern JavaJSImpl JavaJS;
