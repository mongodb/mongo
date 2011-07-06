// java.cpp

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


#include "pch.h"
#include "engine_java.h"
#include <iostream>
#include <map>
#include <list>

#include "../db/jsobj.h"
#include "../db/db.h"

using namespace boost::filesystem;

namespace mongo {

//#define JNI_DEBUG 1

#ifdef JNI_DEBUG
#undef JNI_DEBUG
#define JNI_DEBUG(x) cout << x << endl
#else
#undef JNI_DEBUG
#define JNI_DEBUG(x)
#endif

} // namespace mongo



#include "../util/net/message.h"
#include "../db/db.h"

using namespace std;

namespace mongo {

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
        //out() << "tss_cleanup_implemented called" << endl;
    }
#endif

    JavaJSImpl * JavaJS = 0;
    extern string dbExecCommand;

#if !defined(NOJNI)

    void myJNIClean( JNIEnv * env ) {
        JavaJS->detach( env );
    }

#if defined(_WIN32)
    const char SYSTEM_COLON = ';';
#else
    const char SYSTEM_COLON = ':';
#endif


    void _addClassPath( const char * ed , stringstream & ss , const char * subdir ) {
        path includeDir(ed);
        includeDir /= subdir;
        directory_iterator end;
        try {
            directory_iterator i(includeDir);
            while ( i != end ) {
                path p = *i;
                ss << SYSTEM_COLON << p.string();
                i++;
            }
        }
        catch (...) {
            problem() << "exception looking for ed class path includeDir: " << includeDir.string() << endl;
            sleepsecs(3);
            dbexit( EXIT_JAVA );
        }
    }


    JavaJSImpl::JavaJSImpl(const char *appserverPath) {
        _jvm = 0;
        _mainEnv = 0;
        _dbhook = 0;

        stringstream ss;
        string edTemp;

        const char * ed = 0;
        ss << "-Djava.class.path=.";

        if ( appserverPath ) {
            ed = findEd(appserverPath);
            assert( ed );

            ss << SYSTEM_COLON << ed << "/build/";

            _addClassPath( ed , ss , "include" );
            _addClassPath( ed , ss , "include/jython/" );
            _addClassPath( ed , ss , "include/jython/javalib" );
        }
        else {
            const string jars = findJars();
            _addClassPath( jars.c_str() , ss , "jars" );

            edTemp += (string)jars + "/jars/mongojs-js.jar";
            ed = edTemp.c_str();
        }



#if defined(_WIN32)
        ss << SYSTEM_COLON << "C:\\Program Files\\Java\\jdk\\lib\\tools.jar";
#else
        ss << SYSTEM_COLON << "/opt/java/lib/tools.jar";
#endif

        if ( getenv( "CLASSPATH" ) )
            ss << SYSTEM_COLON << getenv( "CLASSPATH" );

        string s = ss.str();
        char * p = (char *)malloc( s.size() * 4 );
        strcpy( p , s.c_str() );
        char *q = p;
#if defined(_WIN32)
        while ( *p ) {
            if ( *p == '/' ) *p = '\\';
            p++;
        }
#endif

        log(1) << "classpath: " << q << endl;

        JavaVMOption * options = new JavaVMOption[4];
        options[0].optionString = q;
        options[1].optionString = (char*)"-Djava.awt.headless=true";
        options[2].optionString = (char*)"-Xmx300m";

        // Prevent JVM from using async signals internally, since on linux the pre-installed handlers for these
        // signals don't seem to be respected by JNI.
        options[3].optionString = (char*)"-Xrs";
        // -Xcheck:jni

        _vmArgs = new JavaVMInitArgs();
        _vmArgs->version = JNI_VERSION_1_4;
        _vmArgs->options = options;
        _vmArgs->nOptions = 4;
        _vmArgs->ignoreUnrecognized = JNI_FALSE;

        log(1) << "loading JVM" << endl;
        jint res = JNI_CreateJavaVM( &_jvm, (void**)&_mainEnv, _vmArgs );

        if ( res ) {
            log() << "using classpath: " << q << endl;
            log()
                    << " res : " << (unsigned) res << " "
                    << "_jvm : " << _jvm  << " "
                    << "_env : " << _mainEnv << " "
                    << endl;
            problem() << "Couldn't create JVM res:" << (int) res << " terminating" << endl;
            log() << "(try --nojni if you do not require that functionality)" << endl;
            exit(22);
        }
        jassert( res == 0 );
        jassert( _jvm > 0 );
        jassert( _mainEnv > 0 );

        _envs = new boost::thread_specific_ptr<JNIEnv>( myJNIClean );
        assert( ! _envs->get() );
        _envs->reset( _mainEnv );

        _dbhook = findClass( "ed/db/JSHook" );
        if ( _dbhook == 0 ) {
            log() << "using classpath: " << q << endl;
            printException();
        }
        jassert( _dbhook );

        if ( ed ) {
            jmethodID init = _mainEnv->GetStaticMethodID( _dbhook ,  "init" , "(Ljava/lang/String;)V" );
            jassert( init );
            _mainEnv->CallStaticVoidMethod( _dbhook , init , _getEnv()->NewStringUTF( ed ) );
        }

        _dbjni = findClass( "ed/db/DBJni" );
        jassert( _dbjni );

        _scopeCreate = _mainEnv->GetStaticMethodID( _dbhook , "scopeCreate" , "()J" );
        _scopeInit = _mainEnv->GetStaticMethodID( _dbhook , "scopeInit" , "(JLjava/nio/ByteBuffer;)Z" );
        _scopeSetThis = _mainEnv->GetStaticMethodID( _dbhook , "scopeSetThis" , "(JLjava/nio/ByteBuffer;)Z" );
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
        jassert( _scopeInit );
        jassert( _scopeSetThis );
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

    JavaJSImpl::~JavaJSImpl() {
        if ( _jvm ) {
            _jvm->DestroyJavaVM();
            cout << "Destroying JVM" << endl;
        }
    }

// scope

    jlong JavaJSImpl::scopeCreate() {
        return _getEnv()->CallStaticLongMethod( _dbhook , _scopeCreate );
    }

    jboolean JavaJSImpl::scopeReset( jlong id ) {
        return _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeReset );
    }

    void JavaJSImpl::scopeFree( jlong id ) {
        _getEnv()->CallStaticVoidMethod( _dbhook , _scopeFree , id );
    }

// scope setters

    int JavaJSImpl::scopeSetBoolean( jlong id , const char * field , jboolean val ) {
        jstring fieldString = _getEnv()->NewStringUTF( field );
        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , fieldString  , val );
        _getEnv()->DeleteLocalRef( fieldString );
        return res;
    }

    int JavaJSImpl::scopeSetNumber( jlong id , const char * field , double val ) {
        jstring fieldString = _getEnv()->NewStringUTF( field );
        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetNumber , id , fieldString , val );
        _getEnv()->DeleteLocalRef( fieldString );
        return res;
    }

    int JavaJSImpl::scopeSetString( jlong id , const char * field , const char * val ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        jstring s2 = _getEnv()->NewStringUTF( val );
        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetString , id , s1 , s2 );
        _getEnv()->DeleteLocalRef( s1 );
        _getEnv()->DeleteLocalRef( s2 );
        return res;
    }

    int JavaJSImpl::scopeSetObject( jlong id , const char * field , const BSONObj * obj ) {
        jobject bb = 0;
        if ( obj ) {
            bb = _getEnv()->NewDirectByteBuffer( (void*)(obj->objdata()) , (jlong)(obj->objsize()) );
            jassert( bb );
        }

        jstring s1 = _getEnv()->NewStringUTF( field );
        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetObject , id , s1 , bb );
        _getEnv()->DeleteLocalRef( s1 );
        if ( bb )
            _getEnv()->DeleteLocalRef( bb );

        return res;
    }

    int JavaJSImpl::scopeInit( jlong id , const BSONObj * obj ) {
        if ( ! obj )
            return 0;

        jobject bb = _getEnv()->NewDirectByteBuffer( (void*)(obj->objdata()) , (jlong)(obj->objsize()) );
        jassert( bb );

        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeInit , id , bb );
        _getEnv()->DeleteLocalRef( bb );
        return res;
    }

    int JavaJSImpl::scopeSetThis( jlong id , const BSONObj * obj ) {
        if ( ! obj )
            return 0;

        jobject bb = _getEnv()->NewDirectByteBuffer( (void*)(obj->objdata()) , (jlong)(obj->objsize()) );
        jassert( bb );

        int res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeSetThis , id , bb );
        _getEnv()->DeleteLocalRef( bb );
        return res;
    }

// scope getters

    char JavaJSImpl::scopeGetType( jlong id , const char * field ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        int res =_getEnv()->CallStaticByteMethod( _dbhook , _scopeGetType , id , s1 );
        _getEnv()->DeleteLocalRef( s1 );
        return res;
    }

    double JavaJSImpl::scopeGetNumber( jlong id , const char * field ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        double res = _getEnv()->CallStaticDoubleMethod( _dbhook , _scopeGetNumber , id , s1 );
        _getEnv()->DeleteLocalRef( s1 );
        return res;
    }

    jboolean JavaJSImpl::scopeGetBoolean( jlong id , const char * field ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        jboolean res = _getEnv()->CallStaticBooleanMethod( _dbhook , _scopeGetBoolean , id , s1 );
        _getEnv()->DeleteLocalRef( s1 );
        return res;
    }

    string JavaJSImpl::scopeGetString( jlong id , const char * field ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        jstring s = (jstring)_getEnv()->CallStaticObjectMethod( _dbhook , _scopeGetString , id , s1 );
        _getEnv()->DeleteLocalRef( s1 );

        if ( ! s )
            return "";

        const char * c = _getEnv()->GetStringUTFChars( s , 0 );
        string retStr(c);
        _getEnv()->ReleaseStringUTFChars( s , c );
        return retStr;
    }

    BSONObj JavaJSImpl::scopeGetObject( jlong id , const char * field ) {
        jstring s1 = _getEnv()->NewStringUTF( field );
        int guess = _getEnv()->CallStaticIntMethod( _dbhook , _scopeGuessObjectSize , id , _getEnv()->NewStringUTF( field ) );
        _getEnv()->DeleteLocalRef( s1 );

        if ( guess == 0 )
            return BSONObj();

        BSONObj::Holder* holder = (BSONObj::Holder*) malloc(guess + sizeof(unsigned));
        holder->zero()

        jobject bb = _getEnv()->NewDirectByteBuffer( (void*)holder->data , guess );
        jassert( bb );

        int len = _getEnv()->CallStaticIntMethod( _dbhook , _scopeGetObject , id , _getEnv()->NewStringUTF( field ) , bb );
        _getEnv()->DeleteLocalRef( bb );
        jassert( len > 0 && len < guess );

        BSONObj obj(holder);
        assert( obj.objsize() <= guess );
        return obj;
    }

// other

    jlong JavaJSImpl::functionCreate( const char * code ) {
        jstring s = _getEnv()->NewStringUTF( code );
        jassert( s );
        jlong id = _getEnv()->CallStaticLongMethod( _dbhook , _functionCreate , s );
        _getEnv()->DeleteLocalRef( s );
        return id;
    }

    int JavaJSImpl::invoke( jlong scope , jlong function ) {
        return _getEnv()->CallStaticIntMethod( _dbhook , _invoke , scope , function );
    }

// --- fun run method

    void JavaJSImpl::run( const char * js ) {
        jclass c = findClass( "ed/js/JS" );
        jassert( c );

        jmethodID m = _getEnv()->GetStaticMethodID( c , "eval" , "(Ljava/lang/String;)Ljava/lang/Object;" );
        jassert( m );

        jstring s = _getEnv()->NewStringUTF( js );
        log() << _getEnv()->CallStaticObjectMethod( c , m , s ) << endl;
        _getEnv()->DeleteLocalRef( s );
    }

    void JavaJSImpl::printException() {
        jthrowable exc = _getEnv()->ExceptionOccurred();
        if ( exc ) {
            _getEnv()->ExceptionDescribe();
            _getEnv()->ExceptionClear();
        }

    }

    JNIEnv * JavaJSImpl::_getEnv() {
        JNIEnv * env = _envs->get();
        if ( env )
            return env;

        int res = _jvm->AttachCurrentThread( (void**)&env , (void*)&_vmArgs );
        if ( res ) {
            out() << "ERROR javajs attachcurrentthread fails res:" << res << '\n';
            assert(false);
        }

        _envs->reset( env );
        return env;
    }

    Scope * JavaJSImpl::createScope() {
        return new JavaScope();
    }

    void ScriptEngine::setup() {
        if ( ! JavaJS ) {
            JavaJS = new JavaJSImpl();
            globalScriptEngine = JavaJS;
        }
    }

    void jasserted(const char *msg, const char *file, unsigned line) {
        log() << "jassert failed " << msg << " " << file << " " << line << endl;
        if ( JavaJS ) JavaJS->printException();
        throw AssertionException();
    }


    const char* findEd(const char *path) {

#if defined(_WIN32)

        if (!path) {
            path = findEd();
        }

        // @TODO check validity

        return path;
#else

        if (!path) {
            return findEd();
        }

        log() << "Appserver location specified : " << path << endl;

        if (!path) {
            log() << "   invalid appserver location : " << path << " : terminating - prepare for bus error" << endl;
            return 0;
        }

        DIR *testDir = opendir(path);

        if (testDir) {
            log(1) << "   found directory for appserver : " << path << endl;
            closedir(testDir);
            return path;
        }
        else {
            log() << "   ERROR : not a directory for specified appserver location : " << path << " - prepare for bus error" << endl;
            return null;
        }
#endif
    }

    const char * findEd() {

#if defined(_WIN32)
        log() << "Appserver location will be WIN32 default : c:/l/ed/" << endl;
        return "c:/l/ed";
#else

        static list<const char*> possibleEdDirs;
        if ( ! possibleEdDirs.size() ) {
            possibleEdDirs.push_back( "../../ed/ed/" ); // this one for dwight dev box
            possibleEdDirs.push_back( "../ed/" );
            possibleEdDirs.push_back( "../../ed/" );
            possibleEdDirs.push_back( "../babble/" );
            possibleEdDirs.push_back( "../../babble/" );
        }

        for ( list<const char*>::iterator i = possibleEdDirs.begin() ; i != possibleEdDirs.end(); i++ ) {
            const char * temp = *i;
            DIR * test = opendir( temp );
            if ( ! test )
                continue;

            closedir( test );
            log(1) << "found directory for appserver : " << temp << endl;
            return temp;
        }

        return 0;
#endif
    };

    const string findJars() {

        static list<string> possible;
        if ( ! possible.size() ) {
            possible.push_back( "./" );
            possible.push_back( "../" );

            log(2) << "dbExecCommand: " << dbExecCommand << endl;

            string dbDir = dbExecCommand;
#ifdef WIN32
            if ( dbDir.find( "\\" ) != string::npos ) {
                dbDir = dbDir.substr( 0 , dbDir.find_last_of( "\\" ) );
            }
            else {
                dbDir = ".";
            }
#else
            if ( dbDir.find( "/" ) != string::npos ) {
                dbDir = dbDir.substr( 0 , dbDir.find_last_of( "/" ) );
            }
            else {
                bool found = false;

                if ( getenv( "PATH" ) ) {
                    string s = getenv( "PATH" );
                    s += ":";
                    pcrecpp::StringPiece input( s );
                    string dir;
                    pcrecpp::RE re("(.*?):");
                    while ( re.Consume( &input, &dir ) ) {
                        string test = dir + "/" + dbExecCommand;
                        if ( boost::filesystem::exists( test ) ) {
                            while ( boost::filesystem::symbolic_link_exists( test ) ) {
                                char tmp[2048];
                                int len = readlink( test.c_str() , tmp , 2048 );
                                tmp[len] = 0;
                                log(5) << " symlink " << test << "  -->> " << tmp << endl;
                                test = tmp;

                                dir = test.substr( 0 , test.rfind( "/" ) );
                            }
                            dbDir = dir;
                            found = true;
                            break;
                        }
                    }
                }

                if ( ! found )
                    dbDir = ".";
            }
#endif

            log(2) << "dbDir [" << dbDir << "]" << endl;
            possible.push_back( ( dbDir + "/../lib/mongo/" ));
            possible.push_back( ( dbDir + "/../lib64/mongo/" ));
            possible.push_back( ( dbDir + "/../lib32/mongo/" ));
            possible.push_back( ( dbDir + "/" ));
            possible.push_back( ( dbDir + "/lib64/mongo/" ));
            possible.push_back( ( dbDir + "/lib32/mongo/" ));
        }

        for ( list<string>::iterator i = possible.begin() ; i != possible.end(); i++ ) {
            const string temp = *i;
            const string jarDir = ((string)temp) + "jars/";

            log(5) << "possible jarDir [" << jarDir << "]" << endl;

            path p(jarDir );
            if ( ! boost::filesystem::exists( p) )
                continue;

            log(1) << "found directory for jars : " << jarDir << endl;
            return temp;
        }

        problem() << "ERROR : can't find directory for jars - terminating" << endl;
        exit(44);
        return 0;

    };


// ---

    JNIEXPORT void JNICALL java_native_say(JNIEnv * env , jclass, jobject outBuffer ) {
        JNI_DEBUG( "native say called!" );

        Message out( env->GetDirectBufferAddress( outBuffer ) , false );
        Message in;

        jniCallback( out , in );
        assert( ! out.doIFreeIt() );
        curNs = 0;
    }

    JNIEXPORT jint JNICALL java_native_call(JNIEnv * env , jclass, jobject outBuffer , jobject inBuffer ) {
        JNI_DEBUG( "native call called!" );

        Message out( env->GetDirectBufferAddress( outBuffer ) , false );
        Message in;

        jniCallback( out , in );
        curNs = 0;

        JNI_DEBUG( "in.data : " << in.data );
        if ( in.data && in.data->len > 0 ) {
            JNI_DEBUG( "copying data of len :" << in.data->len );
            assert( env->GetDirectBufferCapacity( inBuffer ) >= in.data->len );
            memcpy( env->GetDirectBufferAddress( inBuffer ) , in.data , in.data->len );

            assert( ! out.doIFreeIt() );
            assert( in.doIFreeIt() );
            return in.data->len;
        }

        return 0;
    }

// ----

    void JavaJSImpl::runTest() {

        const int debug = 0;

        JavaJSImpl& JavaJS = *mongo::JavaJS;

        jlong scope = JavaJS.scopeCreate();
        jassert( scope );
        if ( debug ) out() << "got scope" << endl;


        jlong func1 = JavaJS.functionCreate( "foo = 5.6; bar = \"eliot\"; abc = { foo : 517 }; " );
        jassert( ! JavaJS.invoke( scope , func1 ) );


        if ( debug ) out() << "func3 start" << endl;
        jlong func3 = JavaJS.functionCreate( "function(){ z = true; } " );
        jassert( func3 );
        jassert( ! JavaJS.invoke( scope , func3 ) );
        jassert( JavaJS.scopeGetBoolean( scope , "z" ) );
        if ( debug ) out() << "func3 done" << endl;

        if ( debug ) out() << "going to get object" << endl;
        BSONObj obj = JavaJS.scopeGetObject( scope , "abc" );
        if ( debug ) out() << "done getting object" << endl;

        if ( debug ) {
            out() << "obj : " << obj.toString() << endl;
        }

        {
            time_t start = time(0);
            for ( int i=0; i<5000; i++ ) {
                JavaJS.scopeSetObject( scope , "obj" , &obj );
            }
            time_t end = time(0);

            if ( debug )
                out() << "time : " << (unsigned) ( end - start ) << endl;
        }

        if ( debug ) out() << "func4 start" << endl;
        JavaJS.scopeSetObject( scope , "obj" , &obj );
        if ( debug ) out() << "\t here 1" << endl;
        jlong func4 = JavaJS.functionCreate( "tojson( obj );" );
        if ( debug ) out() << "\t here 2" << endl;
        jassert( ! JavaJS.invoke( scope , func4 ) );
        if ( debug ) out() << "func4 end" << endl;

        if ( debug ) out() << "func5 start" << endl;
        jassert( JavaJS.scopeSetObject( scope , "c" , &obj ) );
        jlong func5 = JavaJS.functionCreate( "assert.eq( 517 , c.foo );" );
        jassert( func5 );
        jassert( ! JavaJS.invoke( scope , func5 ) );
        if ( debug ) out() << "func5 done" << endl;

        if ( debug ) out() << "func6 start" << endl;
        for ( int i=0; i<100; i++ ) {
            double val = i + 5;
            JavaJS.scopeSetNumber( scope , "zzz" , val );
            jlong func6 = JavaJS.functionCreate( " xxx = zzz; " );
            jassert( ! JavaJS.invoke( scope , func6 ) );
            double n = JavaJS.scopeGetNumber( scope , "xxx" );
            jassert( val == n );
        }
        if ( debug ) out() << "func6 done" << endl;

        jlong func7 = JavaJS.functionCreate( "return 11;" );
        jassert( ! JavaJS.invoke( scope , func7 ) );
        assert( 11 == JavaJS.scopeGetNumber( scope , "return" ) );

        scope = JavaJS.scopeCreate();
        jlong func8 = JavaJS.functionCreate( "function(){ return 12; }" );
        jassert( ! JavaJS.invoke( scope , func8 ) );
        assert( 12 == JavaJS.scopeGetNumber( scope , "return" ) );

    }

#endif

} // namespace mongo
