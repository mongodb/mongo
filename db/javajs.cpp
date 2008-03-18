// java.cpp

#include "stdafx.h"
#include <iostream>
#include <map>
#include <list>

#undef yassert
#include <boost/filesystem/convenience.hpp>
#undef assert
#define assert xassert
#define yassert 1

using namespace boost::filesystem;          

#include "javajs.h"
#include "jsobj.h"

using namespace std;

JavaJSImpl JavaJS;

JavaJSImpl::JavaJSImpl(){

  char * ed = findEd();
  stringstream ss;

#if defined(_WIN32)
  char colon = ';';
#else
  char colon = ':';
#endif

  ss << "-Djava.class.path=.";
  ss << colon << ed << "/build/";
  
  {
    path includeDir(ed);
    includeDir /= "include";
    directory_iterator end;
    directory_iterator i(includeDir);
    while( i != end ) {
      path p = *i;
      ss << colon << p.string();
      i++;
    }
  }

#if defined(_WIN32)
  ss << colon << "C:\\Program Files\\Java\\jdk1.6.0_05\\lib\\tools.jar";
#else
  ss << colon << "/opt/java/lib/tools.jar";
#endif

  if( getenv( "CLASSPATH" ) )
	  ss << colon << getenv( "CLASSPATH" );

  string s = ss.str();
  char * p = (char *)malloc( s.size() * 4 );
  strcpy( p , s.c_str() );
  char *q = p;
#if defined(_WIN32)
  while( *p ) {
	  if( *p == '/' ) *p = '\\';
	  p++;
  }
#endif
  cout << "using classpath: " << q << endl;

  JavaVMOption options[2];
  options[0].optionString = q;
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
  jassert( _dbhook );

  _scopeCreate = _env->GetStaticMethodID( _dbhook , "scopeCreate" , "()J" );
  _scopeReset = _env->GetStaticMethodID( _dbhook , "scopeReset" , "(J)Z" );
  _scopeFree = _env->GetStaticMethodID( _dbhook , "scopeFree" , "(J)V" );

  _scopeGetNumber = _env->GetStaticMethodID( _dbhook , "scopeGetNumber" , "(JLjava/lang/String;)D" );
  _scopeGetString = _env->GetStaticMethodID( _dbhook , "scopeGetString" , "(JLjava/lang/String;)Ljava/lang/String;" );
  _scopeGetBoolean = _env->GetStaticMethodID( _dbhook , "scopeGetBoolean" , "(JLjava/lang/String;)Z" );
  _scopeGetType = _env->GetStaticMethodID( _dbhook , "scopeGetType" , "(JLjava/lang/String;)B" );
  _scopeGetObject = _env->GetStaticMethodID( _dbhook , "scopeGetObject" , "(JLjava/lang/String;Ljava/nio/ByteBuffer;)I" );
  _scopeGuessObjectSize = _env->GetStaticMethodID( _dbhook , "scopeGuessObjectSize" , "(JLjava/lang/String;)J" );
  
  _scopeSetNumber = _env->GetStaticMethodID( _dbhook , "scopeSetNumber" , "(JLjava/lang/String;D)Z" );
  _scopeSetBoolean = _env->GetStaticMethodID( _dbhook , "scopeSetBoolean" , "(JLjava/lang/String;Z)Z" );
  _scopeSetString = _env->GetStaticMethodID( _dbhook , "scopeSetString" , "(JLjava/lang/String;Ljava/lang/String;)Z" );
  _scopeSetObject = _env->GetStaticMethodID( _dbhook , "scopeSetObject" , "(JLjava/lang/String;Ljava/nio/ByteBuffer;)Z" );

  _functionCreate = _env->GetStaticMethodID( _dbhook , "functionCreate" , "(Ljava/lang/String;)J" );
  _invoke = _env->GetStaticMethodID( _dbhook , "invoke" , "(JJ)I" );
  
  jassert( _scopeCreate );  
  jassert( _scopeReset );
  jassert( _scopeFree );

  jassert( _scopeGetNumber );
  jassert( _scopeGetString );
  jassert( _scopeGetObject );
  jassert( _scopeGetBoolean );
  jassert( _scopeGetType );
  jassert( _scopeGuessObjectSize );

  jassert( _scopeSetNumber );
  jassert( _scopeSetBoolean );
  jassert( _scopeSetString );
  jassert( _scopeSetObject );

  jassert( _functionCreate );
  jassert( _invoke );

  //javajstest();  
}

JavaJSImpl::~JavaJSImpl(){
  if ( _jvm ){
    _jvm->DestroyJavaVM();
    cerr << "Destroying JVM" << endl;
  }
}

// scope

jlong JavaJSImpl::scopeCreate(){ 
  return _env->CallStaticLongMethod( _dbhook , _scopeCreate );
}

jboolean JavaJSImpl::scopeReset( jlong id ){
  return _env->CallStaticBooleanMethod( _dbhook , _scopeReset );
}

void JavaJSImpl::scopeFree( jlong id ){
  _env->CallStaticVoidMethod( _dbhook , _scopeFree );
}

// scope setters

bool JavaJSImpl::scopeSetNumber( jlong id , char * field , double val ){
  return _env->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , _env->NewStringUTF( field ) , val );
}

bool JavaJSImpl::scopeSetString( jlong id , char * field , char * val ){
  return _env->CallStaticBooleanMethod( _dbhook , _scopeSetString , id , _env->NewStringUTF( field ) , _env->NewStringUTF( val ) );
}

bool JavaJSImpl::scopeSetObject( jlong id , char * field , JSObj * obj ){
  jobject bb = 0;
  if ( obj )
    bb = _env->NewDirectByteBuffer( (void*)(obj->objdata()) , (obj->objsize()) );

  return _env->CallStaticBooleanMethod( _dbhook , _scopeSetObject , id , _env->NewStringUTF( field ) , bb );
}

// scope getters

double JavaJSImpl::scopeGetNumber( jlong id , char * field ){
  return _env->CallStaticDoubleMethod( _dbhook , _scopeGetNumber , id , _env->NewStringUTF( field ) );
}

char * JavaJSImpl::scopeGetString( jlong id , char * field ){
  jstring s = (jstring)_env->CallStaticObjectMethod( _dbhook , _scopeGetString , id , _env->NewStringUTF( field ) );
  if ( ! s )
    return 0;
  
  const char * c = _env->GetStringUTFChars( s , 0 );
  char * buf = new char[ strlen(c) + 1 ];
  strcpy( buf , c );
  _env->ReleaseStringUTFChars( s , c );

  return buf;
}

JSObj * JavaJSImpl::scopeGetObject( jlong id , char * field ){

  jlong guess = _env->CallStaticIntMethod( _dbhook , _scopeGuessObjectSize , id , _env->NewStringUTF( field ) );
  cout << "guess : " << guess << endl;

  char * buf = new char( (int) guess );
  jobject bb = _env->NewDirectByteBuffer( (void*)buf , guess );
  
  int len = _env->CallStaticIntMethod( _dbhook , _scopeGetObject , id , _env->NewStringUTF( field ) , bb );
  
  buf[len] = 0;
  return new JSObj( buf , true );
}

// other

jlong JavaJSImpl::functionCreate( const char * code ){
  jstring s = _env->NewStringUTF( code );  
  jassert( s );
  jlong id = _env->CallStaticLongMethod( _dbhook , _functionCreate , s );
  return id;
}
 
int JavaJSImpl::invoke( jlong scope , jlong function ){
  return _env->CallStaticIntMethod( _dbhook , _invoke , scope , function );
}

// --- fun run method

void JavaJSImpl::run( char * js ){
  jclass c = findClass( "ed/js/JS" );
  jassert( c );
  
  jmethodID m = _env->GetStaticMethodID( c , "eval" , "(Ljava/lang/String;)Ljava/lang/Object;" );
  jassert( m );
  
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
#if defined(_WIN32)

	return "c:/l/ed/ed";

#else

  static list<char*> possibleEdDirs;
  if ( ! possibleEdDirs.size() ){
    possibleEdDirs.push_back( "../../ed/ed/" ); // this one for dwight dev box
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
#endif
};

// ----

int javajstest(){

  jlong scope = JavaJS.scopeCreate();
  jlong func = JavaJS.functionCreate( "foo = 5.6; bar = \"eliot\"; abc = { foo : 517 }; " );

  JSObj * o = 0;

  cout << "scope : " << scope << endl;
  cout << "func : " << func << endl;  
  cout << "ret : " << JavaJS.invoke( scope , func ) << endl;
  
  cout << " foo : " << JavaJS.scopeGetNumber( scope , "foo" ) << endl;
  cout << " bar : " << JavaJS.scopeGetString( scope , "bar" ) << endl;
  
  JSObj * obj = JavaJS.scopeGetObject( scope , "abc" );
  cout << obj->toString() << endl;

  JavaJS.scopeSetObject( scope , "obj" , obj );
  cout << "func2 start" << endl;
  jlong func2 = JavaJS.functionCreate( "print( tojson( obj ) );" );
  cout << "\t here" << endl;
  jassert( ! JavaJS.invoke( scope , func2 ) );
  cout << "func2 end" << endl;

  cout << "func3 start" << endl;
  jassert( JavaJS.scopeSetNumber( scope , "a" , 5.17 ) );
  jassert( JavaJS.scopeSetString( scope , "b" , "eliot" ) );
  
  jlong func3 = JavaJS.functionCreate( "print( \"5.17 == \" + a );  print( \"eliot == \" + b ); " );
  jassert( ! JavaJS.invoke( scope , func3 ) );
  cout << "func3 end" << endl;

  cout << "func4 start" << endl;
  jassert( JavaJS.scopeSetObject( scope , "c" , obj ) );
  jlong func4 = JavaJS.functionCreate( "print( \"setObject : 517 == \" + c.foo );" );
  jassert( func4 );
  jassert( ! JavaJS.invoke( scope , func4 ) );
  cout << "func4 done" << endl;

  cout << "func5 start" << endl;
  jlong func5 = JavaJS.functionCreate( "function(){ z = true; print( \"this is fun\" ); } " );
  jassert( func5 );
  jassert( ! JavaJS.invoke( scope , func5 ) );
  jassert( JavaJS.scopeGetBoolean( scope , "z" ) );
  cout << "func5 done" << endl;

  

  return 0;

}

#if defined(_MAIN)
int main() { return javajstest(); }
#endif
