// utils.cpp
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

#include <boost/thread/xtime.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>
#include <map>
#include <sstream>
#include <fstream>
#include <vector>
#include <fcntl.h>

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

#include "utils.h"
#include "../client/dbclient.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "../util/text.h"
#include "../util/heapcheck.h"
#include "../util/time_support.h"
#include "../util/file.h"
#include "../client/clientOnly-private.h"

namespace mongo {

    DBClientWithCommands *latestConn = 0;
    extern bool dbexitCalled;

#ifdef _WIN32
    inline int close(int fd) { return _close(fd); }
    inline int read(int fd, void* buf, size_t size) { return _read(fd, buf, size); }
    inline int pipe(int fds[2]) { return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT); }
#endif

    namespace JSFiles {
        extern const JSFile servers;
    }

    // these functions have not been audited for thread safety - currently they are called with an exclusive js mutex
    namespace shellUtils {

        Scope* theScope = 0;

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

        // real methods

        void goingAwaySoon();
        BSONObj Quit(const BSONObj& args, void* data) {
            // If not arguments are given first element will be EOO, which
            // converts to the integer value 0.
            goingAwaySoon();
            int exit_code = int( args.firstElement().number() );
            ::exit(exit_code);
            return undefined_;
        }

        BSONObj JSGetMemInfo( const BSONObj& args, void* data ) {
            ProcessInfo pi;
            uassert( 10258 ,  "processinfo not supported" , pi.supported() );

            BSONObjBuilder e;
            e.append( "virtual" , pi.getVirtualMemorySize() );
            e.append( "resident" , pi.getResidentSize() );

            BSONObjBuilder b;
            b.append( "ret" , e.obj() );

            return b.obj();
        }


#ifndef MONGO_SAFE_SHELL

        BSONObj listFiles(const BSONObj& _args, void* data) {
            static BSONObj cd = BSON( "0" << "." );
            BSONObj args = _args.isEmpty() ? cd : _args;

            uassert( 10257 ,  "need to specify 1 argument to listFiles" , args.nFields() == 1 );

            BSONArrayBuilder lst;

            string rootname = args.firstElement().valuestrsafe();
            boost::filesystem::path root( rootname );
            stringstream ss;
            ss << "listFiles: no such directory: " << rootname;
            string msg = ss.str();
            uassert( 12581, msg.c_str(), boost::filesystem::exists( root ) );

            boost::filesystem::directory_iterator end;
            boost::filesystem::directory_iterator i( root);

            while ( i != end ) {
                boost::filesystem::path p = *i;
                BSONObjBuilder b;
                b << "name" << p.string();
                b.appendBool( "isDirectory", is_directory( p ) );
                if ( ! boost::filesystem::is_directory( p ) ) {
                    try {
                        b.append( "size" , (double)boost::filesystem::file_size( p ) );
                    }
                    catch ( ... ) {
                        i++;
                        continue;
                    }
                }

                lst.append( b.obj() );
                i++;
            }
            
            BSONObjBuilder ret;
            ret.appendArray( "", lst.done() );
            return ret.obj();
        }

        BSONObj ls(const BSONObj& args, void* data) {
            BSONObj o = listFiles(args, data);
            if( !o.isEmpty() ) {
                for( BSONObj::iterator i = o.firstElement().Obj().begin(); i.more(); ) {
                    BSONObj f = i.next().Obj();
                    cout << f["name"].String();
                    if( f["isDirectory"].trueValue() ) cout << '/';
                    cout << '\n';
                }
                cout.flush();
            }
            return BSONObj();
        }

        BSONObj cd(const BSONObj& args, void* data) {
#if defined(_WIN32)
            std::wstring dir = toWideString( args.firstElement().String().c_str() );
            if( SetCurrentDirectory(dir.c_str()) )
                return BSONObj();
#else
            string dir = args.firstElement().String();
            /*            if( chdir(dir.c_str) ) == 0 )
                            return BSONObj();
                            */
            if( 1 ) return BSON(""<<"implementation not done for posix");
#endif
            return BSON( "" << "change directory failed" );
        }

        BSONObj pwd(const BSONObj&, void* data) {
            boost::filesystem::path p = boost::filesystem::current_path();
            return BSON( "" << p.string() );
        }

        BSONObj hostname(const BSONObj&, void* data) {
            return BSON( "" << getHostName() );
        }

        static BSONElement oneArg(const BSONObj& args) {
            uassert( 12597 , "need to specify 1 argument" , args.nFields() == 1 );
            return args.firstElement();
        }

        const int CANT_OPEN_FILE = 13300;

        BSONObj cat(const BSONObj& args, void* data) {
            BSONElement e = oneArg(args);
            stringstream ss;
            ifstream f(e.valuestrsafe());
            uassert(CANT_OPEN_FILE, "couldn't open file", f.is_open() );

            streamsize sz = 0;
            while( 1 ) {
                char ch = 0;
                // slow...maybe change one day
                f.get(ch);
                if( ch == 0 ) break;
                ss << ch;
                sz += 1;
                uassert(13301, "cat() : file to big to load as a variable", sz < 1024 * 1024 * 16);
            }
            return BSON( "" << ss.str() );
        }

        BSONObj md5sumFile(const BSONObj& args, void* data) {
            BSONElement e = oneArg(args);
            stringstream ss;
            FILE* f = fopen(e.valuestrsafe(), "rb");
            uassert(CANT_OPEN_FILE, "couldn't open file", f );

            md5digest d;
            md5_state_t st;
            md5_init(&st);

            enum {BUFLEN = 4*1024};
            char buffer[BUFLEN];
            int bytes_read;
            while( (bytes_read = fread(buffer, 1, BUFLEN, f)) ) {
                md5_append( &st , (const md5_byte_t*)(buffer) , bytes_read );
            }

            md5_finish(&st, d);
            return BSON( "" << digestToString( d ) );
        }

        BSONObj mkdir(const BSONObj& args, void* data) {
            boost::filesystem::create_directories(args.firstElement().String());
            return BSON( "" << true );
        }

        BSONObj removeFile(const BSONObj& args, void* data) {
            BSONElement e = oneArg(args);
            bool found = false;

            boost::filesystem::path root( e.valuestrsafe() );
            if ( boost::filesystem::exists( root ) ) {
                found = true;
                boost::filesystem::remove_all( root );
            }

            BSONObjBuilder b;
            b.appendBool( "removed" , found );
            return b.obj();
        }

        /**
         * @param args - [ name, byte index ]
         * In this initial implementation, all bits in the specified byte are flipped.
         */
        BSONObj fuzzFile(const BSONObj& args, void* data) {
            uassert( 13619, "fuzzFile takes 2 arguments", args.nFields() == 2 );
            shared_ptr< File > f( new File() );
            f->open( args.getStringField( "0" ) );
            uassert( 13620, "couldn't open file to fuzz", !f->bad() && f->is_open() );

            char c;
            f->read( args.getIntField( "1" ), &c, 1 );
            c = ~c;
            f->write( args.getIntField( "1" ), &c, 1 );

            return undefined_;
            // f close is implicit
        }

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

        class ProgramRunner {
            vector<string> argv_;
            int port_;
            int pipe_;
            pid_t pid_;
        public:
            pid_t pid() const { return pid_; }
            int port() const { return port_; }

            boost::filesystem::path find(string prog) {
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
                try {
                    if( theScope->type("_path") == String ) {
                        string path = theScope->getString("_path");
                        if( !path.empty() ) {
                            boost::filesystem::path t = boost::filesystem::path(path) / p;
                            if( boost::filesystem::exists(t) ) return t;
                        }
                    }
                }
                catch(...) { }
                {
                    boost::filesystem::path t = boost::filesystem::initial_path() / p;
                    if( boost::filesystem::exists(t)  ) return t;
                }
                return p; // not found; might find via system path
            }

            ProgramRunner( const BSONObj &args , bool isMongoProgram=true) {
                assert( !args.isEmpty() );

                string program( args.firstElement().valuestrsafe() );
                assert( !program.empty() );
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
                        assert( e.type() == mongo::String );
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
                    assert( port_ > 0 );
                }
                if ( port_ > 0 && dbs.count( port_ ) != 0 ) {
                    cerr << "count for port: " << port_ << " is not 0 is: " << dbs.count( port_ ) << endl;
                    assert( dbs.count( port_ ) == 0 );
                }
            }

            void start() {
                int pipeEnds[ 2 ];
                assert( pipe( pipeEnds ) != -1 );

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

            // Continue reading output
            void operator()() {
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
                        assert( lenToRead > 0 );
                        int ret = read( pipe_, (void *)start, lenToRead );
                        if( mongo::dbexitCalled )
                            break;
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
                        }
                        else {
                            assert( strlen( buf ) < bufSize );
                        }
                        start = buf + strlen( buf );
                    }
                }
                catch(...) {
                }
            }
            void launch_process(int child_stdout) {
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
                assert( pid_ != -1 );

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
        };

        //returns true if process exited
        bool wait_for_pid(pid_t pid, bool block=true, int* exit_code=NULL) {
#ifdef _WIN32
            assert(handles.count(pid));
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
            assert( a.nFields() == 1 );
            string path = a.firstElement().valuestrsafe();
            assert( !path.empty() );
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
            assert( a.nFields() == 2 );
            BSONObjIterator i( a );
            string from = i.next().str();
            string to = i.next().str();
            assert( !from.empty() );
            assert( !to.empty() );
            if ( boost::filesystem::exists( to ) )
                boost::filesystem::remove_all( to );
            boost::filesystem::create_directory( to );
            copyDir( from, to );
            return undefined_;
        }

        inline void kill_wrapper(pid_t pid, int sig, int port) {
#ifdef _WIN32
            if (sig == SIGKILL || port == 0) {
                assert( handles.count(pid) );
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
                    assert( x == 0 );
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
                assert( "Failed to terminate process" == 0 );
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
                assert( e.isNumber() );
                ret = int( e.number() );
            }
            return ret;
        }

        /** stopMongoProgram(port[, signal]) */
        BSONObj StopMongoProgram( const BSONObj &a, void* data ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
            uassert( 15853 , "stopMongo needs a number" , a.firstElement().isNumber() );
            int port = int( a.firstElement().number() );
            int code = killDb( port, 0, getSignal( a ) );
            cout << "shell: stopped mongo program on port " << port << endl;
            return BSON( "" << (double)code );
        }

        BSONObj StopMongoProgramByPid( const BSONObj &a, void* data ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
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
#else // ndef MONGO_SAFE_SHELL
        void KillMongoProgramInstances() {}
#endif

        MongoProgramScope::~MongoProgramScope() {
            DESTRUCTOR_GUARD(
                KillMongoProgramInstances();
                ClearRawMongoProgramOutput( BSONObj(), 0 );
            )
        }

        unsigned _randomSeed;

        BSONObj JSSrand( const BSONObj &a, void* data ) {
            uassert( 12518, "srand requires a single numeric argument",
                     a.nFields() == 1 && a.firstElement().isNumber() );
            _randomSeed = (unsigned)a.firstElement().numberLong(); // grab least significant digits
            return undefined_;
        }

        BSONObj JSRand( const BSONObj &a, void* data ) {
            uassert( 12519, "rand accepts no arguments", a.nFields() == 0 );
            unsigned r;
#if !defined(_WIN32)
            r = rand_r( &_randomSeed );
#else
            r = rand(); // seed not used in this case
#endif
            return BSON( "" << double( r ) / ( double( RAND_MAX ) + 1 ) );
        }

        BSONObj isWindows(const BSONObj& a, void* data) {
            uassert( 13006, "isWindows accepts no arguments", a.nFields() == 0 );
#ifdef _WIN32
            return BSON( "" << true );
#else
            return BSON( "" << false );
#endif
        }

        const char* getUserDir() {
#ifdef _WIN32
            return getenv( "USERPROFILE" );
#else
            return getenv( "HOME" );
#endif
        }
        BSONObj getHostName(const BSONObj& a, void* data) {
            uassert( 13411, "getHostName accepts no arguments", a.nFields() == 0 );
            char buf[260]; // HOST_NAME_MAX is usually 255
            assert(gethostname(buf, 260) == 0);
            buf[259] = '\0';
            return BSON("" << buf);

        }

        void installShellUtils( Scope& scope ) {
            theScope = &scope;
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "_srand" , JSSrand );
            scope.injectNative( "_rand" , JSRand );
            scope.injectNative( "_isWindows" , isWindows );

#ifndef MONGO_SAFE_SHELL
            //can't launch programs
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

            scope.injectNative( "getHostName" , getHostName );
            scope.injectNative( "removeFile" , removeFile );
            scope.injectNative( "fuzzFile" , fuzzFile );
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "ls" , ls );
            scope.injectNative( "pwd", pwd );
            scope.injectNative( "cd", cd );
            scope.injectNative( "cat", cat );
            scope.injectNative( "hostname", hostname);
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "copyDbpath", CopyDbpath );
            scope.injectNative( "md5sumFile", md5sumFile );
            scope.injectNative( "mkdir" , mkdir );
#endif
        }

        void initScope( Scope &scope ) {
            scope.externalSetup();
            mongo::shellUtils::installShellUtils( scope );
            scope.execSetup(JSFiles::servers);

            if ( !_dbConnect.empty() ) {
                uassert( 12513, "connect failed", scope.exec( _dbConnect , "(connect)" , false , true , false ) );
                if ( !_dbAuth.empty() ) {
                    installGlobalUtils( scope );
                    uassert( 12514, "login failed", scope.exec( _dbAuth , "(auth)" , true , true , false ) );
                }
            }
        }

        //   connstr, myuris
        map< string, set<string> > _allMyUris;
        mongo::mutex _allMyUrisMutex("_allMyUrisMutex");
        bool _nokillop = false;
        void onConnect( DBClientWithCommands &c ) {
            latestConn = &c;
            if ( _nokillop ) {
                return;
            }
            BSONObj info;
            if ( c.runCommand( "admin", BSON( "whatsmyuri" << 1 ), info ) ) {
                string connstr = dynamic_cast<DBClientBase&>(c).getServerAddress();
                mongo::mutex::scoped_lock lk( _allMyUrisMutex );
                _allMyUris[connstr].insert(info[ "you" ].str());
            }
        }
    }
}
