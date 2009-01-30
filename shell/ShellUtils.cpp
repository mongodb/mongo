// ShellUtils.cpp

#include "ShellUtils.h"
#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/filesystem/operations.hpp>
#include <iostream>
#include <map>
#include <sstream>

using namespace std;
using namespace v8;
using namespace boost::filesystem;

Handle<v8::Value> Print(const Arguments& args) {
    bool first = true;
    for (int i = 0; i < args.Length(); i++) {
        HandleScope handle_scope;
        if (first) {
            first = false;
        } else {
            printf(" ");
        }
        v8::String::Utf8Value str(args[i]);
        printf("%s", *str);
    }
    printf("\n");
    return v8::Undefined();
}

std::string toSTLString( const Handle<v8::Value> & o ){
    v8::String::Utf8Value str(o);    
    const char * foo = *str;
    std::string s(foo);
    return s;
}

std::ostream& operator<<( std::ostream &s, const Handle<v8::Value> & o ){
    v8::String::Utf8Value str(o);    
    s << *str;
    return s;
}

std::ostream& operator<<( std::ostream &s, const v8::TryCatch * try_catch ){
    HandleScope handle_scope;
    v8::String::Utf8Value exception(try_catch->Exception());
    Handle<v8::Message> message = try_catch->Message();
    
    if (message.IsEmpty()) {
        s << *exception << endl;
    } 
    else {

        v8::String::Utf8Value filename(message->GetScriptResourceName());
        int linenum = message->GetLineNumber();
        cout << *filename << ":" << linenum << " " << *exception << endl;

        v8::String::Utf8Value sourceline(message->GetSourceLine());
        cout << *sourceline << endl;

        int start = message->GetStartColumn();
        for (int i = 0; i < start; i++)
            cout << " ";

        int end = message->GetEndColumn();
        for (int i = start; i < end; i++)
            cout << "^";

        cout << endl;
    }    

    if ( try_catch->next_ )
        s << try_catch->next_;

    return s;
}

Handle<v8::Value> Load(const Arguments& args) {
    for (int i = 0; i < args.Length(); i++) {
        HandleScope handle_scope;
        v8::String::Utf8Value file(args[i]);
        Handle<v8::String> source = ReadFile(*file);
        if (source.IsEmpty()) {
            return v8::ThrowException(v8::String::New("Error loading file"));
        }
        if (!ExecuteString(source, v8::String::New(*file), false, false)) {
            return v8::ThrowException(v8::String::New("Error executing  file"));
        }
    }
    return v8::Undefined();
}


Handle<v8::Value> Quit(const Arguments& args) {
    // If not arguments are given args[0] will yield undefined which
    // converts to the integer value 0.
    int exit_code = args[0]->Int32Value();
    exit(exit_code);
    return v8::Undefined();
}


Handle<v8::Value> Version(const Arguments& args) {
    return v8::String::New(v8::V8::GetVersion());
}

Handle<v8::String> ReadFile(const char* name) {

    path p(name);
    if ( is_directory( p ) ){
        cerr << "can't read directory [" << name << "]" << endl;
        return v8::String::New( "" );
    }
                    
    FILE* file = fopen(name, "rb");
    if (file == NULL) return Handle<v8::String>();

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    rewind(file);

    char* chars = new char[size + 1];
    chars[size] = '\0';
    for (int i = 0; i < size;) {
        int read = fread(&chars[i], 1, size - i, file);
        i += read;
    }
    fclose(file);
    Handle<v8::String> result = v8::String::New(chars, size);
    delete[] chars;
    return result;
}


bool ExecuteString(Handle<v8::String> source, Handle<v8::Value> name,
                   bool print_result, bool report_exceptions ){

    HandleScope handle_scope;
    v8::TryCatch try_catch;
    
    Handle<v8::Script> script = v8::Script::Compile(source, name);
    if (script.IsEmpty()) {
        if (report_exceptions)
            ReportException(&try_catch);
        return false;
    } 
    
    Handle<v8::Value> result = script->Run();
    if ( result.IsEmpty() ){
        if (report_exceptions)
            ReportException(&try_catch);
        return false;
    } 
    
    if ( print_result ){
        
        Local<Context> current = Context::GetCurrent();
        Local<Object> global = current->Global();
        
        Local<Value> shellPrint = global->Get( String::New( "shellPrint" ) );

        if ( shellPrint->IsFunction() ){
            v8::Function * f = (v8::Function*)(*shellPrint);
            Handle<v8::Value> argv[1];
            argv[0] = result;
            f->Call( global , 1 , argv );
        }
        else if ( ! result->IsUndefined() ){
            cout << result << endl;
        }
    }
    
    return true;
}

Handle<v8::Value> JSSleep(const Arguments& args){
    assert( args.Length() == 1 );
    assert( args[0]->IsInt32() );
    

    boost::xtime xt;
    boost::xtime_get(&xt, boost::TIME_UTC);
    int sleepMillisecs = args[0]->ToInt32()->Value();
    xt.sec += ( sleepMillisecs / 1000 );
    xt.nsec += ( sleepMillisecs % 1000 ) * 1000000;
    if ( xt.nsec >= 1000000000 ) {
        xt.nsec -= 1000000000;
        xt.sec++;
    }
    boost::thread::sleep(xt);
    
    return v8::Undefined();
}

Handle<v8::Value> ListFiles(const Arguments& args){
    jsassert( args.Length() == 1 , "need to specify 1 argument to listFiles" );
    
    Handle<v8::Array> lst = v8::Array::New();
    
    path root( toSTLString( args[0] ) );
    
    directory_iterator end;
    directory_iterator i( root);
    
    int num =0;
    while ( i != end ){
        path p = *i;
        
        Handle<v8::Object> o = v8::Object::New();
        o->Set( v8::String::New( "name" ) , v8::String::New( p.string().c_str() ) );
        o->Set( v8::String::New( "isDirectory" ) , v8::Boolean::New( is_directory( p ) ) );

        lst->Set( v8::Number::New( num ) , o );

        num++;
        i++;
    }
    
    return lst;
}

void ReportException(v8::TryCatch* try_catch) {
    cout << try_catch << endl;
}

const char *argv0 = 0;
void RecordMyLocation( const char *_argv0 ) { argv0 = _argv0; }

#if !defined(_WIN32)
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

map< int, pid_t > dbs;

char *copyString( const char *original ) {
    char *ret = reinterpret_cast< char * >( malloc( strlen( original ) + 1 ) );
    strcpy( ret, original );
    return ret;
}

v8::Handle< v8::Value > StartMongod( const v8::Arguments &a ) {
    assert( argv0 );
    boost::filesystem::path mongod = ( boost::filesystem::path( argv0 ) ).branch_path() / "mongod";
    assert( boost::filesystem::exists( mongod ) );
    
    int port = -1;
    char * argv[ a.Length() + 2 ];
    argv[ 0 ] = copyString( mongod.native_file_string().c_str() );
    for( int i = 0; i < a.Length(); ++i ) {
        v8::String::Utf8Value str( a[ i ] );
        char *s = copyString( *str );
        if ( string( "--port" ) == s )
            port = -2;
        else if ( port == -2 )
            port = strtol( s, 0, 10 );
        argv[ 1 + i ] = s;
    }
    argv[ a.Length() + 1 ] = 0;
    
    assert( port > 0 );
    assert( dbs.count( port ) == 0 );

    fflush( 0 );
    pid_t pid = fork();
    assert( pid != -1 );
    
    if ( pid == 0 ) {
        stringstream ss;
        ss << "mongod." << port << ".log";
        string name = ss.str();
        int d = open( name.c_str(), O_WRONLY | O_APPEND | O_CREAT );
        assert( d != -1 );
        assert( fchmod( d, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP ) == 0 );
        const char *bar = "\n\n--- NEW INSTANCE ---\n\n";
        write( d, bar, strlen( bar ) );
        assert( dup2( d, STDOUT_FILENO ) != -1 );
        assert( dup2( d, STDERR_FILENO ) != -1 );
        execvp( argv[ 0 ], argv );
        assert( false );
    }

    int i = 0;
    while( argv[ i ] )
        free( argv[ i++ ] );
    
    dbs.insert( make_pair( port, pid ) );
    return v8::Undefined();
}

v8::Handle< v8::Value > ResetDbpath( const v8::Arguments &a ) {
    assert( a.Length() == 1 );
    v8::String::Utf8Value path( a[ 0 ] );
    if ( boost::filesystem::exists( *path ) )
        boost::filesystem::remove_all( *path );
    boost::filesystem::create_directory( *path );    
    return v8::Undefined();
}

void killDb( int port ) {
    if( dbs.count( port ) != 1 ) {
        cout << "No db started on port: " << port << endl;
        return;
    }

    pid_t pid = dbs[ port ];
    kill( pid, SIGTERM );

    boost::xtime xt;
    boost::xtime_get(&xt, boost::TIME_UTC);
    ++xt.sec;
    int i = 0;
    for( ; i < 5; ++i, ++xt.sec ) {
        int temp;
        if( waitpid( pid, &temp, WNOHANG ) != 0 )
            break;
        boost::thread::sleep( xt );
    }
    if ( i == 5 )
        kill( pid, SIGKILL );
    
    dbs.erase( port );
}

v8::Handle< v8::Value > StopMongod( const v8::Arguments &a ) {
    assert( a.Length() == 1 );
    assert( a[ 0 ]->IsInt32() );
    int port = a[ 0 ]->ToInt32()->Value();
    killDb( port );
    return v8::Undefined();
}

void KillMongodbInstances() {
    for( map< int, pid_t >::iterator i = dbs.begin(); i != dbs.end(); ++i )
        killDb( i->first );    
}

MongodScope::~MongodScope() {
    try {
        KillMongodbInstances();
    } catch ( ... ) {
        assert( false );
    }
}

#else
MongodScope::~MongodScope() {}
#endif

void installShellUtils( Handle<v8::ObjectTemplate>& global ){
    global->Set(v8::String::New("sleep"), v8::FunctionTemplate::New(JSSleep));
    global->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print));
    global->Set(v8::String::New("load"), v8::FunctionTemplate::New(Load));
    global->Set(v8::String::New("listFiles"), v8::FunctionTemplate::New(ListFiles));
    global->Set(v8::String::New("quit"), v8::FunctionTemplate::New(Quit));
    global->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version));
#if !defined(_WIN32)
    global->Set(v8::String::New("_startMongod"), v8::FunctionTemplate::New(StartMongod));
    global->Set(v8::String::New("stopMongod"), v8::FunctionTemplate::New(StopMongod));
    global->Set(v8::String::New("resetDbpath"), v8::FunctionTemplate::New(ResetDbpath));
#endif
}
