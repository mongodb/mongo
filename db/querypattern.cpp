// @file querypattern.cpp - Query pattern matching for selecting similar plans given similar queries.

/*    Copyright 2011 10gen Inc.
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

#include "querypattern.h"

namespace mongo {

    /** for testing only - speed unimportant */
    bool QueryPattern::operator==( const QueryPattern &other ) const {
        bool less = operator<( other );
        bool more = other.operator<( *this );
        assert( !( less && more ) );
        return !( less || more );
    }
    
    /** for testing only - speed unimportant */
    bool QueryPattern::operator!=( const QueryPattern &other ) const {
        return !operator==( other );
    }
    
    void QueryPattern::setSort( const BSONObj sort ) {
        _sort = normalizeSort( sort );
    }
    
    BSONObj QueryPattern::normalizeSort( const BSONObj &spec ) {
        if ( spec.isEmpty() )
            return spec;
        int direction = ( spec.firstElement().number() >= 0 ) ? 1 : -1;
        BSONObjIterator i( spec );
        BSONObjBuilder b;
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            b.append( e.fieldName(), direction * ( ( e.number() >= 0 ) ? -1 : 1 ) );
        }
        return b.obj();
    }
    
} // namespace mongo
