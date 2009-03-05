// ShellUtils.cpp

#include "ShellUtils.h"
#include <boost/smart_ptr.hpp>
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

void sleepms( int ms ) {
    boost::xtime xt;
    boost::xtime_get(&xt, boost::TIME_UTC);
    xt.sec += ( ms / 1000 );
    xt.nsec += ( ms % 1000 ) * 1000000;
    if ( xt.nsec >= 1000000000 ) {
        xt.nsec -= 1000000000;
        xt.sec++;
    }
    boost::thread::sleep(xt);    
}

Handle<v8::Value> JSSleep(const Arguments& args){
    assert( args.Length() == 1 );
    assert( args[0]->IsInt32() );
    int ms = args[ 0 ]->ToInt32()->Value();
    {
        v8::Unlocker u;
        sleepms( ms );
    }
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

extern v8::Handle< v8::Context > baseContext_;

class JSThreadConfig {
public:
    JSThreadConfig( const Arguments &args ) : started_(), done_() {
        jsassert( args.Length() > 0, "need at least one argument" );
        jsassert( args[ 0 ]->IsFunction(), "first argument must be a function" );
        Local< Function > f = Function::Cast( *args[ 0 ] );
        f_ = Persistent< Function >::New( f );
        for( int i = 1; i < args.Length(); ++i )
            args_.push_back( Persistent< Value >::New( args[ i ] ) );
    }
    ~JSThreadConfig() {
        f_.Dispose();
        for( vector< Persistent< Value > >::iterator i = args_.begin(); i != args_.end(); ++i )
            i->Dispose();
        returnData_.Dispose();
    }
    void start() {
        jsassert( !started_, "Thread already started" );
        JSThread jt( *this );
        thread_.reset( new boost::thread( jt ) );
        started_ = true;
    }
    void join() {
        jsassert( started_ && !done_, "Thread not running" );
        Unlocker u;
        thread_->join();
        done_ = true;
    }
    Local< Value > returnData() {
        if ( !done_ )
            join();
        return Local< Value >::New( returnData_ );
    }
private:
    class JSThread {
    public:
        JSThread( JSThreadConfig &config ) : config_( config ) {}
        void operator()() {
            Locker l;
            Context::Scope context_scope( baseContext_ );
            HandleScope handle_scope;
            boost::scoped_array< Persistent< Value > > argv( new Persistent< Value >[ config_.args_.size() ] );
            for( unsigned int i = 0; i < config_.args_.size(); ++i )
                argv[ i ] = Persistent< Value >::New( config_.args_[ i ] );
            Local< Value > ret = config_.f_->Call( Context::GetCurrent()->Global(), config_.args_.size(), argv.get() );
            for( unsigned int i = 0; i < config_.args_.size(); ++i )
                argv[ i ].Dispose();
            config_.returnData_ = Persistent< Value >::New( ret );
        }
    private:
        JSThreadConfig &config_;
    };

    bool started_;
    bool done_;
    Persistent< Function > f_;
    vector< Persistent< Value > > args_;
    auto_ptr< boost::thread > thread_;
    Persistent< Value > returnData_;
};

Handle< Value > ThreadInit( const Arguments &args ) {
    Handle<Object> it = args.This();
    // NOTE I believe the passed JSThreadConfig will never be freed.  If this
    // policy is changed, JSThread may no longer be able to store JSThreadConfig
    // by reference.
    it->Set( String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( args ) ) );
    return Undefined();
}

JSThreadConfig *thisConfig( const Arguments &args ) {
    Local< External > c = External::Cast( *(args.This()->Get( String::New( "_JSThreadConfig" ) ) ) );
    JSThreadConfig *config = (JSThreadConfig *)( c->Value() );
    return config;
}

Handle< Value > ThreadStart( const Arguments &args ) {
    thisConfig( args )->start();
    return Undefined();
}

Handle< Value > ThreadJoin( const Arguments &args ) {
    thisConfig( args )->join();
    return Undefined();
}

Handle< Value > ThreadReturnData( const Arguments &args ) {
    return thisConfig( args )->returnData();
}

const char *argv0 = 0;
void RecordMyLocation( const char *_argv0 ) { argv0 = _argv0; }

#if !defined(_WIN32)
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

map< int, pair< pid_t, int > > dbs;

char *copyString( const char *original ) {
    char *ret = reinterpret_cast< char * >( malloc( strlen( original ) + 1 ) );
    strcpy( ret, original );
    return ret;
}

boost::mutex &mongoProgramOutputMutex( *( new boost::mutex ) );

void writeMongoProgramOutputLine( int port, const char *line ) {
    boost::mutex::scoped_lock lk( mongoProgramOutputMutex );
    cout << "m" << port << "| " << line << endl;
}

class MongoProgramRunner {
    char **argv_;
    int port_;
public:
    MongoProgramRunner( const v8::Arguments &args ) {
        assert( args.Length() > 0 );
        v8::String::Utf8Value programParse( args[ 0 ] );
        assert( *programParse );
        string program( *programParse );
        
        assert( argv0 );
        boost::filesystem::path programPath = ( boost::filesystem::path( argv0 ) ).branch_path() / program;
        assert( boost::filesystem::exists( programPath ) );
        
        port_ = -1;
        argv_ = new char *[ args.Length() + 1 ];
        {
            string s = programPath.native_file_string();
            if ( s == program )
                s = "./" + s;
            argv_[ 0 ] = copyString( s.c_str() );
        }
        
        for( int i = 1; i < args.Length(); ++i ) {
            v8::String::Utf8Value str( args[ i ] );
            assert( *str );
            char *s = copyString( *str );
            if ( string( "--port" ) == s )
                port_ = -2;
            else if ( port_ == -2 )
                port_ = strtol( s, 0, 10 );
            argv_[ i ] = s;
        }
        argv_[ args.Length() ] = 0;
        
        assert( port_ > 0 );
        assert( dbs.count( port_ ) == 0 );        
    }
    
    void operator()() {
        int pipeEnds[ 2 ];
        assert( pipe( pipeEnds ) != -1 );
        
        fflush( 0 );
        pid_t pid = fork();
        assert( pid != -1 );
        
        if ( pid == 0 ) {
            assert( dup2( pipeEnds[ 1 ], STDOUT_FILENO ) != -1 );
            assert( dup2( pipeEnds[ 1 ], STDERR_FILENO ) != -1 );
            execvp( argv_[ 0 ], argv_ );
            assert( ( "Unable to start program", false ) );
        }
        
        int i = 0;
        while( argv_[ i ] )
            free( argv_[ i++ ] );
        free( argv_ );
        
        dbs.insert( make_pair( port_, make_pair( pid, pipeEnds[ 1 ] ) ) );
        
        // This assumes there aren't any 0's in the mongo program output.
        // Hope that's ok.
        char buf[ 1024 ];
        char temp[ 1024 ];
        char *start = buf;
        while( 1 ) {
            int lenToRead = 1023 - ( start - buf );
            int ret = read( pipeEnds[ 0 ], (void *)start, lenToRead );
            assert( ret != -1 );
            start[ ret ] = '\0';
            char *last = buf;
            for( char *i = strchr( buf, '\n' ); i; last = i + 1, i = strchr( last, '\n' ) ) {
                *i = '\0';
                writeMongoProgramOutputLine( port_, last );
            }
            if ( ret == 0 ) {
                if ( *last )
                    writeMongoProgramOutputLine( port_, last );
                break;
            }
            if ( last != buf ) {
                strcpy( temp, last );
                strcpy( buf, temp );
            } else {
                assert( strlen( buf ) < 1023 );
            }
            start = buf + strlen( buf );
        }        
    }
};

v8::Handle< v8::Value > StartMongoProgram( const v8::Arguments &a ) {
    MongoProgramRunner r( a );
    boost::thread t( r );
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

    pid_t pid = dbs[ port ].first;
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

    close( dbs[ port ].second );
    dbs.erase( port );
}

v8::Handle< v8::Value > StopMongoProgram( const v8::Arguments &a ) {
    assert( a.Length() == 1 );
    assert( a[ 0 ]->IsInt32() );
    int port = a[ 0 ]->ToInt32()->Value();
    killDb( port );
    return v8::Undefined();
}

void KillMongoProgramInstances() {
    for( map< int, pair< pid_t, int > >::iterator i = dbs.begin(); i != dbs.end(); ++i )
        killDb( i->first );    
}

MongoProgramScope::~MongoProgramScope() {
    try {
        KillMongoProgramInstances();
    } catch ( ... ) {
        assert( false );
    }
}

#else
MongoProgramScope::~MongoProgramScope() {}
void KillMongoProgramInstances() {}
#endif

Handle< Value > ThreadInject( const Arguments &args ) {
    jsassert( args.Length() == 1 , "threadInject takes exactly 1 argument" );
    jsassert( args[0]->IsObject() , "threadInject needs to be passed a prototype" );
    
    Local<v8::Object> o = args[0]->ToObject();
    
    o->Set( String::New( "init" ) , FunctionTemplate::New( ThreadInit )->GetFunction() );
    o->Set( String::New( "start" ) , FunctionTemplate::New( ThreadStart )->GetFunction() );
    o->Set( String::New( "join" ) , FunctionTemplate::New( ThreadJoin )->GetFunction() );
    o->Set( String::New( "returnData" ) , FunctionTemplate::New( ThreadReturnData )->GetFunction() );
    
    return v8::Undefined();    
}

void installShellUtils( Handle<v8::ObjectTemplate>& global ){
    global->Set(v8::String::New("sleep"), v8::FunctionTemplate::New(JSSleep));
    global->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print));
    global->Set(v8::String::New("load"), v8::FunctionTemplate::New(Load));
    global->Set(v8::String::New("listFiles"), v8::FunctionTemplate::New(ListFiles));
    global->Set(v8::String::New("quit"), v8::FunctionTemplate::New(Quit));
    global->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version));
    global->Set(v8::String::New("threadInject"), v8::FunctionTemplate::New(ThreadInject));
#if !defined(_WIN32)
    global->Set(v8::String::New("_startMongoProgram"), v8::FunctionTemplate::New(StartMongoProgram));
    global->Set(v8::String::New("stopMongod"), v8::FunctionTemplate::New(StopMongoProgram));
    global->Set(v8::String::New("stopMongoProgram"), v8::FunctionTemplate::New(StopMongoProgram));
    global->Set(v8::String::New("resetDbpath"), v8::FunctionTemplate::New(ResetDbpath));
#endif
}
