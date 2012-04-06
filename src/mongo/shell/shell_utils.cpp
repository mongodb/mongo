// mongo/shell/shell_utils.cpp
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

#include <boost/filesystem/convenience.hpp>

#include <map>
#include <fstream>
#include <vector>

#include "mongo/util/net/sock.h"

#include "shell_utils.h"
#include "shell_launcher.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "../util/file.h"
#include "mongo/client/dbclientinterface.h"

namespace mongo {

    DBClientWithCommands *latestConn = 0;

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
            if( chdir( dir.c_str() ) == 0 )
                return BSONObj();
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

        BSONElement oneArg(const BSONObj& args) {
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
#endif // #ifndef MONGO_SAFE_SHELL


#if !defined(_WIN32)
        ThreadLocalValue< unsigned int > _randomSeed;
#endif

        BSONObj JSSrand( const BSONObj &a, void* data ) {
            uassert( 12518, "srand requires a single numeric argument",
                     a.nFields() == 1 && a.firstElement().isNumber() );
#if !defined(_WIN32)
            _randomSeed.set( static_cast< unsigned int >( a.firstElement().numberLong() ) ); // grab least significant digits
#else
            srand( static_cast< unsigned int >( a.firstElement().numberLong() ) );
#endif
            return undefined_;
        }

        BSONObj JSRand( const BSONObj &a, void* data ) {
            uassert( 12519, "rand accepts no arguments", a.nFields() == 0 );
            unsigned r;
#if !defined(_WIN32)
            r = rand_r( &_randomSeed.getRef() );
#else
            r = rand();
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
            verify(gethostname(buf, 260) == 0);
            buf[259] = '\0';
            return BSON("" << buf);

        }

        void installShellUtils( Scope& scope ) {
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "_srand" , JSSrand );
            scope.injectNative( "_rand" , JSRand );
            scope.injectNative( "_isWindows" , isWindows );

#ifndef MONGO_SAFE_SHELL
            //can't launch programs
            installShellLauncher( scope );

            scope.injectNative( "getHostName" , getHostName );
            scope.injectNative( "removeFile" , removeFile );
            scope.injectNative( "fuzzFile" , fuzzFile );
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "ls" , ls );
            scope.injectNative( "pwd", pwd );
            scope.injectNative( "cd", cd );
            scope.injectNative( "cat", cat );
            scope.injectNative( "hostname", hostname);
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
