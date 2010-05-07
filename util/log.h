// log.h

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

namespace mongo {

    using boost::shared_ptr;

    // Utility interface for stringifying object only when val() called.
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
        virtual string val() const { return (string)t_; }
    private:
        const T& t_;
    };

    class Tee { 
    public:
        virtual ~Tee(){}
        virtual void write(const string& str) = 0;
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
    public:
        static int magicNumber(){
            return 1717;
        }
        void flush(Tee *t = 0) {
            // this ensures things are sane
            if ( doneSetup == 1717 ){
                scoped_lock lk(mutex);
                string s = ss.str();
                if( t ) t->write(s);
                cout << s;
                cout.flush();
            }
            ss.str("");
        }

        /** note these are virtual */
        Logstream& operator<<(const char *x) { ss << x; return *this; }
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
                *this << t;
            return *this;
        }

        Logstream& prolog() {
            char now[64];
            time_t_to_String(time(0), now);
            now[20] = 0;
            ss << now;
            return *this;
        }

    private:
        static thread_specific_ptr<Logstream> tsp;
    public:
        static Logstream& get() {
            Logstream *p = tsp.get();
            if( p == 0 )
                tsp.reset( p = new Logstream() );
            return *p;
        }
    };

    extern int logLevel;

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

    inline Nullstream& log( int level ) {
        if ( level > logLevel )
            return nullstream;
        return Logstream::get().prolog();
    }

    inline Nullstream& log() {
        return Logstream::get().prolog();
    }

    /* TODOCONCURRENCY */
    inline ostream& stdcout() {
        return cout;
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

    inline string errnoWithDescription(int x = errno) {
        stringstream s;
        s << "errno:" << x << ' ' << strerror(x);
        return s.str();
    }

    /** output the error # and error message with prefix.  
        handy for use as parm in uassert/massert.
        */
    string errnoWithPrefix( const char * prefix = 0 );

} // namespace mongo
