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

    class Nullstream {
    public:
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
        virtual void flush(){}
    };
    extern Nullstream nullstream;
    
#define LOGIT { ss << x; return *this; }

    class Logstream : public Nullstream {
        static boost::mutex &mutex;
        static int doneSetup;
        stringstream ss;
    public:
        static int magicNumber(){
            return 1717;
        }
        void flush() {
            // this ensures things are sane
            if ( doneSetup == 1717 ){
                boostlock lk(mutex);
                cout << ss.str();
                cout.flush();
            }
            ss.str("");
        }
        Logstream& operator<<(const char *x) LOGIT
        Logstream& operator<<(char *x) LOGIT
        Logstream& operator<<(char x) LOGIT
        Logstream& operator<<(int x) LOGIT
        Logstream& operator<<(ExitCode x) LOGIT
        Logstream& operator<<(long x) LOGIT
        Logstream& operator<<(unsigned long x) LOGIT
        Logstream& operator<<(unsigned x) LOGIT
        Logstream& operator<<(double x) LOGIT
        Logstream& operator<<(void *x) LOGIT
        Logstream& operator<<(const void *x) LOGIT
        Logstream& operator<<(long long x) LOGIT
        Logstream& operator<<(unsigned long long x) LOGIT
        Logstream& operator<<(bool x) LOGIT
        Logstream& operator<<(const LazyString& x) {
            ss << x.val();
            return *this;
        }
        Logstream& operator<< (ostream& ( *_endl )(ostream&)) {
            ss << '\n';
            flush();
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
            Logstream::get().flush();
    }

    /* without prolog */
    inline Nullstream& _log( int level = 0 ){
        if ( level > logLevel )
            return nullstream;
        return Logstream::get();
    }

    inline Nullstream& log( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
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

} // namespace mongo
