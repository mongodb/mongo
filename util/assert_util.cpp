// assert_util.cpp

/*    Copyright 2009 10gen Inc.
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
#include "assert_util.h"
#include "assert.h"
#include "file.h"
#include <cmath>
using namespace std;

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

//#include "../bson/bson.h"
#include "../db/jsobj.h"

namespace mongo {

    AssertionCount assertionCount;
    
    AssertionCount::AssertionCount()
        : regular(0),warning(0),msg(0),user(0),rollovers(0){
    }

    void AssertionCount::rollover(){
        rollovers++;
        regular = 0;
        warning = 0;
        msg = 0;
        user = 0;
    }

    void AssertionCount::condrollover( int newvalue ){
        static int max = (int)pow( 2.0 , 30 );
        if ( newvalue >= max )
            rollover();
    }

    void ExceptionInfo::append( BSONObjBuilder& b , const char * m , const char * c ) const {
        if ( msg.empty() )
            b.append( m , "unknown assertion" );
        else
            b.append( m , msg );
        
        if ( code )
            b.append( c , code );
    }

    
	string getDbContext();
	
	Assertion lastAssert[4];
	
	/* "warning" assert -- safe to continue, so we don't throw exception. */
    void wasserted(const char *msg, const char *file, unsigned line) {
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "wassertion failure");
        lastAssert[1].set(msg, getDbContext().c_str(), file, line);
        assertionCount.condrollover( ++assertionCount.warning );
    }

    void asserted(const char *msg, const char *file, unsigned line) {
        assertionCount.condrollover( ++assertionCount.regular );
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "assertion failure");
        lastAssert[0].set(msg, getDbContext().c_str(), file, line);
        stringstream temp;
        temp << "assertion " << file << ":" << line;
        AssertionException e(temp.str(),0);
        breakpoint();
        throw e;
    }

    void uassert_nothrow(const char *msg) {
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(0,msg);
    }

    void uasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.user );
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msgid,msg);
        throw UserException(msgid, msg);
    }

    void msgasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        tlog() << "Assertion: " << msgid << ":" << msg << endl;
        lastAssert[2].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msgid,msg && *msg ? msg : "massert failure");
        breakpoint();
        printStackTrace();
        throw MsgAssertionException(msgid, msg);
    }

    void msgassertedNoTrace(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        log() << "Assertion: " << msgid << ":" << msg << endl;
        lastAssert[2].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msgid,msg && *msg ? msg : "massert failure");
        throw MsgAssertionException(msgid, msg);
    }

    void streamNotGood( int code , string msg , std::ios& myios ){
        stringstream ss;
        // errno might not work on all systems for streams
        // if it doesn't for a system should deal with here
        ss << msg << " stream invalid: " << errnoWithDescription();
        throw UserException( code , ss.str() );
    }
    
    mongo::mutex *Assertion::_mutex = new mongo::mutex("Assertion");

    string Assertion::toString() {
        if( _mutex == 0 )
            return "";

        scoped_lock lk(*_mutex);

        if ( !isSet() )
            return "";

        stringstream ss;
        ss << msg << '\n';
        if ( *context )
            ss << context << '\n';
        if ( *file )
            ss << file << ' ' << line << '\n';
        return ss.str();
    }	

    
    class LoggingManager {
    public:
        LoggingManager()
            : _enabled(0) , _file(0) {
        }
        
        void start( const string& lp , bool append ){
            uassert( 10268 ,  "LoggingManager already started" , ! _enabled );
            _append = append;

            // test path
            FILE * test = fopen( lp.c_str() , _append ? "a" : "w" );
            if ( ! test ){
                cout << "can't open [" << lp << "] for log file" << endl;
                dbexit( EXIT_BADOPTIONS );
                assert( 0 );
            }
            fclose( test );
            
            _path = lp;
            _enabled = 1;
            rotate();
        }
        
        void rotate(){
            if ( ! _enabled ){
                cout << "LoggingManager not enabled" << endl;
                return;
            }

            if ( _file ){
#ifdef _WIN32
                cout << "log rotation doesn't work on windows" << endl;
                return;
#else
                struct tm t;
                localtime_r( &_opened , &t );
                
                stringstream ss;
                ss << _path << "." << ( 1900 + t.tm_year ) << "-" << t.tm_mon << "-" << t.tm_mday 
                   << "_" << t.tm_hour << "-" << t.tm_min << "-" << t.tm_sec;
                string s = ss.str();
                rename( _path.c_str() , s.c_str() );
#endif
            }
        
            _file = freopen( _path.c_str() , _append ? "a" : "w"  , stdout );
            if ( ! _file ){
                cerr << "can't open: " << _path.c_str() << " for log file" << endl;
                dbexit( EXIT_BADOPTIONS );
                assert(0);
            }
            _opened = time(0);
        }
        
    private:
        
        bool _enabled;
        string _path;
        bool _append;
        
        FILE * _file;
        time_t _opened;
        
    } loggingManager;

    void initLogging( const string& lp , bool append ){
        cout << "all output going to: " << lp << endl;
        loggingManager.start( lp , append );
    }

    void rotateLogs( int signal ){
        loggingManager.rotate();
    }

    string errnoWithPrefix( const char * prefix ){
        stringstream ss;
        if ( prefix )
            ss << prefix << ": ";
        ss << errnoWithDescription();
        return ss.str();
    }


    string demangleName( const type_info& typeinfo ){
#ifdef _WIN32
        return typeinfo.name();
#else
        int status;
        
        char * niceName = abi::__cxa_demangle(typeinfo.name(), 0, 0, &status);
        if ( ! niceName )
            return typeinfo.name();
        
        string s = niceName;
        free(niceName);
        return s;
#endif
    }


}

