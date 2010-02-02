// utils.cpp

#include <boost/thread/xtime.hpp>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <fcntl.h>

#ifdef _WIN32
# include <Windows.h>
# include <io.h>
# define SIGKILL 9
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <signal.h>
# include <sys/stat.h>
# include <sys/wait.h>
#endif

#include "../client/dbclient.h"
#include "../util/processinfo.h"
#include "utils.h"

extern const char * jsconcatcode_server;

namespace mongo {
#ifdef _WIN32
    inline int close(int fd) { return _close(fd); }
    inline int read(int fd, void* buf, size_t size) { return _read(fd, buf, size); }

    inline int pipe(int fds[2]) { return _pipe(fds, 1024, _O_TEXT | _O_NOINHERIT); }
#else
    inline void closesocket(int sock) { close(sock); }
#endif

    namespace shellUtils {
        
        std::string _dbConnect;
        std::string _dbAuth;
                
        const char *argv0 = 0;
        void RecordMyLocation( const char *_argv0 ) { argv0 = _argv0; }
        
        // helpers
        
        BSONObj makeUndefined() {
            BSONObjBuilder b;
            b.appendUndefined( "" );
            return b.obj();
        }
        const BSONObj undefined_ = makeUndefined();
        
        BSONObj encapsulate( const BSONObj &obj ) {
            return BSON( "" << obj );
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
        
        // real methods


        
        mongo::BSONObj JSSleep(const mongo::BSONObj &args){
            assert( args.nFields() == 1 );
            assert( args.firstElement().isNumber() );
            int ms = int( args.firstElement().number() );
            {
                auto_ptr< ScriptEngine::Unlocker > u = globalScriptEngine->newThreadUnlocker();
                sleepms( ms );
            }
            return undefined_;
        }

        BSONObj listFiles(const BSONObj& args){
            uassert( 10257 ,  "need to specify 1 argument to listFiles" , args.nFields() == 1 );
            
            BSONObjBuilder lst;
            
            string rootname = args.firstElement().valuestrsafe();
            path root( rootname );
            stringstream ss;
            ss << "listFiles: no such directory: " << rootname;
            string msg = ss.str();
            uassert( 12581, msg.c_str(), boost::filesystem::exists( root ) );
            
            directory_iterator end;
            directory_iterator i( root);
            
            int num =0;
            while ( i != end ){
                path p = *i;
                BSONObjBuilder b;
                b << "name" << p.string();
                b.appendBool( "isDirectory", is_directory( p ) );
                if ( ! is_directory( p ) ){
                    try { 
                        b.append( "size" , (double)file_size( p ) );
                    }
                    catch ( ... ){
                        i++;
                        continue;
                    }
                }

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
        
        BSONObj Quit(const BSONObj& args) {
            // If not arguments are given first element will be EOO, which
            // converts to the integer value 0.
            int exit_code = int( args.firstElement().number() );
            ::exit(exit_code);
            return undefined_;
        }

        BSONObj JSGetMemInfo( const BSONObj& args ){
            ProcessInfo pi;
            uassert( 10258 ,  "processinfo not supported" , pi.supported() );
            
            BSONObjBuilder e;
            e.append( "virtual" , pi.getVirtualMemorySize() );
            e.append( "resident" , pi.getResidentSize() );
            
            BSONObjBuilder b;
            b.append( "ret" , e.obj() );
            
            return b.obj();
        }

        BSONObj AllocatePorts( const BSONObj &args ) {
            uassert( 10259 ,  "allocatePorts takes exactly 1 argument", args.nFields() == 1 );
            uassert( 10260 ,  "allocatePorts needs to be passed an integer", args.firstElement().isNumber() );
            
            int n = int( args.firstElement().number() );
            
            vector< int > ports;
            vector< int > sockets;
            for( int i = 0; i < n; ++i ) {
                int s = socket( AF_INET, SOCK_STREAM, 0 );
                assert( s );
                
                sockaddr_in address;
                memset(address.sin_zero, 0, sizeof(address.sin_zero));
                address.sin_family = AF_INET;
                address.sin_port = 0;
                address.sin_addr.s_addr = inet_addr( "127.0.0.1" );
                assert( 0 == ::bind( s, (sockaddr*)&address, sizeof( address ) ) );
                
                sockaddr_in newAddress;
                socklen_t len = sizeof( newAddress );
                assert( 0 == getsockname( s, (sockaddr*)&newAddress, &len ) );
                ports.push_back( ntohs( newAddress.sin_port ) );
                sockets.push_back( s );
            }
            for( vector< int >::const_iterator i = sockets.begin(); i != sockets.end(); ++i )
                assert( 0 == closesocket( *i ) );
            
            sort( ports.begin(), ports.end() );
            for( unsigned i = 1; i < ports.size(); ++i )
                massert( 10434 ,  "duplicate ports allocated", ports[ i - 1 ] != ports[ i ] );
            BSONObjBuilder b;
            b.append( "", ports );
            return b.obj();
        }

        map< int, pair< pid_t, int > > dbs;
        map< pid_t, int > shells;
#ifdef _WIN32
        map< pid_t, HANDLE > handles;
#endif
        
        char *copyString( const char *original ) {
            char *ret = reinterpret_cast< char * >( malloc( strlen( original ) + 1 ) );
            strcpy( ret, original );
            return ret;
        }
        
        boost::mutex &mongoProgramOutputMutex( *( new boost::mutex ) );
        stringstream mongoProgramOutput_;

        void writeMongoProgramOutputLine( int port, int pid, const char *line ) {
            boost::mutex::scoped_lock lk( mongoProgramOutputMutex );
            stringstream buf;
            if ( port > 0 )
                buf << "m" << port << "| " << line;
            else
                buf << "sh" << pid << "| " << line;
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
            pid_t pid_;
        public:
            pid_t pid() const { return pid_; }
            MongoProgramRunner( const BSONObj &args ) {
                assert( args.nFields() > 0 );
                string program( args.firstElement().valuestrsafe() );
                
                assert( !program.empty() );
                boost::filesystem::path programPath = ( boost::filesystem::path( argv0 ) ).branch_path() / program;
#ifdef _WIN32
                programPath.replace_extension("exe");
#endif
                massert( 10435 ,  "couldn't find " + programPath.native_file_string(), boost::filesystem::exists( programPath ) );
                
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
                
                if ( program != "mongod" && program != "mongos" && program != "mongobridge" )
                    port_ = 0;
                else
                    assert( port_ > 0 );
                if ( port_ > 0 && dbs.count( port_ ) != 0 ){
                    cerr << "count for port: " << port_ << " is not 0 is: " << dbs.count( port_ ) << endl;
                    assert( dbs.count( port_ ) == 0 );        
                }
            }
            
            void start() {
                int pipeEnds[ 2 ];
                assert( pipe( pipeEnds ) != -1 );
                
                fflush( 0 );
                launch_process(argv_, pipeEnds[1]); //sets pid_
                
                cout << "shell: started mongo program";
                int i = 0;
                while( argv_[ i ] )
                    cout << " " << argv_[ i++ ];
                cout << endl;
                
                i = 0;
                while( argv_[ i ] )
                    free( argv_[ i++ ] );
                free( argv_ );

                if ( port_ > 0 )
                    dbs.insert( make_pair( port_, make_pair( pid_, pipeEnds[ 1 ] ) ) );
                else
                    shells.insert( make_pair( pid_, pipeEnds[ 1 ] ) );
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
                        writeMongoProgramOutputLine( port_, pid_, "WARNING: mongod wrote null bytes to output" );
                    char *last = buf;
                    for( char *i = strchr( buf, '\n' ); i; last = i + 1, i = strchr( last, '\n' ) ) {
                        *i = '\0';
                        writeMongoProgramOutputLine( port_, pid_, last );
                    }
                    if ( ret == 0 ) {
                        if ( *last )
                            writeMongoProgramOutputLine( port_, pid_, last );
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
            void launch_process(char** argv, int child_stdout){
#ifdef _WIN32
                stringstream ss;
                for (int i=0; argv[i]; i++){
                    if (i) ss << ' ';
                    ss << '"' << argv[i] << '"';
                }

                string args = ss.str();

                boost::scoped_array<TCHAR> args_tchar (new TCHAR[args.size() + 1]);
                for (size_t i=0; i < args.size()+1; i++)
                    args_tchar[i] = args[i];

                HANDLE h = (HANDLE)_get_osfhandle(child_stdout);
                assert(h != INVALID_HANDLE_VALUE);
                assert(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

                STARTUPINFO si;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                si.hStdError = h;
                si.hStdOutput = h;
                si.dwFlags |= STARTF_USESTDHANDLES;

                PROCESS_INFORMATION pi;
                ZeroMemory(&pi, sizeof(pi));

                bool success = CreateProcess( NULL, args_tchar.get(), NULL, NULL, true, 0, NULL, NULL, &si, &pi);
                assert(success);

                CloseHandle(pi.hThread);

                pid_ = pi.dwProcessId;
                handles.insert( make_pair( pid_, pi.hProcess ) );

#else

                pid_ = fork();
                assert( pid_ != -1 );
                
                if ( pid_ == 0 ) {
                    
                    assert( dup2( child_stdout, STDOUT_FILENO ) != -1 );
                    assert( dup2( child_stdout, STDERR_FILENO ) != -1 );
                    execvp( argv[ 0 ], argv );
                    massert( 10436 ,  "Unable to start program" , 0 );
                }
#endif
            }
        };
        
        //returns true if process exited
        bool wait_for_pid(pid_t pid, bool block=true){
#ifdef _WIN32
            assert(handles.count(pid));
            HANDLE h = handles[pid];

            if (block)
                WaitForSingleObject(h, INFINITE);

            DWORD ignore;
            if(GetExitCodeProcess(h, &ignore)){
                CloseHandle(h);
                handles.erase(pid);
                return true;
            }else{
                return false;
            }
#else
            int ignore;
            return (pid_ == waitpid(pid, &ignore, (block ? 0 : WNOHANG)));
#endif
        }
        BSONObj StartMongoProgram( const BSONObj &a ) {
            MongoProgramRunner r( a );
            r.start();
            boost::thread t( r );
            return BSON( string( "" ) << int( r.pid() ) );
        }

        BSONObj RunMongoProgram( const BSONObj &a ) {
            MongoProgramRunner r( a );
            r.start();
            boost::thread t( r );
            wait_for_pid(r.pid());
            shells.erase( r.pid() );
            return BSON( string( "" ) << int( r.pid() ) );
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

        inline void kill_wrapper(pid_t pid, int sig, int port){
#ifdef _WIN32
            if (sig == SIGKILL){
                assert( handles.count(pid) );
                assert( ! TerminateProcess(handles[pid], 1) );
            }else{
                DBClientConnection conn;
                conn.connect("127.0.0.1:" + BSONObjBuilder::numStr(port));
                try {
                    conn.simpleCommand("admin", NULL, "shutdown");
                } catch (...) {
                    //Do nothing. This command never returns data to the client and the driver doesn't like that.
                }
            }
#else
            assert( 0 == kill( pid, signal ) );
#endif
        }
            
        
        void killDb( int port, pid_t _pid, int signal ) {
            pid_t pid;
            if ( port > 0 ) {
                if( dbs.count( port ) != 1 ) {
                    cout << "No db started on port: " << port << endl;
                    return;
                }
                pid = dbs[ port ].first;
            } else {
                pid = _pid;
            }
            
            kill_wrapper( pid, signal, port );
            
            int i = 0;
            for( ; i < 65; ++i ) {
                if ( i == 5 ) {
                    char now[64];
                    time_t_to_String(time(0), now);
                    now[ 20 ] = 0;
                    cout << now << " process on port " << port << ", with pid " << pid << " not terminated, sending sigkill" << endl;
                    kill_wrapper( pid, SIGKILL, port );
                }        
                if(wait_for_pid(pid, false))
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
            
            if ( port > 0 ) {
                close( dbs[ port ].second );
                dbs.erase( port );
            } else {
                close( shells[ pid ] );
                shells.erase( pid );
            }
            if ( i > 4 || signal == SIGKILL ) {
                sleepms( 4000 ); // allow operating system to reclaim resources
            }
        }

        int getSignal( const BSONObj &a ) {
            int ret = SIGTERM;
            if ( a.nFields() == 2 ) {
                BSONObjIterator i( a );
                i.next();
                BSONElement e = i.next();
                assert( e.isNumber() );
                ret = int( e.number() );
            }
            return ret;
        }
        
        BSONObj StopMongoProgram( const BSONObj &a ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
            assert( a.firstElement().isNumber() );
            int port = int( a.firstElement().number() );
            killDb( port, 0, getSignal( a ) );
            cout << "shell: stopped mongo program on port " << port << endl;
            return undefined_;
        }        
        
        BSONObj StopMongoProgramByPid( const BSONObj &a ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
            assert( a.firstElement().isNumber() );
            int pid = int( a.firstElement().number() );            
            killDb( 0, pid, getSignal( a ) );
            cout << "shell: stopped mongo program on pid " << pid << endl;
            return undefined_;            
        }
                                                
        void KillMongoProgramInstances() {
            vector< int > ports;
            for( map< int, pair< pid_t, int > >::iterator i = dbs.begin(); i != dbs.end(); ++i )
                ports.push_back( i->first );
            for( vector< int >::iterator i = ports.begin(); i != ports.end(); ++i )
                killDb( *i, 0, SIGTERM );            
            vector< pid_t > pids;
            for( map< pid_t, int >::iterator i = shells.begin(); i != shells.end(); ++i )
                pids.push_back( i->first );
            for( vector< pid_t >::iterator i = pids.begin(); i != pids.end(); ++i )
                killDb( 0, *i, SIGTERM );
        }
        
        MongoProgramScope::~MongoProgramScope() {
            try {
                KillMongoProgramInstances();
            } catch ( ... ) {
                assert( false );
            }
        }

        unsigned _randomSeed;
        
        BSONObj JSSrand( const BSONObj &a ) {
            uassert( 12518, "srand requires a single numeric argument",
                    a.nFields() == 1 && a.firstElement().isNumber() );
            _randomSeed = (unsigned)a.firstElement().numberLong(); // grab least significant digits
            return undefined_;
        }
        
        BSONObj JSRand( const BSONObj &a ) {
            uassert( 12519, "rand accepts no arguments", a.nFields() == 0 );
            unsigned r;
#if !defined(_WIN32)
            r = rand_r( &_randomSeed );
#else
            r = rand(); // seed not used in this case
#endif
            return BSON( "" << double( r ) / ( double( RAND_MAX ) + 1 ) );
        }
        
        void installShellUtils( Scope& scope ){
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "sleep" , JSSleep );
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "_srand" , JSSrand );
            scope.injectNative( "_rand" , JSRand );
            scope.injectNative( "allocatePorts", AllocatePorts );
            scope.injectNative( "_startMongoProgram", StartMongoProgram );
            scope.injectNative( "runMongoProgram", RunMongoProgram );
            scope.injectNative( "stopMongod", StopMongoProgram );
            scope.injectNative( "stopMongoProgram", StopMongoProgram );        
            scope.injectNative( "stopMongoProgramByPid", StopMongoProgramByPid );        
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "rawMongoProgramOutput", RawMongoProgramOutput );
        }

        void initScope( Scope &scope ) {
            scope.externalSetup();
            mongo::shellUtils::installShellUtils( scope );
            scope.execSetup( jsconcatcode_server , "setupServerCode" );
            
            if ( !_dbConnect.empty() ) {
                uassert( 12513, "connect failed", scope.exec( _dbConnect , "(connect)" , false , true , false ) );
                if ( !_dbAuth.empty() ) {
                    uassert( 12514, "login failed", scope.exec( _dbAuth , "(auth)" , true , true , false ) );
                }
            }
        }
    }
}
