// java.h

#pragma once

#include "../stdafx.h"

#define J_USE_OBJ

#pragma pack()
#include <jni.h>

#include <sys/types.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <errno.h>

void jasserted(const char *msg, const char *file, unsigned line);
#define jassert(_Expression) if ( ! ( _Expression ) ){ jasserted(#_Expression, __FILE__, __LINE__); }

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
  jboolean scopeGetBoolean( jlong id , char * field ){
    return _env->CallStaticBooleanMethod( _dbhook , _scopeGetBoolean , id , _env->NewStringUTF( field ) );
  }
#ifdef J_USE_OBJ
  JSObj * scopeGetObject( jlong id , char * field );
#endif
  char scopeGetType( jlong id , char * field ){
    return _env->CallStaticByteMethod( _dbhook , _scopeGetType , id , _env->NewStringUTF( field ) );
  }


  int scopeSetNumber( jlong id , char * field , double val );
  int scopeSetString( jlong id , char * field , char * val );
#ifdef J_USE_OBJ
  int scopeSetObject( jlong id , char * field , JSObj * obj );
#endif
  int scopeSetBoolean( jlong id , char * field , jboolean val ){
      return _env->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , _env->NewStringUTF( field ) , val );
  }
  
  jlong functionCreate( const char * code );
 
  /* return values:
     public static final int NO_SCOPE = -1;
	 public static final int NO_FUNCTION = -2;
	 public static final int INVOKE_ERROR = -3;
	 public static final int INVOKE_SUCCESS = 0;
	*/
  int invoke( jlong scope , jlong function );

  void printException();
 
  void run( char * js );
 private:
  
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
