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
# include <fcntl.h>
#else
# include <cxxabi.h>
# include <sys/file.h>
#endif

#ifdef _WIN32
# define dup2   _dup2       // Microsoft headers use ISO C names
# define fileno _fileno
#endif

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
            bool isdir = boost::filesystem::is_directory(lp);
 
            if ( exists ) {
                if ( isdir ) {
                    cout << "logpath [" << lp << "] should be a file name not a directory" << endl;
                    
                    dbexit( EXIT_BADOPTIONS );
                    assert( 0 );
                }

                if ( ! append ) {
                    stringstream ss;
                    ss << lp << "." << terseCurrentTime( false );
                    string s = ss.str();

                    if ( ! rename( lp.c_str() , s.c_str() ) ) {
                        cout << "log file [" << lp << "] exists; copied to temporary file [" << s << "]" << endl;
                    } else {
                        cout << "log file [" << lp << "] exists and couldn't make backup; run with --logappend or manually remove file" << endl;
                        
                        dbexit( EXIT_BADOPTIONS );
                        assert( 0 );
                    }
                }
            }
            // test path
            FILE * test = fopen( lp.c_str() , _append ? "a" : "w" );
            if ( ! test ) {
                cout << "can't open [" << lp << "] for log file: " << errnoWithDescription() << endl;
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

#ifdef POSIX_FADV_DONTNEED
                posix_fadvise(fileno(_file), 0, 0, POSIX_FADV_DONTNEED);
#endif

                // Rename the (open) existing log file to a timestamped name
                stringstream ss;
                ss << _path << "." << terseCurrentTime( false );
                string s = ss.str();
                rename( _path.c_str() , s.c_str() );
            }

            FILE* tmp = 0;  // The new file using the original logpath name

#if _WIN32
            // We rename an open log file (above, on next rotation) and the trick to getting Windows to do that is
            // to open the file with FILE_SHARE_DELETE.  So, we can't use the freopen() call that non-Windows
            // versions use because it would open the file without the FILE_SHARE_DELETE flag we need.
            //
            HANDLE newFileHandle = CreateFileA(
                    _path.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL
            );
            if ( INVALID_HANDLE_VALUE != newFileHandle ) {
                int newFileDescriptor = _open_osfhandle( reinterpret_cast<intptr_t>(newFileHandle), _O_APPEND );
                tmp = _fdopen( newFileDescriptor, _append ? "a" : "w" );
            }
#else
            tmp = freopen(_path.c_str(), _append ? "a" : "w", stdout);
#endif
            if ( !tmp ) {
                cerr << "can't open: " << _path.c_str() << " for log file" << endl;
                dbexit( EXIT_BADOPTIONS );
                assert( 0 );
            }

            // redirect stdout and stderr to log file
            dup2( fileno( tmp ), 1 );   // stdout
            dup2( fileno( tmp ), 2 );   // stderr

            Logstream::setLogFile(tmp); // after this point no thread will be using old file

#if _WIN32
            if ( _file )
                fclose( _file );  // In Windows, we still have the old file open, close it now
#endif

#if 0 // enable to test redirection
            cout << "written to cout" << endl;
            cerr << "written to cerr" << endl;
            log() << "written to log()" << endl;
#endif

            _file = tmp;    // Save new file for next rotation
        }

    private:
        bool _enabled;
        string _path;
        bool _append;
        FILE * _file;

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
