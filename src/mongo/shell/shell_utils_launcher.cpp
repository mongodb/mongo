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
# include <fcntl.h>
# include <io.h>
# define SIGKILL 9
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <signal.h>
# include <sys/stat.h>
# include <sys/wait.h>
#endif

#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/client/clientOnly-private.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    extern bool dbexitCalled;

#ifdef _WIN32
    inline int close(int fd) { return _close(fd); }
    inline int read(int fd, void* buf, size_t size) { return _read(fd, buf, size); }
    inline int pipe(int fds[2]) { return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT); }
#endif

    /**
     * These utilities are thread safe but do not provide mutually exclusive access to resources
     * identified by the caller.  Resources identified by a pid or port should not be accessed
     * by different threads.  Dependent filesystem paths should not be accessed by different
     * threads.
     */
    namespace shell_utils {

        ProgramOutputMultiplexer programOutputLogger;

        bool ProgramRegistry::isPortRegistered( int port ) const {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            return _ports.count( port ) == 1;
        }
        
        pid_t ProgramRegistry::pidForPort( int port ) const {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            verify( isPortRegistered( port ) );
            return _ports.find( port )->second.first;
        }
        
        void ProgramRegistry::registerPort( int port, pid_t pid, int output ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            verify( !isPortRegistered( port ) );
            _ports.insert( make_pair( port, make_pair( pid, output ) ) );
        }
        
        void ProgramRegistry::deletePort( int port ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            if ( !isPortRegistered( port ) ) {
                return;
            }
            close( _ports.find( port )->second.second );
            _ports.erase( port );
        }
        
        void ProgramRegistry::getRegisteredPorts( vector<int> &ports ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            for( map<int,pair<pid_t,int> >::const_iterator i = _ports.begin(); i != _ports.end();
                ++i ) {
                ports.push_back( i->first );
            }
        }
        
        bool ProgramRegistry::isPidRegistered( pid_t pid ) const {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            return _pids.count( pid ) == 1;
        }
        
        void ProgramRegistry::registerPid( pid_t pid, int output ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            verify( !isPidRegistered( pid ) );
            _pids.insert( make_pair( pid, output ) );
        }
        
        void ProgramRegistry::deletePid( pid_t pid ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            if ( !isPidRegistered( pid ) ) {
                return;
            }
            close( _pids.find( pid )->second );
            _pids.erase( pid );
        }
        
        void ProgramRegistry::getRegisteredPids( vector<pid_t> &pids ) {
            boost::recursive_mutex::scoped_lock lk( _mutex );
            for( map<pid_t,int>::const_iterator i = _pids.begin(); i != _pids.end(); ++i ) {
                pids.push_back( i->first );
            }
        }
        
        ProgramRegistry &registry = *( new ProgramRegistry() );

        void goingAwaySoon() {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            mongo::dbexitCalled = true;
        }

        void ProgramOutputMultiplexer::appendLine( int port, int pid, const char *line ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            if( mongo::dbexitCalled ) throw "program is terminating";
            stringstream buf;
            if ( port > 0 )
                buf << " m" << port << "| " << line;
            else
                buf << "sh" << pid << "| " << line;
            printf( "%s\n", buf.str().c_str() ); // cout << buf.str() << endl;
            _buffer << buf.str() << endl;
        }

        string ProgramOutputMultiplexer::str() const {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            string ret = _buffer.str();
            size_t len = ret.length();
            if ( len > 100000 ) {
                ret = ret.substr( len - 100000, 100000 );
            }
            return ret;
        }
        
        void ProgramOutputMultiplexer::clear() {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            _buffer.str( "" );            
        }
        
        ProgramRunner::ProgramRunner( const BSONObj &args ) {
            verify( !args.isEmpty() );

            string program( args.firstElement().valuestrsafe() );
            verify( !program.empty() );
            boost::filesystem::path programPath = findProgram(program);

#if 0
            if (program == "mongos") {
                _argv.push_back("valgrind");
                _argv.push_back("--log-file=/tmp/mongos-%p.valgrind");
                _argv.push_back("--leak-check=yes");
                _argv.push_back("--suppressions=valgrind.suppressions");
                //_argv.push_back("--error-exitcode=1");
                _argv.push_back("--");
            }
#endif

            _argv.push_back( programPath.native_file_string() );

            _port = -1;

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
                    _port = -2;
                else if ( _port == -2 )
                    _port = strtol( str.c_str(), 0, 10 );
                _argv.push_back(str);
            }

            if ( program != "mongod" && program != "mongos" && program != "mongobridge" )
                _port = 0;
            else {
                if ( _port <= 0 )
                    log() << "error: a port number is expected when running mongod (etc.) from the shell" << endl;
                verify( _port > 0 );
            }
            if ( _port > 0 ) {
                bool haveDbForPort = registry.isPortRegistered( _port );
                if ( haveDbForPort ) {
                    log() << "already have db for port: " << _port << endl;
                    verify( !haveDbForPort );
                }
            }
        }

        void ProgramRunner::start() {
            int pipeEnds[ 2 ];
            verify( pipe( pipeEnds ) != -1 );

            fflush( 0 );
            launchProcess(pipeEnds[1]); //sets _pid

            {
                stringstream ss;
                ss << "shell: started program";
                for (unsigned i=0; i < _argv.size(); i++)
                    ss << " " << _argv[i];
                log() << ss.str() << endl;
            }

            if ( _port > 0 )
                registry.registerPort( _port, _pid, pipeEnds[ 1 ] );
            else
                registry.registerPid( _pid, pipeEnds[ 1 ] );
            _pipe = pipeEnds[ 0 ];
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
                        log() << "error: lenToRead: " << lenToRead << endl;
                        log() << "first 300: " << string(buf,0,300) << endl;
                    }
                    verify( lenToRead > 0 );
                    int ret = read( _pipe, (void *)start, lenToRead );
                    if( mongo::dbexitCalled )
                        break;
                    verify( ret != -1 );
                    start[ ret ] = '\0';
                    if ( strlen( start ) != unsigned( ret ) )
                        programOutputLogger.appendLine( _port, _pid, "WARNING: mongod wrote null bytes to output" );
                    char *last = buf;
                    for( char *i = strchr( buf, '\n' ); i; last = i + 1, i = strchr( last, '\n' ) ) {
                        *i = '\0';
                        programOutputLogger.appendLine( _port, _pid, last );
                    }
                    if ( ret == 0 ) {
                        if ( *last )
                            programOutputLogger.appendLine( _port, _pid, last );
                        close( _pipe );
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
        
        boost::filesystem::path ProgramRunner::findProgram( const string &prog ) {
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

        void ProgramRunner::launchProcess( int child_stdout ) {
#ifdef _WIN32
            stringstream ss;
            for( unsigned i=0; i < _argv.size(); i++ ) {
                if (i) ss << ' ';
                if (_argv[i].find(' ') == string::npos)
                    ss << _argv[i];
                else {
                    ss << '"';
                    // escape all embedded quotes
                    for (size_t j=0; j<_argv[i].size(); ++j) {
                        if (_argv[i][j]=='"') ss << '"';
                        ss << _argv[i][j];
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
                ss << "couldn't start process " << _argv[0] << "; " << lpMsgBuf;
                uassert(14042, ss.str(), success);
                LocalFree(lpMsgBuf);
            }

            CloseHandle(pi.hThread);

            _pid = pi.dwProcessId;
            registry._handles.insert( make_pair( _pid, pi.hProcess ) );

#else

            scoped_array<const char *> argvStorage( new const char* [_argv.size()+1] );
            const char** argv = argvStorage.get();
            for (unsigned i=0; i < _argv.size(); i++) {
                argv[i] = _argv[i].c_str();
            }
            argv[_argv.size()] = 0;

            scoped_array<const char *> envStorage( new const char* [2] );
            const char** env = envStorage.get();
            env[0] = NULL;
            env[1] = NULL;

            bool isMongos = ( _argv[0].find( "mongos" ) != string::npos );
            
            _pid = fork();
            // Async signal unsafe functions should not be called in the child process.

            if ( _pid == -1 ) {
                verify( _pid != -1 );
            }
            else if ( _pid == 0 ) {
                // DON'T ASSERT IN THIS BLOCK - very bad things will happen

                if ( dup2( child_stdout, STDOUT_FILENO ) == -1 ||
                        dup2( child_stdout, STDERR_FILENO ) == -1 ) {

                    // Async signal unsafe code reporting a terminal error condition.
                    cout << "Unable to dup2 child output: " << errnoWithDescription() << endl;
                    ::_Exit(-1); //do not pass go, do not call atexit handlers
                }

                // Heap-check for mongos only. 'argv[0]' must be in the path format.
                if ( isMongos ) {
#if defined(HEAP_CHECKING)
                    env[0] = "HEAPCHECK=normal";
                    env[1] = NULL;

                    // NOTE execve is async signal safe, but it is not clear that execvpe is async
                    // signal safe.
                    execvpe( argv[ 0 ], const_cast<char**>(argv) , const_cast<char**>(env) );
#endif // HEAP_CHECKING
                }

                // NOTE execve is async signal safe, but it is not clear that execvp is async
                // signal safe.
                execvp( argv[ 0 ], const_cast<char**>(argv) );

                // Async signal unsafe code reporting a terminal error condition.
                cout << "Unable to start program " << argv[0] << ' ' << errnoWithDescription() << endl;
                ::_Exit(-1);
            }

#endif
        }

        //returns true if process exited
        bool wait_for_pid(pid_t pid, bool block=true, int* exit_code=NULL) {
#ifdef _WIN32
            verify(registry._handles.count(pid));
            HANDLE h = registry._handles[pid];

            if (block)
                WaitForSingleObject(h, INFINITE);

            DWORD tmp;
            if(GetExitCodeProcess(h, &tmp)) {
                if ( tmp == STILL_ACTIVE ) {
                    return false;
                }
                CloseHandle(h);
                registry._handles.erase(pid);
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

        BSONObj RawMongoProgramOutput( const BSONObj &args, void* data ) {
            return BSON( "" << programOutputLogger.str() );
        }
        
        BSONObj ClearRawMongoProgramOutput( const BSONObj &args, void* data ) {
            programOutputLogger.clear();
            return undefinedReturn;
        }

        BSONObj WaitProgram( const BSONObj& a, void* data ) {
            int pid = singleArg( a ).numberInt();
            BSONObj x = BSON( "" << wait_for_pid( pid ) );
            registry.deletePid( pid );
            return x;
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
                registry.deletePort( r.port() );
            }
            else {
                registry.deletePid( r.pid() );
            }
            return BSON( string( "" ) << exit_code );
        }

        BSONObj RunProgram(const BSONObj &a, void* data) {
            ProgramRunner r( a );
            r.start();
            boost::thread t( r );
            int exit_code;
            wait_for_pid(r.pid(), true,  &exit_code);
            registry.deletePid( r.pid() );
            return BSON( string( "" ) << exit_code );
        }

        BSONObj ResetDbpath( const BSONObj &a, void* data ) {
            verify( a.nFields() == 1 );
            string path = a.firstElement().valuestrsafe();
            verify( !path.empty() );
            if ( boost::filesystem::exists( path ) )
                boost::filesystem::remove_all( path );
            boost::filesystem::create_directory( path );
            return undefinedReturn;
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
            return undefinedReturn;
        }

      inline void kill_wrapper( pid_t pid, int sig, int port, const BSONObj& opt ) {
#ifdef _WIN32
            if (sig == SIGKILL || port == 0) {
                verify( registry._handles.count(pid) );
                TerminateProcess(registry._handles[pid], 1); // returns failure for "zombie" processes.
            }
            else {
                DBClientConnection conn;
                try {
                    conn.connect("127.0.0.1:" + BSONObjBuilder::numStr(port));

                    BSONElement authObj = opt["auth"];

                    if ( !authObj.eoo() ){
                        string errMsg;
                        conn.auth( "admin", authObj["user"].String(),
                                   authObj["pwd"].String(), errMsg );

                        if ( !errMsg.empty() ) {
                            cout << "Failed to authenticate before shutdown: "
                                 << errMsg << endl;
                        }
                    }

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
                    log() << "killFailed: " << errnoWithDescription() << endl;
                    verify( x == 0 );
                }
            }

#endif
        }

       int killDb( int port, pid_t _pid, int signal, const BSONObj& opt ) {
            pid_t pid;
            int exitCode = 0;
            if ( port > 0 ) {
                if( !registry.isPortRegistered( port ) ) {
                    log() << "No db started on port: " << port << endl;
                    return 0;
                }
                pid = registry.pidForPort( port );
            }
            else {
                pid = _pid;
            }

            kill_wrapper( pid, signal, port, opt );

            int i = 0;
            for( ; i < 130; ++i ) {
                if ( i == 60 ) {
                    char now[64];
                    time_t_to_String(time(0), now);
                    now[ 20 ] = 0;
                    log() << now << " process on port " << port << ", with pid " << pid << " not terminated, sending sigkill" << endl;
                    kill_wrapper( pid, SIGKILL, port, opt );
                }
                if(wait_for_pid(pid, false, &exitCode))
                    break;
                sleepmillis( 1000 );
            }
            if ( i == 130 ) {
                char now[64];
                time_t_to_String(time(0), now);
                now[ 20 ] = 0;
                log() << now << " failed to terminate process on port " << port << ", with pid " << pid << endl;
                verify( "Failed to terminate process" == 0 );
            }

            if ( port > 0 ) {
                registry.deletePort( port );
            }
            else {
                registry.deletePid( pid );
            }
            // FIXME I think the intention here is to do an extra sleep only when SIGKILL is sent to the child process.
            // We may want to change the 4 below to 29, since values of i greater than that indicate we sent a SIGKILL.
            if ( i > 4 || signal == SIGKILL ) {
                sleepmillis( 4000 ); // allow operating system to reclaim resources
            }

            return exitCode;
        }

       int killDb( int port, pid_t _pid, int signal ) {
           BSONObj dummyOpt;
           return killDb( port, _pid, signal, dummyOpt );
       }

        int getSignal( const BSONObj &a ) {
            int ret = SIGTERM;
            if ( a.nFields() >= 2 ) {
                BSONObjIterator i( a );
                i.next();
                BSONElement e = i.next();
                verify( e.isNumber() );
                ret = int( e.number() );
            }
            return ret;
        }

        BSONObj getStopMongodOpts( const BSONObj &a ) {
            if ( a.nFields() == 3 ) {
                BSONObjIterator i( a );
                i.next();
                i.next();
                BSONElement e = i.next();

                if ( e.isABSONObj() ){
                  return e.embeddedObject();
                }
            }

            return BSONObj();
        }

        /** stopMongoProgram(port[, signal]) */
        BSONObj StopMongoProgram( const BSONObj &a, void* data ) {
            verify( a.nFields() >= 1 || a.nFields() <= 3 );
            uassert( 15853 , "stopMongo needs a number" , a.firstElement().isNumber() );
            int port = int( a.firstElement().number() );
            int code = killDb( port, 0, getSignal( a ), getStopMongodOpts( a ));
            log() << "shell: stopped mongo program on port " << port << endl;
            return BSON( "" << (double)code );
        }

        BSONObj StopMongoProgramByPid( const BSONObj &a, void* data ) {
            verify( a.nFields() == 1 || a.nFields() == 2 );
            uassert( 15852 , "stopMongoByPid needs a number" , a.firstElement().isNumber() );
            int pid = int( a.firstElement().number() );
            int code = killDb( 0, pid, getSignal( a ) );
            log() << "shell: stopped mongo program on pid " << pid << endl;
            return BSON( "" << (double)code );
        }

        void KillMongoProgramInstances() {
            vector< int > ports;
            registry.getRegisteredPorts( ports );
            for( vector< int >::iterator i = ports.begin(); i != ports.end(); ++i )
                killDb( *i, 0, SIGTERM );
            vector< pid_t > pids;
            registry.getRegisteredPids( pids );
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
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "copyDbpath", CopyDbpath );
        }
    }
}
