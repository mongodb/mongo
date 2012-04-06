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

#include "shell_utils.h"
#include "shell_utils_extended.h"
#include "shell_utils_launcher.h"
#include "../util/processinfo.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    DBClientWithCommands *latestConn = 0;

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

        void installShellUtils( Scope& scope ) {
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "_srand" , JSSrand );
            scope.injectNative( "_rand" , JSRand );
            scope.injectNative( "_isWindows" , isWindows );

#ifndef MONGO_SAFE_SHELL
            //can't launch programs
            installShellUtilsLauncher( scope );
            installShellUtilsExtended( scope );
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
