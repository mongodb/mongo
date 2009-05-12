// ShellUtils.cpp

#include "ShellUtils.h"
#include "../db/jsobj.h"
#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/filesystem/operations.hpp>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

using namespace std;
using namespace v8;
using namespace boost::filesystem;
using namespace mongo;

BSONObj makeUndefined() {
    BSONObjBuilder b;
    b.appendUndefined( "" );
    return b.obj();
}
BSONObj undefined_ = makeUndefined();

BSONObj encapsulate( const BSONObj &obj ) {
    return BSON( "" << obj );
}

BSONObj Print(const BSONObj &args) {
    bool first = true;
    BSONObjIterator i( args );
    while( i.more() ) {
        BSONElement e = i.next();
        if ( e.eoo() )
            break;
        if (first) {
            first = false;
        } else {
            printf(" ");
        }
        string str( e.jsonString( TenGen, false ) );
        printf("%s", str.c_str());        
    }
    printf("\n");
    return undefined_;
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

BSONObj Load(const BSONObj& args) {
    BSONObjIterator i( args );
    while( i.more() ) {
        BSONElement e = i.next();
        if ( e.eoo() )
            break;
        assert( e.type() == mongo::String );
        Handle<v8::String> source = ReadFile(e.valuestr());
        massert( "error loading file", !source.IsEmpty() );
        massert( "error executing file", ExecuteString(source, v8::String::New(e.valuestr()), false, true));
    }
    return undefined_;
}


BSONObj Quit(const BSONObj& args) {
    // If not arguments are given first element will be EOO, which
    // converts to the integer value 0.
    int exit_code = int( args.firstElement().number() );
    ::exit(exit_code);
    return undefined_;
}


BSONObj Version(const BSONObj& args) {
    return BSON( "" << v8::V8::GetVersion() );
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
        Local<v8::Object> global = current->Global();
        
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

mongo::BSONObj JSSleep(const mongo::BSONObj &args){
    assert( args.nFields() == 1 );
    assert( args.firstElement().isNumber() );
    int ms = int( args.firstElement().number() );
    {
        v8::Unlocker u;
        sleepms( ms );
    }
    return undefined_;
}

BSONObj ListFiles(const BSONObj& args){
    jsassert( args.nFields() == 1 , "need to specify 1 argument to listFiles" );
    
    BSONObjBuilder lst;
    
    path root( args.firstElement().valuestrsafe() );
    
    directory_iterator end;
    directory_iterator i( root);
    
    int num =0;
    while ( i != end ){
        path p = *i;
        
        BSONObjBuilder b;
        b << "name" << p.string();
        b.appendBool( "isDirectory", is_directory( p ) );
        stringstream ss;
        ss << num;
        string name = ss.str();
        lst.append( name.c_str(), b.done() );
        
        num++;
        i++;
    }
    
    BSONObjBuilder ret;
    ret.appendArray( "", lst.done() );
    return ret.obj();
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
        Local< v8::Function > f = v8::Function::Cast( *args[ 0 ] );
        f_ = Persistent< v8::Function >::New( f );
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
            // Context scope and handle scope held in thread specific storage,
            // so need to configure for each thread.
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
    Persistent< v8::Function > f_;
    vector< Persistent< Value > > args_;
    auto_ptr< boost::thread > thread_;
    Persistent< Value > returnData_;
};

Handle< Value > ThreadInit( const Arguments &args ) {
    Handle<v8::Object> it = args.This();
    // NOTE I believe the passed JSThreadConfig will never be freed.  If this
    // policy is changed, JSThread may no longer be able to store JSThreadConfig
    // by reference.
    it->Set( String::New( "_JSThreadConfig" ), External::New( new JSThreadConfig( args ) ) );
    return v8::Undefined();
}

JSThreadConfig *thisConfig( const Arguments &args ) {
    Local< External > c = External::Cast( *(args.This()->Get( String::New( "_JSThreadConfig" ) ) ) );
    JSThreadConfig *config = (JSThreadConfig *)( c->Value() );
    return config;
}

Handle< Value > ThreadStart( const Arguments &args ) {
    thisConfig( args )->start();
    return v8::Undefined();
}

Handle< Value > ThreadJoin( const Arguments &args ) {
    thisConfig( args )->join();
    return v8::Undefined();
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
stringstream mongoProgramOutput_;

void writeMongoProgramOutputLine( int port, const char *line ) {
    boost::mutex::scoped_lock lk( mongoProgramOutputMutex );
    stringstream buf;
    buf << "m" << port << "| " << line;
    cout << buf.str() << endl;
    mongoProgramOutput_ << buf.str() << endl;
}

BSONObj RawMongoProgramOutput( const BSONObj &args ) {
    boost::mutex::scoped_lock lk( mongoProgramOutputMutex );
    return BSON( "" << mongoProgramOutput_.str() );
}

class MongoProgramRunner {
    char **argv_;
    int port_;
    int pipe_;
public:
    MongoProgramRunner( const BSONObj &args ) {
        assert( args.nFields() > 0 );
        string program( args.firstElement().valuestrsafe() );
        
        assert( !program.empty() );
        boost::filesystem::path programPath = ( boost::filesystem::path( argv0 ) ).branch_path() / program;
        assert( boost::filesystem::exists( programPath ) );
        
        port_ = -1;
        argv_ = new char *[ args.nFields() + 1 ];
        {
            string s = programPath.native_file_string();
            if ( s == program )
                s = "./" + s;
            argv_[ 0 ] = copyString( s.c_str() );
        }

        BSONObjIterator j( args );
        j.next();
        for( int i = 1; i < args.nFields(); ++i ) {
            BSONElement e = j.next();
            string str;
            if ( e.isNumber() ) {
                stringstream ss;
                ss << e.number();
                str = ss.str();
            } else {
                assert( e.type() == mongo::String );
                str = e.valuestr();
            }
            char *s = copyString( str.c_str() );
            if ( string( "--port" ) == s )
                port_ = -2;
            else if ( port_ == -2 )
                port_ = strtol( s, 0, 10 );
            argv_[ i ] = s;
        }
        argv_[ args.nFields() ] = 0;
        
        assert( port_ > 0 );
        if ( dbs.count( port_ ) != 0 ){
            cerr << "count for port: " << port_ << " is not 0 is: " << dbs.count( port_ ) << endl;
            assert( dbs.count( port_ ) == 0 );        
        }
    }
    
    void start() {
        int pipeEnds[ 2 ];
        assert( pipe( pipeEnds ) != -1 );
        
        fflush( 0 );
        pid_t pid = fork();
        assert( pid != -1 );
        
        if ( pid == 0 ) {
            assert( dup2( pipeEnds[ 1 ], STDOUT_FILENO ) != -1 );
            assert( dup2( pipeEnds[ 1 ], STDERR_FILENO ) != -1 );
            execvp( argv_[ 0 ], argv_ );
            assert( "Unable to start program" == 0 );
        }
        
        cout << "shell: started mongo program";
        int i = 0;
        while( argv_[ i ] )
            cout << " " << argv_[ i++ ];
        cout << endl;
        
        i = 0;
        while( argv_[ i ] )
            free( argv_[ i++ ] );
        free( argv_ );
        
        dbs.insert( make_pair( port_, make_pair( pid, pipeEnds[ 1 ] ) ) );
        pipe_ = pipeEnds[ 0 ];
    }
    
    // Continue reading output
    void operator()() {
        // This assumes there aren't any 0's in the mongo program output.
        // Hope that's ok.
        char buf[ 1024 ];
        char temp[ 1024 ];
        char *start = buf;
        while( 1 ) {
            int lenToRead = 1023 - ( start - buf );
            int ret = read( pipe_, (void *)start, lenToRead );
            assert( ret != -1 );
            start[ ret ] = '\0';
            if ( strlen( start ) != unsigned( ret ) )
                writeMongoProgramOutputLine( port_, "WARNING: mongod wrote null bytes to output" );
            char *last = buf;
            for( char *i = strchr( buf, '\n' ); i; last = i + 1, i = strchr( last, '\n' ) ) {
                *i = '\0';
                writeMongoProgramOutputLine( port_, last );
            }
            if ( ret == 0 ) {
                if ( *last )
                    writeMongoProgramOutputLine( port_, last );
                close( pipe_ );
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

BSONObj StartMongoProgram( const BSONObj &a ) {
    MongoProgramRunner r( a );
    r.start();
    boost::thread t( r );
    return undefined_;
}

BSONObj ResetDbpath( const BSONObj &a ) {
    assert( a.nFields() == 1 );
    string path = a.firstElement().valuestrsafe();
    assert( !path.empty() );
    if ( boost::filesystem::exists( path ) )
        boost::filesystem::remove_all( path );
    boost::filesystem::create_directory( path );    
    return undefined_;
}

void killDb( int port, int signal ) {
    if( dbs.count( port ) != 1 ) {
        cout << "No db started on port: " << port << endl;
        return;
    }

    pid_t pid = dbs[ port ].first;
    assert( 0 == kill( pid, signal ) );

    int i = 0;
    for( ; i < 65; ++i ) {
        if ( i == 5 ) {
            char now[64];
            time_t_to_String(time(0), now);
            now[ 20 ] = 0;
            cout << now << " process on port " << port << ", with pid " << pid << " not terminated, sending sigkill" << endl;
            assert( 0 == kill( pid, SIGKILL ) );
        }        
        int temp;
        int ret = waitpid( pid, &temp, WNOHANG );
        if ( ret == pid )
            break;
        sleepms( 1000 );
    }
    if ( i == 65 ) {
        char now[64];
        time_t_to_String(time(0), now);
        now[ 20 ] = 0;
        cout << now << " failed to terminate process on port " << port << ", with pid " << pid << endl;
        assert( "Failed to terminate process" == 0 );
    }

    close( dbs[ port ].second );
    dbs.erase( port );
    if ( i > 4 || signal == SIGKILL ) {
        sleepms( 4000 ); // allow operating system to reclaim resources
    }
}

BSONObj StopMongoProgram( const BSONObj &a ) {
    assert( a.nFields() == 1 || a.nFields() == 2 );
    assert( a.firstElement().isNumber() );
    int port = int( a.firstElement().number() );
    int signal = SIGTERM;
    if ( a.nFields() == 2 ) {
        BSONObjIterator i( a );
        i.next();
        BSONElement e = i.next();
        assert( e.isNumber() );
        signal = int( e.number() );
    }
    killDb( port, signal );
    cout << "shell: stopped mongo program on port " << port << endl;
    return undefined_;
}

void KillMongoProgramInstances() {
    vector< int > ports;
    for( map< int, pair< pid_t, int > >::iterator i = dbs.begin(); i != dbs.end(); ++i )
        ports.push_back( i->first );
    for( vector< int >::iterator i = ports.begin(); i != ports.end(); ++i )
        killDb( *i, SIGTERM );
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

#ifndef _WIN32
BSONObj AllocatePorts( const BSONObj &args ) {
    jsassert( args.nFields() == 1 , "allocatePorts takes exactly 1 argument" );
    jsassert( args.firstElement().isNumber() , "allocatePorts needs to be passed an integer" );

    int n = int( args.firstElement().number() );

    vector< int > ports;
    for( int i = 0; i < n; ++i ) {
        int s = socket( AF_INET, SOCK_STREAM, 0 );
        assert( s );

        sockaddr_in address;
        memset(address.sin_zero, 0, sizeof(address.sin_zero));
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = 0;        
        assert( 0 == ::bind( s, (sockaddr*)&address, sizeof( address ) ) );
        
        sockaddr_in newAddress;
        socklen_t len = sizeof( newAddress );
        assert( 0 == getsockname( s, (sockaddr*)&newAddress, &len ) );
        ports.push_back( ntohs( newAddress.sin_port ) );
        
        assert( 0 == close( s ) );
    }

    sort( ports.begin(), ports.end() );
    BSONObjBuilder b;
    b.append( "", ports );
    return b.obj();
}
#endif

void installShellUtils( mongo::Scope &scope, v8::Handle<v8::ObjectTemplate>& global ) {
    scope.injectNative( "sleep", JSSleep );
    scope.injectNative( "print", Print );
    scope.injectNative( "load", Load );
    scope.injectNative( "listFiles", ListFiles );
    scope.injectNative( "quit", Quit );
    scope.injectNative( "version", Version );
    global->Set( String::New( "threadInject" ), FunctionTemplate::New( ThreadInject ) );
#if !defined(_WIN32)
    scope.injectNative( "allocatePorts", AllocatePorts );
    scope.injectNative( "_startMongoProgram", StartMongoProgram );
    scope.injectNative( "stopMongod", StopMongoProgram );
    scope.injectNative( "stopMongoProgram", StopMongoProgram );
    scope.injectNative( "resetDbpath", ResetDbpath );
    scope.injectNative( "rawMongoProgramOutput", RawMongoProgramOutput );
#endif
}
