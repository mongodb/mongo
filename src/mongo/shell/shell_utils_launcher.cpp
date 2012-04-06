// mongo/shell/shell_utils_launcher.cpp
/*
 *    Copyright 2010 10gen Inc.
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

#include <iostream>
#include <map>
#include <vector>

#ifdef _WIN32
# include <io.h>
# define SIGKILL 9
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <signal.h>
# include <sys/stat.h>
# include <sys/wait.h>
#endif

#include "shell_utils.h"
#include "shell_utils_launcher.h"
#include "../client/clientOnly-private.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    extern bool dbexitCalled;

#ifdef _WIN32
    inline int close(int fd) { return _close(fd); }
    inline int read(int fd, void* buf, size_t size) { return _read(fd, buf, size); }
    inline int pipe(int fds[2]) { return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT); }
#endif

    // these functions have not been audited for thread safety - currently they are called with an exclusive js mutex
    namespace shellUtils {

        map< int, pair< pid_t, int > > dbs;
        map< pid_t, int > shells;
#ifdef _WIN32
        map< pid_t, HANDLE > handles;
#endif

        stringstream mongoProgramOutput_;

        void goingAwaySoon() {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            mongo::dbexitCalled = true;
        }

        void writeMongoProgramOutputLine( int port, int pid, const char *line ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            if( mongo::dbexitCalled ) throw "program is terminating";
            stringstream buf;
            if ( port > 0 )
                buf << " m" << port << "| " << line;
            else
                buf << "sh" << pid << "| " << line;
            printf( "%s\n", buf.str().c_str() ); // cout << buf.str() << endl;
            mongoProgramOutput_ << buf.str() << endl;
        }

        // only returns last 100000 characters
        BSONObj RawMongoProgramOutput( const BSONObj &args, void* data ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            string out = mongoProgramOutput_.str();
            size_t len = out.length();
            if ( len > 100000 )
                out = out.substr( len - 100000, 100000 );
            return BSON( "" << out );
        }

        BSONObj ClearRawMongoProgramOutput( const BSONObj &args, void* data ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            mongoProgramOutput_.str( "" );
            return undefined_;
        }

        boost::filesystem::path ProgramRunner::find(string prog) {
            boost::filesystem::path p = prog;
#ifdef _WIN32
            p = change_extension(p, ".exe");
#endif

            if( boost::filesystem::exists(p) ) {
#ifndef _WIN32
                p = boost::filesystem::initial_path() / p;
#endif
                return p;
            }

            {
                boost::filesystem::path t = boost::filesystem::current_path() / p;
                if( boost::filesystem::exists(t)  ) return t;
            }
            {
                boost::filesystem::path t = boost::filesystem::initial_path() / p;
                if( boost::filesystem::exists(t)  ) return t;
            }
            return p; // not found; might find via system path
        }

        ProgramRunner::ProgramRunner( const BSONObj &args , bool isMongoProgram) {
            verify( !args.isEmpty() );

            string program( args.firstElement().valuestrsafe() );
            verify( !program.empty() );
            boost::filesystem::path programPath = find(program);

            if (isMongoProgram) {
#if 0
                if (program == "mongos") {
                    argv_.push_back("valgrind");
                    argv_.push_back("--log-file=/tmp/mongos-%p.valgrind");
                    argv_.push_back("--leak-check=yes");
                    argv_.push_back("--suppressions=valgrind.suppressions");
                    //argv_.push_back("--error-exitcode=1");
                    argv_.push_back("--");
                }
#endif
            }

            argv_.push_back( programPath.native_file_string() );

            port_ = -1;

            BSONObjIterator j( args );
            j.next(); // skip program name (handled above)
            while(j.more()) {
                BSONElement e = j.next();
                string str;
                if ( e.isNumber() ) {
                    stringstream ss;
                    ss << e.number();
                    str = ss.str();
                }
                else {
                    verify( e.type() == mongo::String );
                    str = e.valuestr();
                }
                if ( str == "--port" )
                    port_ = -2;
                else if ( port_ == -2 )
                    port_ = strtol( str.c_str(), 0, 10 );
                argv_.push_back(str);
            }

            if ( program != "mongod" && program != "mongos" && program != "mongobridge" )
                port_ = 0;
            else {
                if ( port_ <= 0 )
                    cout << "error: a port number is expected when running mongod (etc.) from the shell" << endl;
                verify( port_ > 0 );
            }
            if ( port_ > 0 && dbs.count( port_ ) != 0 ) {
                cerr << "count for port: " << port_ << " is not 0 is: " << dbs.count( port_ ) << endl;
                verify( dbs.count( port_ ) == 0 );
            }
        }

        void ProgramRunner::start() {
            int pipeEnds[ 2 ];
            verify( pipe( pipeEnds ) != -1 );

            fflush( 0 );
            launch_process(pipeEnds[1]); //sets pid_

            {
                stringstream ss;
                ss << "shell: started program";
                for (unsigned i=0; i < argv_.size(); i++)
                    ss << " " << argv_[i];
                ss << '\n';
                cout << ss.str(); cout.flush();
            }

            if ( port_ > 0 )
                dbs.insert( make_pair( port_, make_pair( pid_, pipeEnds[ 1 ] ) ) );
            else
                shells.insert( make_pair( pid_, pipeEnds[ 1 ] ) );
            pipe_ = pipeEnds[ 0 ];
        }

        void ProgramRunner::operator()() {
            try {
                // This assumes there aren't any 0's in the mongo program output.
                // Hope that's ok.
                const unsigned bufSize = 128 * 1024;
                char buf[ bufSize ];
                char temp[ bufSize ];
                char *start = buf;
                while( 1 ) {
                    int lenToRead = ( bufSize - 1 ) - ( start - buf );
                    if ( lenToRead <= 0 ) {
                        cout << "error: lenToRead: " << lenToRead << endl;
                        cout << "first 300: " << string(buf,0,300) << endl;
                    }
                    verify( lenToRead > 0 );
                    int ret = read( pipe_, (void *)start, lenToRead );
                    if( mongo::dbexitCalled )
                        break;
                    verify( ret != -1 );
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
                    }
                    else {
                        verify( strlen( buf ) < bufSize );
                    }
                    start = buf + strlen( buf );
                }
            }
            catch(...) {
            }
        }
        
        void ProgramRunner::launch_process(int child_stdout) {
#ifdef _WIN32
            stringstream ss;
            for( unsigned i=0; i < argv_.size(); i++ ) {
                if (i) ss << ' ';
                if (argv_[i].find(' ') == string::npos)
                    ss << argv_[i];
                else {
                    ss << '"';
                    // escape all embedded quotes
                    for (size_t j=0; j<argv_[i].size(); ++j) {
                        if (argv_[i][j]=='"') ss << '"';
                        ss << argv_[i][j];
                    }
                    ss << '"';
                }
            }

            string args = ss.str();

            boost::scoped_array<TCHAR> args_tchar (new TCHAR[args.size() + 1]);
            size_t i;
            for(i=0; i < args.size(); i++)
                args_tchar[i] = args[i];
            args_tchar[i] = 0;

            HANDLE h = (HANDLE)_get_osfhandle(child_stdout);
            verify(h != INVALID_HANDLE_VALUE);
            verify(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

            STARTUPINFO si;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.hStdError = h;
            si.hStdOutput = h;
            si.dwFlags |= STARTF_USESTDHANDLES;

            PROCESS_INFORMATION pi;
            ZeroMemory(&pi, sizeof(pi));

            bool success = CreateProcess( NULL, args_tchar.get(), NULL, NULL, true, 0, NULL, NULL, &si, &pi) != 0;
            if (!success) {
                LPSTR lpMsgBuf=0;
                DWORD dw = GetLastError();
                FormatMessageA(
                    FORMAT_MESSAGE_ALLOCATE_BUFFER |
                    FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL,
                    dw,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPSTR)&lpMsgBuf,
                    0, NULL );
                stringstream ss;
                ss << "couldn't start process " << argv_[0] << "; " << lpMsgBuf;
                uassert(14042, ss.str(), success);
                LocalFree(lpMsgBuf);
            }

            CloseHandle(pi.hThread);

            pid_ = pi.dwProcessId;
            handles.insert( make_pair( pid_, pi.hProcess ) );

#else

            pid_ = fork();
            verify( pid_ != -1 );

            if ( pid_ == 0 ) {
                // DON'T ASSERT IN THIS BLOCK - very bad things will happen

                const char** argv = new const char* [argv_.size()+1]; // don't need to free - in child
                for (unsigned i=0; i < argv_.size(); i++) {
                    argv[i] = argv_[i].c_str();
                }
                argv[argv_.size()] = 0;

                if ( dup2( child_stdout, STDOUT_FILENO ) == -1 ||
                        dup2( child_stdout, STDERR_FILENO ) == -1 ) {
                    cout << "Unable to dup2 child output: " << errnoWithDescription() << endl;
                    ::_Exit(-1); //do not pass go, do not call atexit handlers
                }

                const char** env = new const char* [2]; // don't need to free - in child
                env[0] = NULL;
#if defined(HEAP_CHECKING)
                env[0] = "HEAPCHECK=normal";
                env[1] = NULL;

                // Heap-check for mongos only. 'argv[0]' must be in the path format.
                if ( argv_[0].find("mongos") != string::npos) {
                    execvpe( argv[ 0 ], const_cast<char**>(argv) , const_cast<char**>(env) );
                }
#endif // HEAP_CHECKING

                execvp( argv[ 0 ], const_cast<char**>(argv) );

                cout << "Unable to start program " << argv[0] << ' ' << errnoWithDescription() << endl;
                ::_Exit(-1);
            }

#endif
        }

        //returns true if process exited
        bool wait_for_pid(pid_t pid, bool block=true, int* exit_code=NULL) {
#ifdef _WIN32
            verify(handles.count(pid));
            HANDLE h = handles[pid];

            if (block)
                WaitForSingleObject(h, INFINITE);

            DWORD tmp;
            if(GetExitCodeProcess(h, &tmp)) {
                if ( tmp == STILL_ACTIVE ) {
                    return false;
                }
                CloseHandle(h);
                handles.erase(pid);
                if (exit_code)
                    *exit_code = tmp;
                return true;
            }
            else {
                return false;
            }
#else
            int tmp;
            bool ret = (pid == waitpid(pid, &tmp, (block ? 0 : WNOHANG)));
            if (exit_code)
                *exit_code = WEXITSTATUS(tmp);
            return ret;

#endif
        }

        BSONObj WaitProgram( const BSONObj& a, void* data ) {
            int pid = oneArg( a ).numberInt();
            BSONObj x = BSON( "" << wait_for_pid( pid ) );
            shells.erase( pid );
            return x;
        }

        BSONObj WaitMongoProgramOnPort( const BSONObj &a, void* data ) {
            int port = oneArg( a ).numberInt();
            uassert( 13621, "no known mongo program on port", dbs.count( port ) != 0 );
            log() << "waiting port: " << port << ", pid: " << dbs[ port ].first << endl;
            bool ret = wait_for_pid( dbs[ port ].first );
            if ( ret ) {
                dbs.erase( port );
            }
            return BSON( "" << ret );
        }

        BSONObj StartMongoProgram( const BSONObj &a, void* data ) {
            _nokillop = true;
            ProgramRunner r( a );
            r.start();
            boost::thread t( r );
            return BSON( string( "" ) << int( r.pid() ) );
        }

        BSONObj RunMongoProgram( const BSONObj &a, void* data ) {
            ProgramRunner r( a );
            r.start();
            boost::thread t( r );
            int exit_code;
            wait_for_pid( r.pid(), true, &exit_code );
            if ( r.port() > 0 ) {
                dbs.erase( r.port() );
            }
            else {
                shells.erase( r.pid() );
            }
            return BSON( string( "" ) << exit_code );
        }

        BSONObj RunProgram(const BSONObj &a, void* data) {
            ProgramRunner r( a, false );
            r.start();
            boost::thread t( r );
            int exit_code;
            wait_for_pid(r.pid(), true,  &exit_code);
            shells.erase( r.pid() );
            return BSON( string( "" ) << exit_code );
        }

        BSONObj ResetDbpath( const BSONObj &a, void* data ) {
            verify( a.nFields() == 1 );
            string path = a.firstElement().valuestrsafe();
            verify( !path.empty() );
            if ( boost::filesystem::exists( path ) )
                boost::filesystem::remove_all( path );
            boost::filesystem::create_directory( path );
            return undefined_;
        }

        void copyDir( const boost::filesystem::path &from, const boost::filesystem::path &to ) {
            boost::filesystem::directory_iterator end;
            boost::filesystem::directory_iterator i( from );
            while( i != end ) {
                boost::filesystem::path p = *i;
                if ( p.leaf() != "mongod.lock" ) {
                    if ( boost::filesystem::is_directory( p ) ) {
                        boost::filesystem::path newDir = to / p.leaf();
                        boost::filesystem::create_directory( newDir );
                        copyDir( p, newDir );
                    }
                    else {
                        boost::filesystem::copy_file( p, to / p.leaf() );
                    }
                }
                ++i;
            }
        }

        // NOTE target dbpath will be cleared first
        BSONObj CopyDbpath( const BSONObj &a, void* data ) {
            verify( a.nFields() == 2 );
            BSONObjIterator i( a );
            string from = i.next().str();
            string to = i.next().str();
            verify( !from.empty() );
            verify( !to.empty() );
            if ( boost::filesystem::exists( to ) )
                boost::filesystem::remove_all( to );
            boost::filesystem::create_directory( to );
            copyDir( from, to );
            return undefined_;
        }

        inline void kill_wrapper(pid_t pid, int sig, int port) {
#ifdef _WIN32
            if (sig == SIGKILL || port == 0) {
                verify( handles.count(pid) );
                TerminateProcess(handles[pid], 1); // returns failure for "zombie" processes.
            }
            else {
                DBClientConnection conn;
                try {
                    conn.connect("127.0.0.1:" + BSONObjBuilder::numStr(port));
                    BSONObj info;
                    BSONObjBuilder b;
                    b.append( "shutdown", 1 );
                    b.append( "force", 1 );
                    conn.runCommand( "admin", b.done(), info );
                }
                catch (...) {
                    //Do nothing. This command never returns data to the client and the driver doesn't like that.
                }
            }
#else
            int x = kill( pid, sig );
            if ( x ) {
                if ( errno == ESRCH ) {
                }
                else {
                    cout << "killFailed: " << errnoWithDescription() << endl;
                    verify( x == 0 );
                }
            }

#endif
        }

        int killDb( int port, pid_t _pid, int signal ) {
            pid_t pid;
            int exitCode = 0;
            if ( port > 0 ) {
                if( dbs.count( port ) != 1 ) {
                    cout << "No db started on port: " << port << endl;
                    return 0;
                }
                pid = dbs[ port ].first;
            }
            else {
                pid = _pid;
            }

            kill_wrapper( pid, signal, port );

            int i = 0;
            for( ; i < 130; ++i ) {
                if ( i == 60 ) {
                    char now[64];
                    time_t_to_String(time(0), now);
                    now[ 20 ] = 0;
                    cout << now << " process on port " << port << ", with pid " << pid << " not terminated, sending sigkill" << endl;
                    kill_wrapper( pid, SIGKILL, port );
                }
                if(wait_for_pid(pid, false, &exitCode))
                    break;
                sleepmillis( 1000 );
            }
            if ( i == 130 ) {
                char now[64];
                time_t_to_String(time(0), now);
                now[ 20 ] = 0;
                cout << now << " failed to terminate process on port " << port << ", with pid " << pid << endl;
                verify( "Failed to terminate process" == 0 );
            }

            if ( port > 0 ) {
                close( dbs[ port ].second );
                dbs.erase( port );
            }
            else {
                close( shells[ pid ] );
                shells.erase( pid );
            }
            // FIXME I think the intention here is to do an extra sleep only when SIGKILL is sent to the child process.
            // We may want to change the 4 below to 29, since values of i greater than that indicate we sent a SIGKILL.
            if ( i > 4 || signal == SIGKILL ) {
                sleepmillis( 4000 ); // allow operating system to reclaim resources
            }

            return exitCode;
        }

        int getSignal( const BSONObj &a ) {
            int ret = SIGTERM;
            if ( a.nFields() == 2 ) {
                BSONObjIterator i( a );
                i.next();
                BSONElement e = i.next();
                verify( e.isNumber() );
                ret = int( e.number() );
            }
            return ret;
        }

        /** stopMongoProgram(port[, signal]) */
        BSONObj StopMongoProgram( const BSONObj &a, void* data ) {
            verify( a.nFields() == 1 || a.nFields() == 2 );
            uassert( 15853 , "stopMongo needs a number" , a.firstElement().isNumber() );
            int port = int( a.firstElement().number() );
            int code = killDb( port, 0, getSignal( a ) );
            cout << "shell: stopped mongo program on port " << port << endl;
            return BSON( "" << (double)code );
        }

        BSONObj StopMongoProgramByPid( const BSONObj &a, void* data ) {
            verify( a.nFields() == 1 || a.nFields() == 2 );
            uassert( 15852 , "stopMongoByPid needs a number" , a.firstElement().isNumber() );
            int pid = int( a.firstElement().number() );
            int code = killDb( 0, pid, getSignal( a ) );
            cout << "shell: stopped mongo program on pid " << pid << endl;
            return BSON( "" << (double)code );
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
            DESTRUCTOR_GUARD(
                KillMongoProgramInstances();
                ClearRawMongoProgramOutput( BSONObj(), 0 );
            )
        }

        void installShellUtilsLauncher( Scope& scope ) {
            scope.injectNative( "_startMongoProgram", StartMongoProgram );
            scope.injectNative( "runProgram", RunProgram );
            scope.injectNative( "run", RunProgram );
            scope.injectNative( "runMongoProgram", RunMongoProgram );
            scope.injectNative( "stopMongod", StopMongoProgram );
            scope.injectNative( "stopMongoProgram", StopMongoProgram );
            scope.injectNative( "stopMongoProgramByPid", StopMongoProgramByPid );
            scope.injectNative( "rawMongoProgramOutput", RawMongoProgramOutput );
            scope.injectNative( "clearRawMongoProgramOutput", ClearRawMongoProgramOutput );
            scope.injectNative( "waitProgram" , WaitProgram );
            scope.injectNative( "waitMongoProgramOnPort" , WaitMongoProgramOnPort );
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "copyDbpath", CopyDbpath );
        }
    }
}
