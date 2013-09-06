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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/util/stack_introspect.h"

#if !defined(_WIN32)

#include <cstdlib>
#include <cxxabi.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "mongo/platform/backtrace.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/text.h"

using namespace std;

namespace mongo {
    
    namespace {
    
        int maxBackTraceFrames = 25;

        enum IsCons {
            YES,
            NO,
            EXEMPT
        };

        IsCons isNameAConstructorOrDestructor( string name ) {
            //cout << "XX : " << name << endl;
            
            size_t x = name.rfind( '(' );
            if ( name[name.size()-1] != ')' || x == string::npos )
                return NO;
            
            name = name.substr( 0 , x );
            
            vector<string> pieces = StringSplitter::split( name , "::" );
            
            if ( pieces.size() < 2 )
                return NO;
            
            string method = pieces[pieces.size()-1];
            string clazz = pieces[pieces.size()-2];
            
            if ( method[0] == '~' )
                method = method.substr(1);
            
            if ( name.find( "Geo" ) != string::npos ) 
                return EXEMPT;
            
            if ( name.find( "Tests" ) != string::npos )
                return EXEMPT;
            
            if ( name.find( "ScopedDistributedLock" ) != string::npos )
                return EXEMPT;

            if ( name.find( "PooledScope" ) != string::npos ) {
                // SERVER-8090
                return EXEMPT;
            }

            if ( name.find( "Matcher2" ) != string::npos ) {
                // SERVER-9778
                return EXEMPT;
            }

            return method == clazz ? YES : NO;
        }
        
        class Cache {
        public:
            
            Cache() : _mutex( "ObjectLifyCycleCache" ){}
            
            bool inCache( void* name , IsCons& val ) const {
                SimpleMutex::scoped_lock lk( _mutex );
                map<void*,IsCons>::const_iterator it = _map.find( name );
                if ( it == _map.end() )
                    return false;
                val = it->second;
                return true;
            }
            
            void set( void* name , IsCons val ) {
                SimpleMutex::scoped_lock lk( _mutex );
                _map[name] = val;
            }
            
        private:
            map<void*,IsCons> _map;
            mutable SimpleMutex _mutex;
        };
        
        Cache &cache = *(new Cache());
    }
    
    bool inConstructorChain( bool printOffending ){
        void* b[maxBackTraceFrames];
        int size = backtrace( b, maxBackTraceFrames );

        char** strings = 0;
        
        for ( int i = 0; i < size; i++ ) {
            
            {
                IsCons isCons = NO;
                if ( cache.inCache( b[i] , isCons ) ) {
                    if (isCons == NO)
                        continue;

                    if (strings) ::free(strings);
                    return isCons == YES;
                }
            }

            if ( ! strings ) 
                strings = backtrace_symbols( b, size );

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
            ::free( nice );

            IsCons isCons = isNameAConstructorOrDestructor(myNiceCopy);
            cache.set(b[i], isCons);

            if (isCons == EXEMPT) {
                ::free( strings );
                return false;
            }

            if (isCons == YES) {
                if ( printOffending )
                    log() << "found a constructor in the call tree: " << myNiceCopy << "\n"
                          << symbol << std::endl;

                ::free( strings );
                return true;
            }
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

#endif  // #if !defined(_WIN32)
