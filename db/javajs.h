// java.h

#ifndef _JAVA_H_
#define _JAVA_H_

#include "jsobj.h"

class JavaJSImpl {
 public:
  JavaJSImpl();
  ~JavaJSImpl();
  
  long scopeCreate();
  jboolean scopeReset( long id );
  void scopeFree( long id );

  double scopeGetNumber( long id , char * field );
  char * scopeGetString( long id , char * field );
  
  long functionCreate( const char * code );
 
  int invoke( long scope , long functions , JSObj * obj  );
 
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

  jmethodID _functionCreate;
  
  jmethodID _invoke;

} JavaJS;


#endif _JAVA_H_
