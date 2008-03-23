// java.h

#pragma once

#include "../stdafx.h"

#define J_USE_OBJ

#pragma pack()

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

char * findEd();

class JSObj;

class JavaJSImpl {
 public:
  JavaJSImpl();
  ~JavaJSImpl();
  
  jlong scopeCreate();
  jboolean scopeReset( jlong id );
  void scopeFree( jlong id );

  double scopeGetNumber( jlong id , const char * field );
  string scopeGetString( jlong id , const char * field );
  jboolean scopeGetBoolean( jlong id , const char * field ){
    return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeGetBoolean , id , _getEnv()->NewStringUTF( field ) );
  }
  JSObj scopeGetObject( jlong id , const char * field );
  char scopeGetType( jlong id , const char * field ){
    return _getEnv()->CallStaticByteMethod( _dbhook , _scopeGetType , id , _getEnv()->NewStringUTF( field ) );
  }

  int scopeSetNumber( jlong id , const char * field , double val );
  int scopeSetString( jlong id , const char * field , char * val );
  int scopeSetObject( jlong id , const char * field , JSObj * obj );
  int scopeSetBoolean( jlong id , const char * field , jboolean val ) {
      return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , _getEnv()->NewStringUTF( field ) , val );
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

// a javascript "scope"
class Scope { 
 public:
  Scope() { s = JavaJS->scopeCreate(); }
  ~Scope() { JavaJS->scopeFree(s); s = 0; }
  void reset() { JavaJS->scopeReset(s); }
  
  double getNumber(const char *field) { return JavaJS->scopeGetNumber(s,field); }
  string getString(const char *field) { return JavaJS->scopeGetString(s,field); }
  jboolean getBoolean(const char *field) { return JavaJS->scopeGetBoolean(s,field); }
  JSObj getObject(const char *field ) { return JavaJS->scopeGetObject(s,field); }
  int type(const char *field ) { return JavaJS->scopeGetType(s,field); }
  
  void setNumber(const char *field, double val ) { JavaJS->scopeSetNumber(s,field,val); }
  void setString(const char *field, char * val ) { JavaJS->scopeSetString(s,field,val); }
  void setObject(const char *field, JSObj& obj ) { JavaJS->scopeSetObject(s,field,&obj); }
  void setBoolean(const char *field, jboolean val ) { JavaJS->scopeSetBoolean(s,field,val); }
  
  int invoke(jlong function) { return JavaJS->invoke(s,function); }
  
  jlong s;
};

