// @file log.h

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

#pragma once

#include <string.h>
#include <errno.h>
#include "../bson/util/builder.h"

#ifndef _WIN32
//#include <syslog.h>
#endif

namespace mongo {

    enum LogLevel {  LL_DEBUG , LL_INFO , LL_NOTICE , LL_WARNING , LL_ERROR , LL_SEVERE };
    
    inline const char * logLevelToString( LogLevel l ){
        switch ( l ){
        case LL_DEBUG:
        case LL_INFO: 
        case LL_NOTICE:
            return "";
        case LL_WARNING: 
            return "warning" ; 
        case LL_ERROR: 
            return "ERROR";
        case LL_SEVERE: 
            return "SEVERE";
        default:
            return "UNKNOWN";
        }
    }
    
    class LazyString {
    public:
        virtual ~LazyString() {}
        virtual string val() const = 0;
    };

    // Utility class for stringifying object only when val() called.
    template< class T >
    class LazyStringImpl : public LazyString {
    public:
        LazyStringImpl( const T &t ) : t_( t ) {}
        virtual string val() const { return t_.toString(); }
    private:
        const T& t_;
    };

    class Tee { 
    public:
        virtual ~Tee(){}
        virtual void write(LogLevel level , const string& str) = 0;
    };

    class Nullstream {
    public:
        virtual Nullstream& operator<< (Tee* tee) { 
            return *this;
        }
        virtual ~Nullstream() {}
        virtual Nullstream& operator<<(const char *) {
            return *this;
        }
        virtual Nullstream& operator<<(const string& ) {
            return *this;
        }
        virtual Nullstream& operator<<(const StringData& ) {
            return *this;
        }
        virtual Nullstream& operator<<(char *) {
            return *this;
        }
        virtual Nullstream& operator<<(char) {
            return *this;
        }
        virtual Nullstream& operator<<(int) {
            return *this;
        }
        virtual Nullstream& operator<<(ExitCode) {
            return *this;
        }
        virtual Nullstream& operator<<(unsigned long) {
            return *this;
        }
        virtual Nullstream& operator<<(long) {
            return *this;
        }
        virtual Nullstream& operator<<(unsigned) {
            return *this;
        }
        virtual Nullstream& operator<<(double) {
            return *this;
        }
        virtual Nullstream& operator<<(void *) {
            return *this;
        }
        virtual Nullstream& operator<<(const void *) {
            return *this;
        }
        virtual Nullstream& operator<<(long long) {
            return *this;
        }
        virtual Nullstream& operator<<(unsigned long long) {
            return *this;
        }
        virtual Nullstream& operator<<(bool) {
            return *this;
        }
        virtual Nullstream& operator<<(const LazyString&) {
            return *this;
        }
        template< class T >
        Nullstream& operator<<(T *t) {
            return operator<<( static_cast<void*>( t ) );
        }        
        template< class T >
        Nullstream& operator<<(const T *t) {
            return operator<<( static_cast<const void*>( t ) );
        }        
        template< class T >
        Nullstream& operator<<(const shared_ptr<T> p ){
            return *this;
        }
        template< class T >
        Nullstream& operator<<(const T &t) {
            return operator<<( static_cast<const LazyString&>( LazyStringImpl< T >( t ) ) );
        }
        virtual Nullstream& operator<< (ostream& ( *endl )(ostream&)) {
            return *this;
        }
        virtual Nullstream& operator<< (ios_base& (*hex)(ios_base&)) {
            return *this;
        }
        virtual void flush(Tee *t = 0) {}
    };
    extern Nullstream nullstream;
    
    class Logstream : public Nullstream {
        static mongo::mutex mutex;
        static int doneSetup;
        stringstream ss;
        LogLevel logLevel;
        static FILE* logfile;
        static boost::scoped_ptr<ostream> stream;
        static vector<Tee*> * globalTees;
    public:

        inline static void logLockless( const StringData& s );
        
        static void setLogFile(FILE* f){
            scoped_lock lk(mutex);
            logfile = f;
        }

        static int magicNumber(){
            return 1717;
        }

        static int getLogDesc() {
            int fd = -1;
            if (logfile != NULL)
                fd = fileno( logfile );
            return fd;
        }

        inline void flush(Tee *t = 0);
        
        inline Nullstream& setLogLevel(LogLevel l){
            logLevel = l;
            return *this;
        }

        /** note these are virtual */
        Logstream& operator<<(const char *x) { ss << x; return *this; }
        Logstream& operator<<(const string& x) { ss << x; return *this; }
        Logstream& operator<<(const StringData& x) { ss << x.data(); return *this; }
        Logstream& operator<<(char *x)       { ss << x; return *this; }
        Logstream& operator<<(char x)        { ss << x; return *this; }
        Logstream& operator<<(int x)         { ss << x; return *this; }
        Logstream& operator<<(ExitCode x)    { ss << x; return *this; }
        Logstream& operator<<(long x)          { ss << x; return *this; }
        Logstream& operator<<(unsigned long x) { ss << x; return *this; }
        Logstream& operator<<(unsigned x)      { ss << x; return *this; }
        Logstream& operator<<(double x)        { ss << x; return *this; }
        Logstream& operator<<(void *x)         { ss << x; return *this; }
        Logstream& operator<<(const void *x)   { ss << x; return *this; }
        Logstream& operator<<(long long x)     { ss << x; return *this; }
        Logstream& operator<<(unsigned long long x) { ss << x; return *this; }
        Logstream& operator<<(bool x)               { ss << x; return *this; }

        Logstream& operator<<(const LazyString& x) {
            ss << x.val();
            return *this;
        }
        Nullstream& operator<< (Tee* tee) { 
            ss << '\n';
            flush(tee);
            return *this;
        }
        Logstream& operator<< (ostream& ( *_endl )(ostream&)) {
            ss << '\n';
            flush(0);
            return *this;
        }
        Logstream& operator<< (ios_base& (*_hex)(ios_base&)) {
            ss << _hex;
            return *this;
        }

        template< class T >
        Nullstream& operator<<(const shared_ptr<T> p ){
            T * t = p.get();
            if ( ! t )
                *this << "null";
            else 
                *this << *t;
            return *this;
        }

        Logstream& prolog() {
            return *this;
        }
        
        void addGlobalTee( Tee * t ){
            if ( ! globalTees )
                globalTees = new vector<Tee*>();
            globalTees->push_back( t );
        }

    private:
        static thread_specific_ptr<Logstream> tsp;
        Logstream(){
            _init();
        }
        void _init(){
            ss.str("");
            logLevel = LL_INFO;
        }
    public:
        static Logstream& get() {
            Logstream *p = tsp.get();
            if( p == 0 )
                tsp.reset( p = new Logstream() );
            return *p;
        }
    };

    extern int logLevel;
    extern int tlogLevel;

    inline Nullstream& out( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
        return Logstream::get();
    }
    
    /* flush the log stream if the log level is 
       at the specified level or higher. */
    inline void logflush(int level = 0) { 
        if( level > logLevel )
            Logstream::get().flush(0);
    }

    /* without prolog */
    inline Nullstream& _log( int level = 0 ){
        if ( level > logLevel )
            return nullstream;
        return Logstream::get();
    }

    /** logging which we may not want during unit tests runs.
        set tlogLevel to -1 to suppress tlog() output in a test program. */
    inline Nullstream& tlog( int level = 0 ) {
        if ( level > tlogLevel || level > logLevel )
            return nullstream;
        return Logstream::get().prolog();
    }

    inline Nullstream& log( int level ) {
        if ( level > logLevel )
            return nullstream;
        return Logstream::get().prolog();
    }

#define MONGO_LOG(level) if ( logLevel >= (level) ) log( level )
#define LOG MONGO_LOG

    inline Nullstream& log( LogLevel l ) {
        return Logstream::get().prolog().setLogLevel( l );
    }


    inline Nullstream& log() {
        return Logstream::get().prolog();
    }
    
    inline Nullstream& error() {
        return log( LL_ERROR );
    }

    inline Nullstream& warning() {
        return log( LL_WARNING );
    }

    /* default impl returns "" -- mongod overrides */
    extern const char * (*getcurns)();

    inline Nullstream& problem( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
        Logstream& l = Logstream::get().prolog();
        l << ' ' << getcurns() << ' ';
        return l;
    }

    /**
       log to a file rather than stdout
       defined in assert_util.cpp 
     */
    void initLogging( const string& logpath , bool append );
    void rotateLogs( int signal = 0 );

    std::string toUtf8String(const std::wstring& wide);

    inline string errnoWithDescription(int x = errno) {
        stringstream s;
        s << "errno:" << x << ' ';

#if defined(_WIN32)
        LPTSTR errorText = NULL;
        FormatMessage(
            FORMAT_MESSAGE_FROM_SYSTEM
            |FORMAT_MESSAGE_ALLOCATE_BUFFER
            |FORMAT_MESSAGE_IGNORE_INSERTS,  
            NULL,
            x, 0,
            (LPTSTR) &errorText,  // output
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

    /** output the error # and error message with prefix.  
        handy for use as parm in uassert/massert.
        */
    string errnoWithPrefix( const char * prefix );

    void Logstream::logLockless( const StringData& s ){
        if ( doneSetup == 1717 ){
            if(fwrite(s.data(), s.size(), 1, logfile)){
                fflush(logfile);
            }else{
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
        // this ensures things are sane
        if ( doneSetup == 1717 ) {
            string msg = ss.str();
            string threadName = getThreadName();
            const char * type = logLevelToString(logLevel);

            int spaceNeeded = msg.size() + 64 + threadName.size();
            int bufSize = 128;
            while ( bufSize < spaceNeeded )
                bufSize += 128;

            BufBuilder b(bufSize);
            time_t_to_String( time(0) , b.grow(20) );
            if (!threadName.empty()){
                b.appendChar( '[' );
                b.appendStr( threadName , false );
                b.appendChar( ']' );
                b.appendChar( ' ' );
            }
            if ( type[0] ){
                b.appendStr( type , false );
                b.appendStr( ": " , false );
            }
            b.appendStr( msg );

            string out( b.buf() , b.len() - 1);

            scoped_lock lk(mutex);

            if( t ) t->write(logLevel,out);
            if ( globalTees ){
                for ( unsigned i=0; i<globalTees->size(); i++ )
                    (*globalTees)[i]->write(logLevel,out);
            }

#ifndef _WIN32
            //syslog( LOG_INFO , "%s" , cc );
#endif
            if(fwrite(out.data(), out.size(), 1, logfile)){
                fflush(logfile);
            }else{
                int x = errno;
                cout << "Failed to write to logfile: " << errnoWithDescription(x) << ": " << out << endl;
            }
        }
        _init();
    }

} // namespace mongo
