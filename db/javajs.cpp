// java.cpp

#include "stdafx.h"
#include "javajs.h"
#include <iostream>
#include <map>
#include <list>

#undef yassert
#include <boost/filesystem/convenience.hpp>
#undef assert
#define assert xassert
#define yassert 1

using namespace boost::filesystem;          

#ifdef J_USE_OBJ
#include "jsobj.h"
#pragma message("warning: including jsobj.h")
#endif

#include "../grid/message.h"
#include "db.h"

using namespace std;

#if defined(_WIN32)
/* [dm] this being undefined without us adding it here means there is 
        no tss cleanup on windows for boost lib?
        we don't care for now esp on windows only

		the boost source says:

		  This function's sole purpose is to cause a link error in cases where
		  automatic tss cleanup is not implemented by Boost.Threads as a
		  reminder that user code is responsible for calling the necessary
		  functions at the appropriate times (and for implementing an a
		  tss_cleanup_implemented() function to eliminate the linker's
		  missing symbol error).

		  If Boost.Threads later implements automatic tss cleanup in cases
		  where it currently doesn't (which is the plan), the duplicate
		  symbol error will warn the user that their custom solution is no
		  longer needed and can be removed. 
*/
extern "C" void tss_cleanup_implemented(void) {
	//cout << "tss_cleanup_implemented called" << endl;
}
#endif

JavaJSImpl * JavaJS = 0;

void myJNIClean( JNIEnv * env ){
  JavaJS->detach( env );
}

JavaJSImpl::JavaJSImpl(){
  _jvm = 0; 
  _mainEnv = 0;
  _dbhook = 0;

  const char * ed = findEd();
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

  JavaVMOption * options = new JavaVMOption[3];
  options[0].optionString = q;
  options[1].optionString = (char*)"-Djava.awt.headless=true";
  options[2].optionString = (char*)"-Xmx300m";
// -Xcheck:jni  

  _vmArgs = new JavaVMInitArgs();
  _vmArgs->version = JNI_VERSION_1_4;
  _vmArgs->options = options;
  _vmArgs->nOptions = 3;
  _vmArgs->ignoreUnrecognized = JNI_FALSE;

  cerr << "Creating JVM" << endl;
  jint res = JNI_CreateJavaVM( &_jvm, (void**)&_mainEnv, _vmArgs );

  if( res ) {
	  cout << "using classpath: " << q << endl;
	  cerr
		  << "res : " << res << " " 
		  << "_jvm : " << _jvm  << " " 
		  << "_env : " << _mainEnv << " "
		  << endl;
  }

  jassert( res == 0 );
  jassert( _jvm > 0 );
  jassert( _mainEnv > 0 );    

  _envs = new boost::thread_specific_ptr<JNIEnv>( myJNIClean );
  assert( ! _envs->get() );
  _envs->reset( _mainEnv );

  _dbhook = findClass( "ed/db/JSHook" );
  if( _dbhook == 0 )
    cout << "using classpath: " << q << endl;
  jassert( _dbhook );

  {
    jmethodID init = _mainEnv->GetStaticMethodID( _dbhook ,  "init" , "(Ljava/lang/String;)V" );
    jassert( init );
    _mainEnv->CallStaticVoidMethod( _dbhook , init , _getEnv()->NewStringUTF( ed ) );
  }

  _dbjni = findClass( "ed/db/DBJni" );
  jassert( _dbjni );

  _scopeCreate = _mainEnv->GetStaticMethodID( _dbhook , "scopeCreate" , "()J" );
  _scopeReset = _mainEnv->GetStaticMethodID( _dbhook , "scopeReset" , "(J)Z" );
  _scopeFree = _mainEnv->GetStaticMethodID( _dbhook , "scopeFree" , "(J)V" );

  _scopeGetNumber = _mainEnv->GetStaticMethodID( _dbhook , "scopeGetNumber" , "(JLjava/lang/String;)D" );
  _scopeGetString = _mainEnv->GetStaticMethodID( _dbhook , "scopeGetString" , "(JLjava/lang/String;)Ljava/lang/String;" );
  _scopeGetBoolean = _mainEnv->GetStaticMethodID( _dbhook , "scopeGetBoolean" , "(JLjava/lang/String;)Z" );
  _scopeGetType = _mainEnv->GetStaticMethodID( _dbhook , "scopeGetType" , "(JLjava/lang/String;)B" );
  _scopeGetObject = _mainEnv->GetStaticMethodID( _dbhook , "scopeGetObject" , "(JLjava/lang/String;Ljava/nio/ByteBuffer;)I" );
  _scopeGuessObjectSize = _mainEnv->GetStaticMethodID( _dbhook , "scopeGuessObjectSize" , "(JLjava/lang/String;)J" );
  
  _scopeSetNumber = _mainEnv->GetStaticMethodID( _dbhook , "scopeSetNumber" , "(JLjava/lang/String;D)Z" );
  _scopeSetBoolean = _mainEnv->GetStaticMethodID( _dbhook , "scopeSetBoolean" , "(JLjava/lang/String;Z)Z" );
  _scopeSetString = _mainEnv->GetStaticMethodID( _dbhook , "scopeSetString" , "(JLjava/lang/String;Ljava/lang/String;)Z" );
  _scopeSetObject = _mainEnv->GetStaticMethodID( _dbhook , "scopeSetObject" , "(JLjava/lang/String;Ljava/nio/ByteBuffer;)Z" );

  _functionCreate = _mainEnv->GetStaticMethodID( _dbhook , "functionCreate" , "(Ljava/lang/String;)J" );
  _invoke = _mainEnv->GetStaticMethodID( _dbhook , "invoke" , "(JJ)I" );

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
  
  JNINativeMethod * nativeSay = new JNINativeMethod();
  nativeSay->name = (char*)"native_say";
  nativeSay->signature = (char*)"(Ljava/nio/ByteBuffer;)V";
  nativeSay->fnPtr = (void*)java_native_say;
  _mainEnv->RegisterNatives( _dbjni , nativeSay , 1 );


  JNINativeMethod * nativeCall = new JNINativeMethod();
  nativeCall->name = (char*)"native_call";
  nativeCall->signature = (char*)"(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)I";
  nativeCall->fnPtr = (void*)java_native_call;
  _mainEnv->RegisterNatives( _dbjni , nativeCall , 1 );

}

JavaJSImpl::~JavaJSImpl(){
  if ( _jvm ){
    _jvm->DestroyJavaVM();
    cerr << "Destroying JVM" << endl;
  }
}

// scope

jlong JavaJSImpl::scopeCreate(){ 
  return _getEnv()->CallStaticLongMethod( _dbhook , _scopeCreate );
}

jboolean JavaJSImpl::scopeReset( jlong id ){
  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeReset );
}

void JavaJSImpl::scopeFree( jlong id ){
  _getEnv()->CallStaticVoidMethod( _dbhook , _scopeFree );
}

// scope setters

int JavaJSImpl::scopeSetBoolean( jlong id , const char * field , jboolean val ) {
  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , _getEnv()->NewStringUTF( field ) , val );
}

int JavaJSImpl::scopeSetNumber( jlong id , const char * field , double val ){
  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , _getEnv()->NewStringUTF( field ) , val );
}

int JavaJSImpl::scopeSetString( jlong id , const char * field , const char * val ){
  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetString , id , _getEnv()->NewStringUTF( field ) , _getEnv()->NewStringUTF( val ) );
}

int JavaJSImpl::scopeSetObject( jlong id , const char * field , JSObj * obj ){
  jobject bb = 0;
  if ( obj ){
    //cout << "from c : " << obj->toString() << endl;
    bb = _getEnv()->NewDirectByteBuffer( (void*)(obj->objdata()) , (jlong)(obj->objsize()) );
    jassert( bb );
  }

  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetObject , id , _getEnv()->NewStringUTF( field ) , bb );
}

// scope getters

char JavaJSImpl::scopeGetType( jlong id , const char * field ){
  return _getEnv()->CallStaticByteMethod( _dbhook , _scopeGetType , id , _getEnv()->NewStringUTF( field ) );
}

double JavaJSImpl::scopeGetNumber( jlong id , const char * field ){
  return _getEnv()->CallStaticDoubleMethod( _dbhook , _scopeGetNumber , id , _getEnv()->NewStringUTF( field ) );
}

jboolean JavaJSImpl::scopeGetBoolean( jlong id , const char * field ){
  return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeGetBoolean , id , _getEnv()->NewStringUTF( field ) );
}

string JavaJSImpl::scopeGetString( jlong id , const char * field ) {
  jstring s = (jstring)_getEnv()->CallStaticObjectMethod( _dbhook , _scopeGetString , id , _getEnv()->NewStringUTF( field ) );
  if ( ! s )
    return "";
  
  const char * c = _getEnv()->GetStringUTFChars( s , 0 );
  string retStr(c);
  _getEnv()->ReleaseStringUTFChars( s , c );
  return retStr;
}

#ifdef J_USE_OBJ
JSObj JavaJSImpl::scopeGetObject( jlong id , const char * field ) 
{
  int guess = _getEnv()->CallStaticIntMethod( _dbhook , _scopeGuessObjectSize , id , _getEnv()->NewStringUTF( field ) );

  char * buf = (char *) malloc(guess);
  jobject bb = _getEnv()->NewDirectByteBuffer( (void*)buf , guess );
  jassert( bb );
  
  int len = _getEnv()->CallStaticIntMethod( _dbhook , _scopeGetObject , id , _getEnv()->NewStringUTF( field ) , bb );
  //cout << "len : " << len << endl;
  jassert( len > 0 && len < guess ); 

  JSObj obj(buf, true);
  assert( obj.objsize() <= guess );
  return obj;
}
#endif

// other

jlong JavaJSImpl::functionCreate( const char * code ){
  jstring s = _getEnv()->NewStringUTF( code );  
  jassert( s );
  jlong id = _getEnv()->CallStaticLongMethod( _dbhook , _functionCreate , s );
  return id;
}
 
int JavaJSImpl::invoke( jlong scope , jlong function ){
  return _getEnv()->CallStaticIntMethod( _dbhook , _invoke , scope , function );
}

// --- fun run method

void JavaJSImpl::run( const char * js ){
  jclass c = findClass( "ed/js/JS" );
  jassert( c );
  
  jmethodID m = _getEnv()->GetStaticMethodID( c , "eval" , "(Ljava/lang/String;)Ljava/lang/Object;" );
  jassert( m );
  
  jstring s = _getEnv()->NewStringUTF( js );
  cout << _getEnv()->CallStaticObjectMethod( c , m , s ) << endl;
}

void JavaJSImpl::printException(){
  jthrowable exc = _getEnv()->ExceptionOccurred();
  if ( exc ){
    _getEnv()->ExceptionDescribe();
    _getEnv()->ExceptionClear();
  }

}

JNIEnv * JavaJSImpl::_getEnv(){
  JNIEnv * env = _envs->get();
  if ( env )
    return env;

  int res = _jvm->AttachCurrentThread( (void**)&env , (void*)&_vmArgs );
  jassert( res == 0 );
  _envs->reset( env );
  return env;
}

void jasserted(const char *msg, const char *file, unsigned line) { 
  cout << "jassert failed " << msg << " " << file << " " << line << endl;
  if ( JavaJS ) JavaJS->printException();
  throw AssertionException();
}


const char * findEd(){

#if defined(_WIN32)
  return "c:/l/ed";
#else

  static list<const char*> possibleEdDirs;
  if ( ! possibleEdDirs.size() ){
    possibleEdDirs.push_back( "../../ed/ed/" ); // this one for dwight dev box
    possibleEdDirs.push_back( "../ed/" );
    possibleEdDirs.push_back( "../../ed/" );
  }

  for ( list<const char*>::iterator i = possibleEdDirs.begin() ; i != possibleEdDirs.end(); i++ ){
    const char * temp = *i;
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

// ---

JNIEXPORT void JNICALL java_native_say(JNIEnv * env , jclass, jobject outBuffer ){
  cerr << "native say called!" << endl;

  Message out( env->GetDirectBufferAddress( outBuffer ) , false );
  Message in;

  jniCallback( out , in );
}

JNIEXPORT jint JNICALL java_native_call(JNIEnv * env , jclass, jobject outBuffer , jobject inBuffer ){
  cerr << "native call called!" << endl;
  
  Message out( env->GetDirectBufferAddress( outBuffer ) , false );
  Message in;

  jniCallback( out , in );

  cerr << "in.data : " << in.data << endl;
  if ( in.data && in.data->len > 0 ){
    cerr << "copying data of len :" << in.data->len << endl;
    memcpy( env->GetDirectBufferAddress( inBuffer ) , in.data , in.data->len );
    return in.data->len;
  }

  return 0;
}

// ----

int javajstest() {

  const int debug = 0;

  JavaJSImpl& JavaJS = *::JavaJS;

  if ( debug ) cout << "about to create scope" << endl;
  jlong scope = JavaJS.scopeCreate();
  jassert( scope );
  if ( debug ) cout << "got scope" << endl;
         
  
  jlong func1 = JavaJS.functionCreate( "foo = 5.6; bar = \"eliot\"; abc = { foo : 517 }; " );
  jassert( ! JavaJS.invoke( scope , func1 ) );
  
  jassert( 5.6 == JavaJS.scopeGetNumber( scope , "foo" ) );
  jassert( ((string)"eliot") == JavaJS.scopeGetString( scope , "bar" ) );
  
  if ( debug ) cout << "func2 start" << endl;
  jassert( JavaJS.scopeSetNumber( scope , "a" , 5.17 ) );
  jassert( JavaJS.scopeSetString( scope , "b" , "eliot" ) );
  jlong func2 = JavaJS.functionCreate( "assert( 5.17 == a ); assert( \"eliot\" == b );" );
  jassert( ! JavaJS.invoke( scope , func2 ) );
  if ( debug ) cout << "func2 end" << endl;

  if ( debug ) cout << "func3 start" << endl;
  jlong func3 = JavaJS.functionCreate( "function(){ z = true; } " );
  jassert( func3 );
  jassert( ! JavaJS.invoke( scope , func3 ) );
  jassert( JavaJS.scopeGetBoolean( scope , "z" ) );
  if ( debug ) cout << "func3 done" << endl;

#ifdef J_USE_OBJ  
  
  if ( debug ) cout << "going to get object" << endl;
  JSObj obj = JavaJS.scopeGetObject( scope , "abc" );
  if ( debug ) cout << "done gettting object" << endl;
  
  if ( debug ){
    cout << "obj : " << obj.toString() << endl;
  }

  {
    time_t start = time(0);
    for ( int i=0; i<5000; i++ ){
      JavaJS.scopeSetObject( scope , "obj" , &obj );
    }
    time_t end = time(0);

	if( debug )
		cout << "time : " << ( end - start ) << endl;
  }
  
  if ( debug ) cout << "func4 start" << endl;    
  JavaJS.scopeSetObject( scope , "obj" , &obj );
  if ( debug ) cout << "\t here 1" << endl;
  jlong func4 = JavaJS.functionCreate( "tojson( obj );" );
  if ( debug ) cout << "\t here 2" << endl;
  jassert( ! JavaJS.invoke( scope , func4 ) );
  if ( debug ) cout << "func4 end" << endl;
  
  if ( debug ) cout << "func5 start" << endl;
  jassert( JavaJS.scopeSetObject( scope , "c" , &obj ) );
  jlong func5 = JavaJS.functionCreate( "assert( 517 == c.foo );" );
  jassert( func5 );
  jassert( ! JavaJS.invoke( scope , func5 ) );
  if ( debug ) cout << "func5 done" << endl;

#endif

  if ( debug ) cout << "func6 start" << endl;
  for ( int i=0; i<100; i++ ){
    double val = i + 5;
    JavaJS.scopeSetNumber( scope , "zzz" , val );
    jlong func6 = JavaJS.functionCreate( " xxx = zzz; " );
    jassert( ! JavaJS.invoke( scope , func6 ) );
	double n = JavaJS.scopeGetNumber( scope , "xxx" );
    jassert( val == n );
  }
  if ( debug ) cout << "func6 done" << endl;

  jlong func7 = JavaJS.functionCreate( "return 11;" );
  jassert( ! JavaJS.invoke( scope , func7 ) );
  assert( 11 == JavaJS.scopeGetNumber( scope , "return" ) );

  scope = JavaJS.scopeCreate();
  jlong func8 = JavaJS.functionCreate( "function(){ return 12; }" );
  jassert( ! JavaJS.invoke( scope , func8 ) );
  assert( 12 == JavaJS.scopeGetNumber( scope , "return" ) );

  
  return 0;

}

#if defined(_MAIN)
int main() { return javajstest(); }
#endif
