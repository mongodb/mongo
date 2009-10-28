// queryutil.h

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

#include "jsobj.h"

namespace mongo {

    struct FieldBound {
        BSONElement bound_;
        bool inclusive_;
        bool operator==( const FieldBound &other ) const {
            return bound_.woCompare( other.bound_ ) == 0 &&
            inclusive_ == other.inclusive_;
        }
    };

    struct FieldInterval {
        FieldInterval(){}
        FieldInterval( const BSONElement& e ){
            lower_.bound_ = upper_.bound_ = e;
            lower_.inclusive_ = upper_.inclusive_ = true;
        }
        FieldBound lower_;
        FieldBound upper_;
        bool valid() const {
            int cmp = lower_.bound_.woCompare( upper_.bound_, false );
            return ( cmp < 0 || ( cmp == 0 && lower_.inclusive_ && upper_.inclusive_ ) );
        }
    };

    // range of a field's value that may be determined from query -- used to
    // determine index limits
    class FieldRange {
    public:
        FieldRange( const BSONElement &e = BSONObj().firstElement() , bool optimize=true );
        const FieldRange &operator&=( const FieldRange &other );
        BSONElement min() const { assert( !empty() ); return intervals_[ 0 ].lower_.bound_; }
        BSONElement max() const { assert( !empty() ); return intervals_[ intervals_.size() - 1 ].upper_.bound_; }
        bool minInclusive() const { assert( !empty() ); return intervals_[ 0 ].lower_.inclusive_; }
        bool maxInclusive() const { assert( !empty() ); return intervals_[ intervals_.size() - 1 ].upper_.inclusive_; }
        bool equality() const {
            return
                !empty() &&
                min().woCompare( max(), false ) == 0 &&
                maxInclusive() &&
                minInclusive();
        }
        bool nontrivial() const {
            return
                ! empty() && 
                ( minKey.firstElement().woCompare( min(), false ) != 0 ||
                  maxKey.firstElement().woCompare( max(), false ) != 0 );
        }
        bool empty() const { return intervals_.empty(); }
		const vector< FieldInterval > &intervals() const { return intervals_; }
    private:
        BSONObj addObj( const BSONObj &o );
        string simpleRegexEnd( string regex );
        vector< FieldInterval > intervals_;
        vector< BSONObj > objData_;
    };
    
    // implements query pattern matching, used to determine if a query is
    // similar to an earlier query and should use the same plan
    class QueryPattern {
    public:
        friend class FieldRangeSet;
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
            while( i.moreWithEOO() ) {
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
    
    // ranges of fields' value that may be determined from query -- used to
    // determine index limits
    class FieldRangeSet {
    public:
        FieldRangeSet( const char *ns, const BSONObj &query , bool optimize=true );
        const FieldRange &range( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = ranges_.find( fieldName );
            if ( f == ranges_.end() )
                return trivialRange();
            return f->second;
        }
        int nNontrivialRanges() const {
            int count = 0;
            for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i )
                if ( i->second.nontrivial() )
                    ++count;
            return count;
        }
        const char *ns() const { return ns_; }
        BSONObj query() const { return query_; }
        // if fields is specified, order fields of returned object to match those of 'fields'
        BSONObj simplifiedQuery( const BSONObj &fields = BSONObj() ) const;
        bool matchPossible() const {
            for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i )
                if ( i->second.empty() )
                    return false;
            return true;
        }
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
        BoundList indexBounds( const BSONObj &keyPattern, int direction ) const;
    private:
        static FieldRange *trivialRange_;
        static FieldRange &trivialRange();
        mutable map< string, FieldRange > ranges_;
        const char *ns_;
        BSONObj query_;
    };

    /**
       used for doing field limiting
     */
    class FieldMatcher {
    public:

        FieldMatcher(bool include=false) : errmsg(NULL), include_(include)  {}
        
        void add( const BSONObj& o );

        void append( BSONObjBuilder& b , const BSONElement& e ) const;

        BSONObj getSpec() const;

        const char* errmsg; //null if FieldMatcher is valid
    private:

        void add( const string& field, bool include );
        void appendArray( BSONObjBuilder& b , const BSONObj& a ) const;

        bool include_; // true if default at this level is to include
        //TODO: benchmark vector<pair> vs map
        typedef map<string, boost::shared_ptr<FieldMatcher> > FieldMap;
        FieldMap fields_;
        BSONObj source_;
    };


} // namespace mongo
