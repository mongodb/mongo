// Copyright 2009.  10gen, Inc.

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>

#include "mongo/util/concurrency/mutex.h"

using namespace std;

#ifdef MONGO_HAVE_EXECINFO_BACKTRACE

#include <execinfo.h>
#include <cxxabi.h>

namespace mongo {
    static const int maxBackTraceFrames = 20;
    
    void printStackTrace( std::ostream &os ) {
        
        void *b[maxBackTraceFrames];
        
        int size = ::backtrace( b, maxBackTraceFrames );
        for ( int i = 0; i < size; i++ )
            os << std::hex << b[i] << std::dec << ' ';
        os << std::endl;
        
        char **strings;
        
        strings = ::backtrace_symbols( b, size );
        for ( int i = 0; i < size; i++ )
            os << ' ' << strings[i] << '\n';
        os.flush();
        ::free( strings );
    }

   string lastPiece( string& name ) {
        size_t x = name.rfind( "::" );
        if ( x == string::npos )
            return "";
        
        string last = name.substr( x + 2 );
        name = name.substr( 0 , x );
        return last;
    }

    bool isNameAConstructor( string name ) {
        size_t x = name.rfind( '(' );
        if ( name[name.size()-1] != ')' || x == string::npos )
            return false;
        
        name = name.substr( 0 , x );
        
        string method = lastPiece( name );
        if ( method.size() == 0 )
            return false;

        string clazz = lastPiece( name );
        if ( clazz.size() == 0 )
            clazz = name;
        
        return method == clazz;
    }

    class ConsCache {
    public:
        
        ConsCache() : _mutex( "ConsCache" ){}

        bool inCache( const string& name , bool& val ) const {
            SimpleMutex::scoped_lock lk( _mutex );
            map<string,bool>::const_iterator it = _map.find( name );
            if ( it == _map.end() )
                return false;
            return it->second;
        }

        void set( const string& name , bool val ) {
            SimpleMutex::scoped_lock lk( _mutex );
            _map[name] = val;
        }

    private:
        map<string,bool> _map;
        mutable SimpleMutex _mutex;
    };

    ConsCache &consCache = *(new ConsCache());

    bool inConstructorChain( bool printOffending ){
        void* b[maxBackTraceFrames];
        int size = ::backtrace( b, maxBackTraceFrames );
        char** strings = ::backtrace_symbols( b, size );
        
        for ( int i = 0; i < size; i++ ) {
            string x = strings[i];
            
            {
                bool temp;
                if ( consCache.inCache( x , temp ) )
                    return temp;
            }

            size_t l = x.find( '(' );
            size_t r = x.find( '+' );
            if ( l == string::npos || r == string::npos )
                continue;
            string sym = x.substr( l + 1 , r-l-1);
            if ( sym.size() == 0 )
                continue;

            int status = -1;
            char * nice = abi::__cxa_demangle( sym.c_str() , 0 , 0 , &status );
            if ( status ) 
                continue;
            
            string myNiceCopy = nice;
            
            if ( isNameAConstructor( nice ) ) {
                if ( printOffending ) 
                    std::cout << "found a constructor in the call tree: " << nice << std::endl;
                ::free( strings );
                ::free( nice );
                return true;
            }

            ::free( nice );

        }
        ::free( strings );

        return false;
    }

    bool inConstructorChainSupported() {
        return true;
    }
}

#else

namespace mongo {   
    bool inConstructorChain(){ return false; }
    bool inConstructorChainSupported() { return false; }
    void printStackTrace( std::ostream &os ) {}
}

#endif  // defined(MONGO_HAVE_EXECINFO_BACKTRACE)

