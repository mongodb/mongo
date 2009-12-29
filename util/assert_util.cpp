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

#include "stdafx.h"
#include "assert_util.h"
#include "assert.h"
#include "file.h"

namespace mongo {

	string getDbContext();
	
	Assertion lastAssert[4];
	
	/* "warning" assert -- safe to continue, so we don't throw exception. */
    void wasserted(const char *msg, const char *file, unsigned line) {
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "wassertion failure");
        lastAssert[1].set(msg, getDbContext().c_str(), file, line);
    }

    void asserted(const char *msg, const char *file, unsigned line) {
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "assertion failure");
        lastAssert[0].set(msg, getDbContext().c_str(), file, line);
        stringstream temp;
        temp << "assertion " << file << ":" << line;
        AssertionException e;
        e.msg = temp.str();
        breakpoint();
        throw e;
    }

    void uassert_nothrow(const char *msg) {
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(0,msg);
    }

    int uacount = 0;
    void uasserted(int msgid, const char *msg) {
        if ( ++uacount < 100 )
            log() << "User Exception " << msgid << ":" << msg << endl;
        else
            RARELY log() << "User Exception " << msg << endl;
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msgid,msg);
        throw UserException(msgid, msg);
    }

    void msgasserted(int msgid, const char *msg) {
        log() << "Assertion: " << msgid << ":" << msg << endl;
        lastAssert[2].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msgid,msg && *msg ? msg : "massert failure");
        breakpoint();
        printStackTrace(); // TEMP?? should we get rid of this?  TODO
        throw MsgAssertionException(msgid, msg);
    }

    boost::mutex *Assertion::_mutex = new boost::mutex();

    string Assertion::toString() {
        if( _mutex == 0 )
            return "";

        boostlock lk(*_mutex);

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

            // test path
            FILE * test = fopen( lp.c_str() , _append ? "a" : "w" );
            if ( ! test ){
                cout << "can't open [" << lp << "] for log file" << endl;
                dbexit( EXIT_BADOPTIONS );
                assert( 0 );
            }
            
            _path = lp;
            _append = append;
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
}

