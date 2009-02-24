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
        FieldBound( const BSONElement &e = emptyObj.firstElement() );
        FieldBound &operator&=( const FieldBound &other );
        BSONElement lower() const { return lower_; }
        BSONElement upper() const { return upper_; }
        bool equality() const { return lower_.woCompare( upper_, false ) == 0; }
        bool nontrivial() const {
            return
            minKey.firstElement().woCompare( lower_, false ) != 0 ||
            maxKey.firstElement().woCompare( upper_, false ) != 0;
        }
    private:
        BSONObj addObj( const BSONObj &o );
        string simpleRegexEnd( string regex );
        BSONElement lower_;
        BSONElement upper_;
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
        bool operator==( const QueryPattern &other ) const {
            map< string, Type >::const_iterator i = fieldTypes_.begin();
            map< string, Type >::const_iterator j = other.fieldTypes_.begin();
            while( i != fieldTypes_.end() ) {
                if ( j == other.fieldTypes_.end() )
                    return false;
                if ( i->first != j->first )
                    return false;
                if ( i->second != j->second )
                    return false;
                ++i;
                ++j;
            }
            return ( j == other.fieldTypes_.end() );
        }
        bool operator!=( const QueryPattern &other ) const {
            return !operator==( other );
        }
    private:
        QueryPattern() {}
        map< string, Type > fieldTypes_;
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
        QueryPattern pattern() const;
    private:
        static FieldBound *trivialBound_;
        static FieldBound &trivialBound();
        map< string, FieldBound > bounds_;
        const char *ns_;
        BSONObj query_;
    };

} // namespace mongo
