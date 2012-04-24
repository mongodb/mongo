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
#include <sstream>
#include <errno.h>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/tss.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit_code.h"

#ifndef _WIN32
#include <syslog.h>
#endif

namespace mongo {

    enum ExitCode;

    enum LogLevel {  LL_DEBUG , LL_INFO , LL_NOTICE , LL_WARNING , LL_ERROR , LL_SEVERE };

    inline const char * logLevelToString( LogLevel l ) {
        switch ( l ) {
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
    
#ifndef _WIN32
    inline const int logLevelToSysLogLevel( LogLevel l) {
        switch ( l ) {
        case LL_DEBUG:
            return LOG_DEBUG;
        case LL_INFO:
            return LOG_INFO;
        case LL_NOTICE:
            return LOG_NOTICE;
        case LL_WARNING:
            return LOG_WARNING;
        case LL_ERROR:
            return LOG_ERR;
        case LL_SEVERE:
            return LOG_CRIT;
        default:
            return LL_INFO;
        }
    }
#endif

    class LabeledLevel {
    public:

	LabeledLevel( int level ) : _level( level ) {}
	LabeledLevel( const char* label, int level ) : _label( label ), _level( level ) {}
	LabeledLevel( const string& label, int level ) : _label( label ), _level( level ) {}

	LabeledLevel operator+( int i ) const {
	    return LabeledLevel( _label, _level + i );
	}

	LabeledLevel operator+( const char* label ) const {
	    if( _label == "" )
		return LabeledLevel( label, _level );
	    return LabeledLevel( _label + string("::") + label, _level );
	}

	LabeledLevel operator+( string& label ) const {
	    return LabeledLevel( _label + string("::") + label, _level );
	}

	LabeledLevel operator-( int i ) const {
	    return LabeledLevel( _label, _level - i );
	}

	const string& getLabel() const { return _label; }
	int getLevel() const { return _level; }

    private:
	string _label;
	int _level;
    };

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
        virtual ~Tee() {}
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
        virtual Nullstream& operator<<(unsigned short) {
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
        Nullstream& operator<<(const boost::shared_ptr<T> &p ) {
            T * t = p.get();
            if ( ! t )
                *this << "null";
            else
                *this << *t;
            return *this;
        }
        template< class T >
        Nullstream& operator<<(const T &t) {
            return operator<<( static_cast<const LazyString&>( LazyStringImpl< T >( t ) ) );
        }

        virtual Nullstream& operator<< (std::ostream& ( *endl )(std::ostream&)) {
            return *this;
        }
        virtual Nullstream& operator<< (std::ios_base& (*hex)(std::ios_base&)) {
            return *this;
        }

        virtual void flush(Tee *t = 0) {}
    };
    extern Nullstream nullstream;

    class Logstream : public Nullstream {
        static mongo::mutex mutex;
        static int doneSetup;
        std::stringstream ss;
        int indent;
        LogLevel logLevel;
        static FILE* logfile;
        static boost::scoped_ptr<std::ostream> stream;
        static std::vector<Tee*> * globalTees;
        static bool isSyslog;
    public:
        static void logLockless( const StringData& s );

        static void setLogFile(FILE* f);
#ifndef _WIN32
        static void useSyslog(const char * name) {
            std::cout << "using syslog ident: " << name << std::endl;
            
            // openlog requires heap allocated non changing pointer
            // this should only be called once per pragram execution

            char * newName = (char *) malloc( strlen(name) + 1 );
            strcpy( newName , name);
            openlog( newName , LOG_ODELAY , LOG_USER );
            isSyslog = true;
        }
#endif
        
        static int magicNumber() { return 1717; }

        static int getLogDesc() {
            int fd = -1;
            if (logfile != NULL)
#if defined(_WIN32)
                // the ISO C++ conformant name is _fileno
                fd = _fileno( logfile );
#else
                fd = fileno( logfile );
#endif
            return fd;
        }

        void flush(Tee *t = 0);

        inline Nullstream& setLogLevel(LogLevel l) {
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
        Logstream& operator<<(unsigned short x){ ss << x; return *this; }
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
        Logstream& operator<< (std::ostream& ( *_endl )(std::ostream&)) {
            ss << '\n';
            flush(0);
            return *this;
        }
        Logstream& operator<< (std::ios_base& (*_hex)(std::ios_base&)) {
            ss << _hex;
            return *this;
        }

        Logstream& prolog() {
            return *this;
        }

        void addGlobalTee( Tee * t ) {
            if ( ! globalTees )
                globalTees = new std::vector<Tee*>();
            globalTees->push_back( t );
        }
        
        void removeGlobalTee( Tee * tee );
        
        void indentInc(){ indent++; }
        void indentDec(){ indent--; }
        int getIndent() const { return indent; }

    private:
        static boost::thread_specific_ptr<Logstream> tsp;
        Logstream() {
            indent = 0;
            _init();
        }
        void _init() {
            ss.str("");
            logLevel = LL_INFO;
        }
    public:
        static Logstream& get();
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
    inline Nullstream& _log( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
        return Logstream::get();
    }

    /** logging which we may not want during unit tests (dbtests) runs.
        set tlogLevel to -1 to suppress tlog() output in a test program. */
    Nullstream& tlog( int level = 0 );

    // log if debug build or if at a certain level
    inline Nullstream& dlog( int level ) {
        if ( level <= logLevel || DEBUG_BUILD )
            return Logstream::get().prolog();
        return nullstream;
    }

    inline Nullstream& log( int level ) {
        if ( level > logLevel )
            return nullstream;
        return Logstream::get().prolog();
    }

#define MONGO_LOG(level) if ( MONGO_likely(logLevel < (level)) ) { } else log( level )
#define LOG MONGO_LOG

    inline Nullstream& log( LogLevel l ) {
        return Logstream::get().prolog().setLogLevel( l );
    }

    inline Nullstream& log( const LabeledLevel& ll ) {
        Nullstream& stream = log( ll.getLevel() );
        if( ll.getLabel() != "" )
            stream << "[" << ll.getLabel() << "] ";
        return stream;
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

    /** output the error # and error message with prefix.
        handy for use as parm in uassert/massert.
        */
    string errnoWithPrefix( const char * prefix );

    struct LogIndentLevel {
        LogIndentLevel(){
            Logstream::get().indentInc();
        }
        ~LogIndentLevel(){
            Logstream::get().indentDec();
        }
    };

    extern Tee* const warnings; // Things put here go in serverStatus

    string errnoWithDescription(int errorcode = -1);
    void rawOut( const string &s );

} // namespace mongo
