/** @file log.cpp
 */

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
#include <cmath>
#include "time_support.h"
using namespace std;

#ifdef _WIN32
# include <io.h>
#else
# include <cxxabi.h>
# include <sys/file.h>
#endif

//#include "../db/jsobj.h"

namespace mongo {

    Nullstream nullstream;
    vector<Tee*>* Logstream::globalTees = 0;

    thread_specific_ptr<Logstream> Logstream::tsp;

    class LoggingManager {
    public:
        LoggingManager()
            : _enabled(0) , _file(0) {
        }

        void start( const string& lp , bool append ) {
            uassert( 10268 ,  "LoggingManager already started" , ! _enabled );
            _append = append;

            bool exists = boost::filesystem::exists(lp);

            // test path
            FILE * test = fopen( lp.c_str() , _append ? "a" : "w" );
            if ( ! test ) {
                if (boost::filesystem::is_directory(lp)) {
                    cout << "logpath [" << lp << "] should be a file name not a directory" << endl;
                }
                else {
                    cout << "can't open [" << lp << "] for log file: " << errnoWithDescription() << endl;
                }
                dbexit( EXIT_BADOPTIONS );
                assert( 0 );
            }

            if (append && exists){
                // two blank lines before and after
                const string msg = "\n\n***** SERVER RESTARTED *****\n\n\n";
                massert(14036, errnoWithPrefix("couldn't write to log file"),
                        fwrite(msg.data(), 1, msg.size(), test) == msg.size());
            }

            fclose( test );

            _path = lp;
            _enabled = 1;
            rotate();
        }

        void rotate() {
            if ( ! _enabled ) {
                cout << "LoggingManager not enabled" << endl;
                return;
            }

            if ( _file ) {
#ifdef _WIN32
                cout << "log rotation net yet supported on windows" << endl;
                return;
#else

#ifdef POSIX_FADV_DONTNEED
                posix_fadvise(fileno(_file), 0, 0, POSIX_FADV_DONTNEED);
#endif

                struct tm t;
                localtime_r( &_opened , &t );

                stringstream ss;
                ss << _path << "." << terseCurrentTime(false);
                string s = ss.str();
                rename( _path.c_str() , s.c_str() );
#endif
            }


            FILE* tmp = freopen(_path.c_str(), (_append ? "a" : "w"), stdout);
            if (!tmp) {
                cerr << "can't open: " << _path.c_str() << " for log file" << endl;
                dbexit( EXIT_BADOPTIONS );
                assert(0);
            }

#ifdef _WIN32 // windows has these functions it just gives them a funny name
# define dup2 _dup2
# define fileno _fileno
#endif
            // redirect stderr to log file
            dup2(fileno(tmp), 2);

            Logstream::setLogFile(tmp); // after this point no thread will be using old file

#if 0 // enable to test redirection
            cout << "written to cout" << endl;
            cerr << "written to cerr" << endl;
            log() << "written to log()" << endl;
#endif

            _file = tmp;
            _opened = time(0);
        }

    private:

        bool _enabled;
        string _path;
        bool _append;

        FILE * _file;
        time_t _opened;

    } loggingManager;

    void initLogging( const string& lp , bool append ) {
        cout << "all output going to: " << lp << endl;
        loggingManager.start( lp , append );
    }

    void rotateLogs( int signal ) {
        loggingManager.rotate();
    }

    // done *before* static initialization
    FILE* Logstream::logfile = stdout;
    bool Logstream::isSyslog = false;

}
