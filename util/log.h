// log.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

namespace mongo {

    // If you don't want your class to inherit from Stringable (for example, if
    // you don't want a virtual destructor) then add a function like the
    // following to your class, which takes a pointer to an object of your class
    // type:
    // static string toString( void * );
    class LazyString {
    public:
        // LazyString is designed to be used in situations where the lifespan of
        // a temporary object used to construct a LazyString completely includes
        // the lifespan of the LazyString object itself.
        template< class T >
        LazyString( const T &t ) : obj_( (void*)&t ), fun_( &T::toString ) {}
        string val() const { return (*fun_)(obj_); }
    private:
        void *obj_;
        string (*fun_) (void *);
    };
    
    class Nullstream {
    public:
        virtual ~Nullstream() {}
      // todo: just use a template for all these
        virtual Nullstream& operator<<(const char *) {
            return *this;
        }
        virtual Nullstream& operator<<(char x) {
            return *this;
        }
        virtual Nullstream& operator<<(int) {
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
        virtual Nullstream& operator<<(long long) {
            return *this;
        }
        virtual Nullstream& operator<<(unsigned long long) {
            return *this;
        }
        virtual Nullstream& operator<<(const string&) {
            return *this;
        }
        virtual Nullstream& operator<<(const LazyString&) {
            return *this;
        }
        virtual Nullstream& operator<<(const Stringable&) {
            return *this;
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
    
#define LOGIT { boostlock lk(mutex); cout << x; return *this; }
    class Logstream : public Nullstream {
        static boost::mutex mutex;
    public:
        void flush() {
            boostlock lk(mutex);
            cout.flush();
        }
        Logstream& operator<<(const char *x) LOGIT
        Logstream& operator<<(char x) LOGIT
        Logstream& operator<<(int x) LOGIT
        Logstream& operator<<(long x) LOGIT
        Logstream& operator<<(unsigned long x) LOGIT
        Logstream& operator<<(unsigned x) LOGIT
        Logstream& operator<<(double x) LOGIT
        Logstream& operator<<(void *x) LOGIT
        Logstream& operator<<(long long x) LOGIT
        Logstream& operator<<(unsigned long long x) LOGIT
        Logstream& operator<<(const string& x) LOGIT
        Logstream& operator<<(const LazyString& x) {
            boostlock lk(mutex);
            cout << x.val();
            return *this;
        }
        Logstream& operator<<(const Stringable& x) {
            boostlock lk(mutex);
            cout << x.toString();
            return *this;
        }
        Logstream& operator<< (ostream& ( *_endl )(ostream&)) {
            boostlock lk(mutex);
            cout << _endl;
            return *this;
        }
        Logstream& operator<< (ios_base& (*_hex)(ios_base&)) {
            boostlock lk(mutex);
            cout << _hex;
            return *this;
        }
        Logstream& prolog(bool withNs = false) {
            char now[64];
            time_t_to_String(time(0), now);
            now[20] = 0;

            boostlock lk(mutex);
            cout << now;
            if ( withNs && /*database && */curNs )
                cout << curNs << ' ';
            return *this;
        }
    };
    extern Logstream logstream;

    extern int logLevel;

    inline Nullstream& problem( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
        return logstream.prolog(true);
    }
    
    inline Nullstream& out( int level = 0 ) {
        if ( level > logLevel )
            return nullstream;
        return logstream;
    }
    
    /* flush the log stream if the log level is 
       at the specified level or higher. */
    inline void logflush(int level = 0) { 
        if( level > logLevel )
            logstream.flush();
    }

    inline Nullstream& log( int level = 0 ){
        if ( level > logLevel )
            return nullstream;
        return logstream.prolog();
    }

    inline ostream& stdcout() {
        return cout;
    }

} // namespace mongo
