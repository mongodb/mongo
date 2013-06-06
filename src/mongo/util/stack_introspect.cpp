// stack_introspect.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/util/stack_introspect.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <vector>

#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/text.h"

using namespace std;

#ifdef MONGO_HAVE_EXECINFO_BACKTRACE

#include <execinfo.h>
#include <cxxabi.h>

namespace mongo {
    
    namespace {
    
        int maxBackTraceFrames = 25;

        bool isNameAConstructorOrDesctructor( string name ) {
            //cout << "XX : " << name << endl;
            
            size_t x = name.rfind( '(' );
            if ( name[name.size()-1] != ')' || x == string::npos )
                return false;
            
            name = name.substr( 0 , x );
            
            vector<string> pieces = StringSplitter::split( name , "::" );
            
            if ( pieces.size() < 2 )
                return false;
            
            string method = pieces[pieces.size()-1];
            string clazz = pieces[pieces.size()-2];
            
            if ( method[0] == '~' )
                method = method.substr(1);
            
            if ( name.find( "Geo" ) != string::npos ) 
                return false;
            
            if ( name.find( "Tests" ) != string::npos )
                return false;
            
            if ( name.find( "ScopedDistributedLock" ) != string::npos )
                return false;

            if ( name.find( "PooledScope" ) != string::npos ) {
                // SERVER-8090
                return false;
            }

            if ( name.find( "Matcher2" ) != string::npos ) {
                // SERVER-9778
                return false;
            }

            return method == clazz;
        }
        
        class Cache {
        public:
            
            Cache() : _mutex( "ObjectLifyCycleCache" ){}
            
            bool inCache( void* name , bool& val ) const {
                SimpleMutex::scoped_lock lk( _mutex );
                map<void*,bool>::const_iterator it = _map.find( name );
                if ( it == _map.end() )
                    return false;
                val = it->second;
                return true;
            }
            
            void set( void* name , bool val ) {
                SimpleMutex::scoped_lock lk( _mutex );
                _map[name] = val;
            }
            
        private:
            map<void*,bool> _map;
            mutable SimpleMutex _mutex;
        };
        
        Cache &cache = *(new Cache());
    }
    
    bool inConstructorChain( bool printOffending ){
        void* b[maxBackTraceFrames];
        int size = ::backtrace( b, maxBackTraceFrames );

        char** strings = 0;
        
        for ( int i = 0; i < size; i++ ) {
            
            {
                bool temp = false;
                if ( cache.inCache( b[i] , temp ) ) {
                    if ( temp )
                        return true;
                    continue;
                }
            }

            if ( ! strings ) 
                strings = ::backtrace_symbols( b, size );

            string symbol = strings[i];

            size_t l = symbol.find( '(' );
            size_t r = symbol.find( '+' );
            if ( l == string::npos || r == string::npos )
                continue;
            symbol = symbol.substr( l + 1 , r-l-1);
            if ( symbol.size() == 0 )
                continue;

            int status = -1;
            char * nice = abi::__cxa_demangle( symbol.c_str() , 0 , 0 , &status );
            if ( status ) 
                continue;
            
            string myNiceCopy = nice;
            
            if ( isNameAConstructorOrDesctructor( nice ) ) {
                if ( printOffending ) 
                    std::cout << "found a constructor in the call tree: " << nice << "\n" << symbol << std::endl;
                ::free( strings );
                ::free( nice );
                cache.set( b[i] , true );
                return true;
            }

            ::free( nice );

            cache.set( b[i] , false );

        }
        ::free( strings );

        return false;
    }

    bool inConstructorChainSupported() {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }
}

#else

namespace mongo {   
    bool inConstructorChain( bool ){ return false; }
    bool inConstructorChainSupported() { return false; }
}

#endif  // defined(MONGO_HAVE_EXECINFO_BACKTRACE)

