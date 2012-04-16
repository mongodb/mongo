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
#include "mongo/db/queryutil.h"

namespace mongo {

    QueryPattern::QueryPattern( const FieldRangeSet &frs, const BSONObj &sort ) {
        for( map<string,FieldRange>::const_iterator i = frs.ranges().begin(); i != frs.ranges().end(); ++i ) {
            if ( i->second.equality() ) {
                _fieldTypes[ i->first ] = QueryPattern::Equality;
            }
            else if ( i->second.empty() ) {
                _fieldTypes[ i->first ] = QueryPattern::Empty;
            }
            else if ( !i->second.universal() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower ) {
                    _fieldTypes[ i->first ] = QueryPattern::UpperAndLowerBound;
                }
                else if ( upper ) {
                    _fieldTypes[ i->first ] = QueryPattern::UpperBound;
                }
                else if ( lower ) {
                    _fieldTypes[ i->first ] = QueryPattern::LowerBound;
                }
                else {
                    _fieldTypes[ i->first ] = QueryPattern::ConstraintPresent;
                }
            }
        }
        setSort( sort );
    }

    /** for testing only - speed unimportant */
    bool QueryPattern::operator==( const QueryPattern &other ) const {
        bool less = operator<( other );
        bool more = other.operator<( *this );
        verify( !( less && more ) );
        return !( less || more );
    }
    
    /** for testing only - speed unimportant */
    bool QueryPattern::operator!=( const QueryPattern &other ) const {
        return !operator==( other );
    }
    
    string typeToString( enum QueryPattern::Type t ) {
        switch (t) {
            case QueryPattern::Empty:
                return "Empty";
            case QueryPattern::Equality:
                return "Equality";
            case QueryPattern::LowerBound:
                return "LowerBound";
            case QueryPattern::UpperBound:
                return "UpperBound";
            case QueryPattern::UpperAndLowerBound:
                return "UpperAndLowerBound";
            case QueryPattern::ConstraintPresent:
                return "ConstraintPresent";
        }
        return "";
    }
    
    string QueryPattern::toString() const {
        BSONObjBuilder b;
        for( map<string,Type>::const_iterator i = _fieldTypes.begin(); i != _fieldTypes.end(); ++i ) {
            b << i->first << typeToString( i->second );
        }
        return BSON( "query" << b.done() << "sort" << _sort ).toString();
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
    
    CachedQueryPlan::CachedQueryPlan( const BSONObj &indexKey, long long nScanned,
                                     CandidatePlanCharacter planCharacter ) :
    _indexKey( indexKey ),
    _nScanned( nScanned ),
    _planCharacter( planCharacter ) {
    }

    
} // namespace mongo
