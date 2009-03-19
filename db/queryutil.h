// queryutil.h

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

#include "jsobj.h"

namespace mongo {

    class FieldBound {
    public:
        FieldBound( const BSONElement &e = BSONObj().firstElement() );
        const FieldBound &operator&=( const FieldBound &other );
        BSONElement lower() const { return lower_; }
        BSONElement upper() const { return upper_; }
        bool lowerInclusive() const { return lowerInclusive_; }
        bool upperInclusive() const { return upperInclusive_; }
        bool equality() const {
            return
            lower_.woCompare( upper_, false ) == 0 &&
            upperInclusive_ &&
            lowerInclusive_;
        }
        bool nontrivial() const {
            return
            minKey.firstElement().woCompare( lower_, false ) != 0 ||
            maxKey.firstElement().woCompare( upper_, false ) != 0;
        }
    private:
        BSONObj addObj( const BSONObj &o );
        string simpleRegexEnd( string regex );
        BSONElement lower_;
        bool lowerInclusive_;
        BSONElement upper_;
        bool upperInclusive_;
        vector< BSONObj > objData_;
    };
    
    class QueryPattern {
    public:
        friend class FieldBoundSet;
        enum Type {
            Equality,
            LowerBound,
            UpperBound,
            UpperAndLowerBound
        };
        // for testing only, speed unimportant
        bool operator==( const QueryPattern &other ) const {
            bool less = operator<( other );
            bool more = other.operator<( *this );
            assert( !( less && more ) );
            return !( less || more );
        }
        bool operator!=( const QueryPattern &other ) const {
            return !operator==( other );
        }
        bool operator<( const QueryPattern &other ) const {
            map< string, Type >::const_iterator i = fieldTypes_.begin();
            map< string, Type >::const_iterator j = other.fieldTypes_.begin();
            while( i != fieldTypes_.end() ) {
                if ( j == other.fieldTypes_.end() )
                    return false;
                if ( i->first < j->first )
                    return true;
                else if ( i->first > j->first )
                    return false;
                if ( i->second < j->second )
                    return true;
                else if ( i->second > j->second )
                    return false;
                ++i;
                ++j;
            }
            if ( j != other.fieldTypes_.end() )
                return true;
            return sort_.woCompare( other.sort_ ) < 0;
        }
    private:
        QueryPattern() {}
        void setSort( const BSONObj sort ) {
            sort_ = normalizeSort( sort );
        }
        BSONObj static normalizeSort( const BSONObj &spec ) {
            if ( spec.isEmpty() )
                return spec;
            int direction = ( spec.firstElement().number() >= 0 ) ? 1 : -1;
            BSONObjIterator i( spec );
            BSONObjBuilder b;
            while( i.more() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                b.append( e.fieldName(), direction * ( ( e.number() >= 0 ) ? -1 : 1 ) );
            }
            return b.obj();
        }
        map< string, Type > fieldTypes_;
        BSONObj sort_;
    };
    
    class FieldBoundSet {
    public:
        FieldBoundSet( const char *ns, const BSONObj &query );
        const FieldBound &bound( const char *fieldName ) const {
            map< string, FieldBound >::const_iterator f = bounds_.find( fieldName );
            if ( f == bounds_.end() )
                return trivialBound();
            return f->second;
        }
        int nBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                ++count;
            return count;
        }
        int nNontrivialBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                if ( i->second.nontrivial() )
                    ++count;
            return count;
        }
        const char *ns() const { return ns_; }
        BSONObj query() const { return query_; }
        BSONObj simplifiedQuery() const;
        bool matchPossible() const {
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                if ( i->second.lower().woCompare( i->second.upper(), false ) > 0 )
                    return false;
            return true;
        }
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
    private:
        static FieldBound *trivialBound_;
        static FieldBound &trivialBound();
        map< string, FieldBound > bounds_;
        const char *ns_;
        BSONObj query_;
    };

} // namespace mongo
