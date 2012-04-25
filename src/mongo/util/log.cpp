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

#include <boost/filesystem/operations.hpp>

namespace mongo {

    int logLevel = 0;
    int tlogLevel = 0;
    mongo::mutex Logstream::mutex("Logstream");
    int Logstream::doneSetup = Logstream::magicNumber();

    const char *default_getcurns() { return ""; }
    const char * (*getcurns)() = default_getcurns;

    Nullstream nullstream;
    vector<Tee*>* Logstream::globalTees = 0;

    thread_specific_ptr<Logstream> Logstream::tsp;

    Nullstream& tlog( int level ) {
        if ( !debug && level > tlogLevel )
            return nullstream;
        if ( level > logLevel )
            return nullstream;
        return Logstream::get().prolog();
    }

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
            bool isreg = boost::filesystem::is_regular(lp);

            if ( exists ) {
                if ( isdir ) {
                    cout << "logpath [" << lp << "] should be a filename, not a directory" << endl;
                    
                    dbexit( EXIT_BADOPTIONS );
                    verify( 0 );
                }

                if ( ! append ) {
                    // only attempt rename if log is regular file
                    if ( isreg ) {
                        stringstream ss;
                        ss << lp << "." << terseCurrentTime( false );
                        string s = ss.str();

                        if ( ! rename( lp.c_str() , s.c_str() ) ) {
                            cout << "log file [" << lp << "] exists; copied to temporary file [" << s << "]" << endl;
                        } else {
                            cout << "log file [" << lp << "] exists and couldn't make backup; run with --logappend or manually remove file (" << strerror(errno) << ")" << endl;
                            
                            dbexit( EXIT_BADOPTIONS );
                            verify( 0 );
                        }
                    }
                }
            }
            // test path
            FILE * test = fopen( lp.c_str() , _append ? "a" : "w" );
            if ( ! test ) {
                cout << "can't open [" << lp << "] for log file: " << errnoWithDescription() << endl;
                dbexit( EXIT_BADOPTIONS );
                verify( 0 );
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
                verify( 0 );
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

    string errnoWithDescription(int x) {
#if defined(_WIN32)
        if( x < 0 ) 
            x = GetLastError();
#else
        if( x < 0 ) 
            x = errno;
#endif
        stringstream s;
        s << "errno:" << x << ' ';

#if defined(_WIN32)
        LPWSTR errorText = NULL;
        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM
            |FORMAT_MESSAGE_ALLOCATE_BUFFER
            |FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            x, 0,
            reinterpret_cast<LPWSTR>( &errorText ),  // output
            0, // minimum size for output buffer
            NULL);
        if( errorText ) {
            string x = toUtf8String(errorText);
            for( string::iterator i = x.begin(); i != x.end(); i++ ) {
                if( *i == '\n' || *i == '\r' )
                    break;
                s << *i;
            }
            LocalFree(errorText);
        }
        else
            s << strerror(x);
        /*
        DWORD n = FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, x,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR) &lpMsgBuf, 0, NULL);
        */
#else
        s << strerror(x);
#endif
        return s.str();
    }

    void Logstream::logLockless( const StringData& s ) {

        if ( s.size() == 0 )
            return;

        if ( doneSetup == 1717 ) {

#if defined(_WIN32)
            // fwrite() has a behavior problem in Windows when writing to the console
            //  when the console is in the UTF-8 code page: fwrite() sends a single
            //  byte and then the rest of the string.  If the first character is
            //  non-ASCII, the console then displays two UTF-8 replacement characters
            //  instead of the single UTF-8 character we want.  write() doesn't have
            //  this problem.
            int fd = fileno( logfile );
            if ( _isatty( fd ) ) {
                fflush( logfile );
                _write( fd, s.data(), s.size() );
                return;
            }
#else
            if ( isSyslog ) {
                syslog( LOG_INFO , "%s" , s.data() );
                return;
            }
#endif

            if (fwrite(s.data(), s.size(), 1, logfile)) {
                fflush(logfile);
            }
            else {
                int x = errno;
                cout << "Failed to write to logfile: " << errnoWithDescription(x) << endl;
            }
        }
        else {
            cout << s.data();
            cout.flush();
        }
    }

    void Logstream::flush(Tee *t) {
        const size_t MAX_LOG_LINE = 1024 * 10;

        // this ensures things are sane
        if ( doneSetup == 1717 ) {
            string msg = ss.str();
            
            string threadName = getThreadName();
            const char * type = logLevelToString(logLevel);

            size_t msgLen = msg.size();
            if ( msgLen > MAX_LOG_LINE )
                msgLen = MAX_LOG_LINE;

            const int spaceNeeded = (int)( msgLen + 64 /* for extra info */ + threadName.size());
            int bufSize = 128;
            while ( bufSize < spaceNeeded )
                bufSize += 128;

            BufBuilder b(bufSize);
            time_t_to_String( time(0) , b.grow(20) );
            if (!threadName.empty()) {
                b.appendChar( '[' );
                b.appendStr( threadName , false );
                b.appendChar( ']' );
                b.appendChar( ' ' );
            }

            for ( int i=0; i<indent; i++ )
                b.appendChar( '\t' );

            if ( type[0] ) {
                b.appendStr( type , false );
                b.appendStr( ": " , false );
            }

            if ( msg.size() > MAX_LOG_LINE ) {
                stringstream sss;
                sss << "warning: log line attempted (" << msg.size() / 1024 << "k) over max size(" << MAX_LOG_LINE / 1024 << "k)";
                sss << ", printing beginning and end ... ";
                b.appendStr( sss.str() );
                const char * xx = msg.c_str();
                b.appendBuf( xx , MAX_LOG_LINE / 3 );
                b.appendStr( " .......... " );
                b.appendStr( xx + msg.size() - ( MAX_LOG_LINE / 3 ) );
            }
            else {
                b.appendStr( msg );
            }

            string out( b.buf() , b.len() - 1);
            verify( b.len() < spaceNeeded );

            scoped_lock lk(mutex);

            if( t ) t->write(logLevel,out);
            if ( globalTees ) {
                for ( unsigned i=0; i<globalTees->size(); i++ )
                    (*globalTees)[i]->write(logLevel,out);
            }
#if defined(_WIN32)
            // fwrite() has a behavior problem in Windows when writing to the console
            //  when the console is in the UTF-8 code page: fwrite() sends a single
            //  byte and then the rest of the string.  If the first character is
            //  non-ASCII, the console then displays two UTF-8 replacement characters
            //  instead of the single UTF-8 character we want.  write() doesn't have
            //  this problem.
            int fd = fileno( logfile );
            if ( _isatty( fd ) ) {
                fflush( logfile );
                _write( fd, out.data(), out.size() );
            }
#else
            if ( isSyslog ) {
                syslog( logLevelToSysLogLevel(logLevel) , "%s" , out.data() );
            }
#endif
            else if ( fwrite( out.data(), out.size(), 1, logfile ) ) {
                fflush(logfile);
            }
            else {
                int x = errno;
                cout << "Failed to write to logfile: " << errnoWithDescription(x) << ": " << out << endl;
            }
#ifdef POSIX_FADV_DONTNEED
            // This only applies to pages that have already been flushed
            RARELY posix_fadvise(fileno(logfile), 0, 0, POSIX_FADV_DONTNEED);
#endif
        }
        _init();
    }
    
    void Logstream::removeGlobalTee( Tee * tee ) {
        if ( !globalTees ) {
            return;
        }
        for( std::vector<Tee*>::iterator i = globalTees->begin(); i != globalTees->end(); ++i ) {
            if ( *i == tee ) {
                globalTees->erase( i );
                return;
            }
        }
    }

    void Logstream::setLogFile(FILE* f) {
        scoped_lock lk(mutex);
        logfile = f;
    }
        
    Logstream& Logstream::get() {
        if ( StaticObserver::_destroyingStatics ) {
            cout << "Logstream::get called in uninitialized state" << endl;
        }
        Logstream *p = tsp.get();
        if( p == 0 )
            tsp.reset( p = new Logstream() );
        return *p;
    }

    /* note: can't use malloc herein - may be in signal handler.
             logLockless() likely does not comply and should still be fixed todo
             likewise class string?
    */
    void rawOut( const string &s ) {
        if( s.empty() ) return;

        char buf[64];
        time_t_to_String( time(0) , buf );
        /* truncate / don't show the year: */
        buf[19] = ' ';
        buf[20] = 0;

        Logstream::logLockless(buf);
        Logstream::logLockless(s);
        Logstream::logLockless("\n");
    }

}
