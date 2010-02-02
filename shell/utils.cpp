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

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "../client/dbclient.h"
#include "../util/processinfo.h"
#include "../util/md5.hpp"
#include "utils.h"

namespace mongo {

    namespace shellUtils {
        
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
            sleepms( ms );
            return undefined_;
        }

        BSONObj listFiles(const BSONObj& args){
            uassert( "need to specify 1 argument to listFiles" , args.nFields() == 1 );
            
            BSONObjBuilder lst;
            
            string rootname = args.firstElement().valuestrsafe();
            path root( rootname );
            
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
            uassert( "processinfo not supported" , pi.supported() );
            
            BSONObjBuilder e;
            e.append( "virtual" , pi.getVirtualMemorySize() );
            e.append( "resident" , pi.getResidentSize() );
            
            BSONObjBuilder b;
            b.append( "ret" , e.obj() );
            
            return b.obj();
        }

        BSONObj JSVersion( const BSONObj& args ){
            cout << "version: " << versionString << endl;
            if ( strstr( versionString , "+" ) )
                printGitVersion();
            return BSONObj();
        }
        
#ifndef _WIN32
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

        map< int, pair< pid_t, int > > dbs;
        map< pid_t, int > shells;
        
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
                massert( "couldn't find " + programPath.native_file_string(), boost::filesystem::exists( programPath ) );
                
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
                pid_ = fork();
                assert( pid_ != -1 );
                
                if ( pid_ == 0 ) {
                    
                    assert( dup2( pipeEnds[ 1 ], STDOUT_FILENO ) != -1 );
                    assert( dup2( pipeEnds[ 1 ], STDERR_FILENO ) != -1 );
                    execvp( argv_[ 0 ], argv_ );
                    massert( "Unable to start program" , 0 );
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
        };
        
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
            int temp;
            waitpid( r.pid() , &temp , 0 );
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

#else
        MongoProgramScope::~MongoProgramScope() {}
        void KillMongoProgramInstances() {}        
#endif

        BSONObj jsmd5( const BSONObj &a ){
            uassert( "js md5 needs a string" , a.firstElement().type() == String );
            const char * s = a.firstElement().valuestrsafe();
            
            md5digest d;
            md5_state_t st;
            md5_init(&st);
            md5_append( &st , (const md5_byte_t*)s , strlen( s ) );
            md5_finish(&st, d);
            
            return BSON( "" << digestToString( d ) );
        }
        
        void installShellUtils( Scope& scope ){
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "sleep" , JSSleep );
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "version" , JSVersion );
            scope.injectNative( "hex_md5" , jsmd5 );
#if !defined(_WIN32)
            scope.injectNative( "_startMongoProgram", StartMongoProgram );
            scope.injectNative( "runMongoProgram", RunMongoProgram );
            scope.injectNative( "stopMongod", StopMongoProgram );
            scope.injectNative( "stopMongoProgram", StopMongoProgram );        
            scope.injectNative( "stopMongoProgramByPid", StopMongoProgramByPid );        
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "rawMongoProgramOutput", RawMongoProgramOutput );
#endif
        }
        
    }
}
