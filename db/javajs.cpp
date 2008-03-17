// java.cpp

#include "stdafx.h"

#include <jni.h>

#include <iostream>
#include <assert.h>
#include <map>
#include <list>


#include "javajs.h"

using namespace std;

JavaJSImpl::JavaJSImpl(){

  char * ed = findEd();
  stringstream ss;

  ss << "-Djava.class.path=.";
  ss << ":" << ed << "/build/";

  { 
    char includeDir[256];
    sprintf( includeDir , "%s/include" , ed );
    DIR * dir = opendir( includeDir );
    
    struct dirent * entry;
    
    while ( ( entry = readdir(dir) ) ){
      ss << ":" << ed << "/include/" << entry->d_name;
    }

    closedir( dir );
  }

  cout << "using classpath : " << ss.str() << endl;

  char classpath[4096];
  sprintf( classpath , "%s:%s" , 
           ss.str().c_str() ,
           getenv( "CLASSPATH" ) ? getenv( "CLASSPATH" ) : "" );

  JavaVMOption options[2];
  options[0].optionString = classpath;
  options[1].optionString = "-Djava.awt.headless=true";
  
  JavaVMInitArgs vm_args;  
  vm_args.version = JNI_VERSION_1_4;
  vm_args.options = options;
  vm_args.nOptions = 2;
  vm_args.ignoreUnrecognized = JNI_TRUE;

  cerr << "Creating JVM" << endl;
  jint res = JNI_CreateJavaVM( &_jvm, (void**) &_env, &vm_args);
  if ( res )
    throw "couldn't make jni ";

  assert( _jvm );

  _dbhook = findClass( "ed/js/DBHook" );
  assert( _dbhook );

  _scopeCreate = _env->GetStaticMethodID( _dbhook , "scopeCreate" , "()J" );
  _scopeReset = _env->GetStaticMethodID( _dbhook , "scopeReset" , "(J)Z" );
  _scopeFree = _env->GetStaticMethodID( _dbhook , "scopeFree" , "(J)V" );

  _scopeGetNumber = _env->GetStaticMethodID( _dbhook , "scopeGetNumber" , "(JLjava/lang/String;)D" );
  _scopeGetString = _env->GetStaticMethodID( _dbhook , "scopeGetString" , "(JLjava/lang/String;)Ljava/lang/String;" );
  _scopeGetObject = _env->GetStaticMethodID( _dbhook , "scopeGetObject" , "(JLjava/lang/String;Ljava/nio/ByteBuffer;)I" );
  _scopeGuessObjectSize = _env->GetStaticMethodID( _dbhook , "scopeGuessObjectSize" , "(JLjava/lang/String;)J" );

  _functionCreate = _env->GetStaticMethodID( _dbhook , "functionCreate" , "(Ljava/lang/String;)J" );
  _invoke = _env->GetStaticMethodID( _dbhook , "invoke" , "(JJLjava/nio/ByteBuffer;)I" );
  
  assert( _scopeCreate );  
  assert( _scopeReset );
  assert( _scopeFree );

  assert( _scopeGetNumber );
  assert( _scopeGetString );
  assert( _scopeGetObject );
  assert( _scopeGuessObjectSize );

  assert( _functionCreate );
  assert( _invoke );
  
}

JavaJSImpl::~JavaJSImpl(){
  if ( _jvm ){
    _jvm->DestroyJavaVM();
    cerr << "Destroying JVM" << endl;
  }
}

// scope

long JavaJSImpl::scopeCreate(){ 
  return _env->CallStaticLongMethod( _dbhook , _scopeCreate );
}

jboolean JavaJSImpl::scopeReset( long id ){
  return _env->CallStaticBooleanMethod( _dbhook , _scopeReset );
}

void JavaJSImpl::scopeFree( long id ){
  _env->CallStaticVoidMethod( _dbhook , _scopeFree );
}

// scope getters

double JavaJSImpl::scopeGetNumber( long id , char * field ){
  return _env->CallStaticDoubleMethod( _dbhook , _scopeGetNumber , (jlong)id , _env->NewStringUTF( field ) );
}

char * JavaJSImpl::scopeGetString( long id , char * field ){
  jstring s = (jstring)_env->CallStaticObjectMethod( _dbhook , _scopeGetString , (jlong)id , _env->NewStringUTF( field ) );
  if ( ! s )
    return 0;
  
  const char * c = _env->GetStringUTFChars( s , 0 );
  char * buf = new char[ strlen(c) + 1 ];
  strcpy( buf , c );
  _env->ReleaseStringUTFChars( s , c );

  return buf;
}

JSObj * JavaJSImpl::scopeGetObject( long id , char * field ){

  long guess = _env->CallStaticIntMethod( _dbhook , _scopeGuessObjectSize , (jlong)id , _env->NewStringUTF( field ) );
  cout << "guess : " << guess << endl;

  char * buf = new char( guess );
  jobject bb = _env->NewDirectByteBuffer( (void*)buf , (jlong)guess );
  
  int len = _env->CallStaticIntMethod( _dbhook , _scopeGetObject , (jlong)id , _env->NewStringUTF( field ) , bb );
  cout << "len : " << len << endl;
  
  buf[len] = 0;
  return new JSObj( buf , true );
}

// other

long JavaJSImpl::functionCreate( const char * code ){
  jstring s = _env->NewStringUTF( code );  
  jassert( s );
  long id = _env->CallStaticLongMethod( _dbhook , _functionCreate , s );
  return id;
}
 
int JavaJSImpl::invoke( long scope , long function , JSObj * obj  ){
  jobject bb = 0;
  
  if ( obj )
    bb = _env->NewDirectByteBuffer( (void*)(obj->objdata()) , (jlong)(obj->objsize()) );
  
  int ret = _env->CallStaticIntMethod( _dbhook , _invoke , (jlong)scope , (jlong)function , bb );
  return ret;
}

// --- fun run method

void JavaJSImpl::run( char * js ){
  jclass c = findClass( "ed/js/JS" );
  assert( c );
    
  jmethodID m = _env->GetStaticMethodID( c , "eval" , "(Ljava/lang/String;)Ljava/lang/Object;" );
  assert( m );
  
  jstring s = _env->NewStringUTF( js );
  cout << _env->CallStaticObjectMethod( c , m , s ) << endl;
}

void JavaJSImpl::printException(){
  jthrowable exc = _env->ExceptionOccurred();
  if ( exc ){
    _env->ExceptionDescribe();
    _env->ExceptionClear();
  }

}

void jasserted(const char *msg, const char *file, unsigned line) { 
  
  cout << "Java Assertion failure " << msg << " " << file << " " << line << endl;
  JavaJS.printException();
  throw AssertionException();
}


char * findEd(){

  static list<char*> possibleEdDirs;
  if ( ! possibleEdDirs.size() ){
    possibleEdDirs.push_back( "../ed/" );
    possibleEdDirs.push_back( "../../ed/" );
  }

  for ( list<char*>::iterator i = possibleEdDirs.begin() ; i != possibleEdDirs.end(); i++ ){
    char * temp = *i;
    DIR * test = opendir( temp );
    if ( ! test )
      continue;
    
    closedir( test );
    cout << "found ed at : " << temp << endl;
    return temp;
  }
  
  return 0;
};

// ----

int main(){

  long scope = JavaJS.scopeCreate();
  long func = JavaJS.functionCreate( "print( Math.random() ); foo = 5.6; bar = \"eliot\"; abc = { foo : 517 }; " );

  JSObj * o = 0;

  cout << "scope : " << scope << endl;
  cout << "func : " << func << endl;  
  cout << "ret : " << JavaJS.invoke( scope , func , o ) << endl;
  
  cout << " foo : " << JavaJS.scopeGetNumber( scope , "foo" ) << endl;
  cout << " bar : " << JavaJS.scopeGetString( scope , "bar" ) << endl;
  
  JSObj * obj = JavaJS.scopeGetObject( scope , "abc" );
  cout << obj->toString() << endl;

  long func2 = JavaJS.functionCreate( "print( tojson( obj ) );" );
  JavaJS.invoke( scope , func2 , obj );

  return 0;

}
