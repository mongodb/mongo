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
        BSONElement _bound;
        bool _inclusive;
        bool operator==( const FieldBound &other ) const {
            return _bound.woCompare( other._bound ) == 0 &&
            _inclusive == other._inclusive;
        }
        void flipInclusive() { _inclusive = !_inclusive; }
    };

    struct FieldInterval {
        FieldInterval() : _cachedEquality( -1 ) {}
        FieldInterval( const BSONElement& e ) : _cachedEquality( -1 ) {
            _lower._bound = _upper._bound = e;
            _lower._inclusive = _upper._inclusive = true;
        }
        FieldBound _lower;
        FieldBound _upper;
        bool strictValid() const {
            int cmp = _lower._bound.woCompare( _upper._bound, false );
            return ( cmp < 0 || ( cmp == 0 && _lower._inclusive && _upper._inclusive ) );
        }
        bool equality() const {
            if ( _cachedEquality == -1 ) {
                _cachedEquality = ( _lower._inclusive && _upper._inclusive && _lower._bound.woCompare( _upper._bound, false ) == 0 );
            }
            return _cachedEquality;
        }
        mutable int _cachedEquality;
    };

    // range of a field's value that may be determined from query -- used to
    // determine index limits
    class FieldRange {
    public:
        FieldRange( const BSONElement &e = BSONObj().firstElement() , bool isNot=false , bool optimize=true );
        const FieldRange &operator&=( const FieldRange &other );
        const FieldRange &operator|=( const FieldRange &other );
        // does not remove fully contained ranges (eg [1,3] - [2,2] doesn't remove anything)
        // in future we can change so that an or on $in:[3] combined with $gt:2 doesn't scan 3 a second time
        const FieldRange &operator-=( const FieldRange &other );
        // true iff other includes this
        bool operator<=( const FieldRange &other );
        BSONElement min() const { assert( !empty() ); return _intervals[ 0 ]._lower._bound; }
        BSONElement max() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._bound; }
        bool minInclusive() const { assert( !empty() ); return _intervals[ 0 ]._lower._inclusive; }
        bool maxInclusive() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._inclusive; }
        bool equality() const {
            return
                !empty() &&
                min().woCompare( max(), false ) == 0 &&
                maxInclusive() &&
                minInclusive();
        }
        bool inQuery() const {
            if ( equality() ) {
                return true;
            }
            for( vector< FieldInterval >::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {            
                if ( !i->equality() ) {
                    return false;
                }
            }
            return true;
        }
        bool nontrivial() const {
            return
                ! empty() && 
                ( minKey.firstElement().woCompare( min(), false ) != 0 ||
                  maxKey.firstElement().woCompare( max(), false ) != 0 );
        }
        bool empty() const { return _intervals.empty(); }
        void makeEmpty() { _intervals.clear(); }
		const vector< FieldInterval > &intervals() const { return _intervals; }
        string getSpecial() const { return _special; }
        void setExclusiveBounds() {
            for( vector< FieldInterval >::iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
                i->_lower._inclusive = false;
                i->_upper._inclusive = false;
            }
        }
        // constructs a range which is the reverse of the current one
        // note - the resulting intervals may not be strictValid()
        void reverse( FieldRange &ret ) const {
            assert( _special.empty() );
            ret._intervals.clear();
            ret._objData = _objData;
            for( vector< FieldInterval >::const_reverse_iterator i = _intervals.rbegin(); i != _intervals.rend(); ++i ) {
                FieldInterval fi;
                fi._lower = i->_upper;
                fi._upper = i->_lower;
                ret._intervals.push_back( fi );
            }
        }
    private:
        BSONObj addObj( const BSONObj &o );
        void finishOperation( const vector< FieldInterval > &newIntervals, const FieldRange &other );
        vector< FieldInterval > _intervals;
        vector< BSONObj > _objData;
        string _special;
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
            map< string, Type >::const_iterator i = _fieldTypes.begin();
            map< string, Type >::const_iterator j = other._fieldTypes.begin();
            while( i != _fieldTypes.end() ) {
                if ( j == other._fieldTypes.end() )
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
            if ( j != other._fieldTypes.end() )
                return true;
            return _sort.woCompare( other._sort ) < 0;
        }
    private:
        QueryPattern() {}
        void setSort( const BSONObj sort ) {
            _sort = normalizeSort( sort );
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
        map< string, Type > _fieldTypes;
        BSONObj _sort;
    };

    // a BoundList contains intervals specified by inclusive start
    // and end bounds.  The intervals should be nonoverlapping and occur in
    // the specified direction of traversal.  For example, given a simple index {i:1}
    // and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
    // would be valid for index {i:-1} with direction -1.
    typedef vector< pair< BSONObj, BSONObj > > BoundList;	
    
    // ranges of fields' value that may be determined from query -- used to
    // determine index limits
    class FieldRangeSet {
    public:
        friend class FieldRangeOrSet;
        friend class FieldRangeVector;
        FieldRangeSet( const char *ns, const BSONObj &query , bool optimize=true );
        bool hasRange( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
            return f != _ranges.end();
        }
        const FieldRange &range( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
            if ( f == _ranges.end() )
                return trivialRange();
            return f->second;            
        }
        FieldRange &range( const char *fieldName ) {
            map< string, FieldRange >::iterator f = _ranges.find( fieldName );
            if ( f == _ranges.end() )
                return trivialRange();
            return f->second;            
        }
        int nNontrivialRanges() const {
            int count = 0;
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i )
                if ( i->second.nontrivial() )
                    ++count;
            return count;
        }
        const char *ns() const { return _ns; }
        // if fields is specified, order fields of returned object to match those of 'fields'
        BSONObj simplifiedQuery( const BSONObj &fields = BSONObj() ) const;
        bool matchPossible() const {
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i )
                if ( i->second.empty() )
                    return false;
            return true;
        }
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
        string getSpecial() const;
        const FieldRangeSet &operator-=( const FieldRangeSet &other ) {
            int nUnincluded = 0;
            string unincludedKey;
            map< string, FieldRange >::iterator i = _ranges.begin();
            map< string, FieldRange >::const_iterator j = other._ranges.begin();
            while( nUnincluded < 2 && i != _ranges.end() && j != other._ranges.end() ) {
                int cmp = i->first.compare( j->first );
                if ( cmp == 0 ) {
                    if ( i->second <= j->second ) {
                        // nothing
                    } else {
                        ++nUnincluded;
                        unincludedKey = i->first;
                    }
                    ++i;
                    ++j;
                } else if ( cmp < 0 ) {
                    ++i;
                } else {
                    // other has a bound we don't, nothing can be done
                    return *this;
                }
            }
            if ( j != other._ranges.end() ) {
                // other has a bound we don't, nothing can be done
                return *this;                
            }
            if ( nUnincluded > 1 ) {
                return *this;
            }
            if ( nUnincluded == 0 ) {
                makeEmpty();
                return *this;
            }
            // nUnincluded == 1
            _ranges[ unincludedKey ] -= other._ranges[ unincludedKey ];
            appendQueries( other );
            return *this;
        }
        const FieldRangeSet &operator&=( const FieldRangeSet &other ) {
            map< string, FieldRange >::iterator i = _ranges.begin();
            map< string, FieldRange >::const_iterator j = other._ranges.begin();
            while( i != _ranges.end() && j != other._ranges.end() ) {
                int cmp = i->first.compare( j->first );
                if ( cmp == 0 ) {
                    i->second &= j->second;
                    ++i;
                    ++j;
                } else if ( cmp < 0 ) {
                    ++i;
                } else {
                    _ranges[ j->first ] = j->second;
                    ++j;
                }
            }
            while( j != other._ranges.end() ) {
                _ranges[ j->first ] = j->second;
                ++j;                
            }
            appendQueries( other );
            return *this;
        }
        // TODO get rid of this
        BoundList indexBounds( const BSONObj &keyPattern, int direction ) const;
    private:
        void appendQueries( const FieldRangeSet &other ) {
            for( vector< BSONObj >::const_iterator i = other._queries.begin(); i != other._queries.end(); ++i ) {
                _queries.push_back( *i );                
            }                        
        }
        void makeEmpty() {
            for( map< string, FieldRange >::iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                i->second.makeEmpty();
            }
        }
        void processQueryField( const BSONElement &e, bool optimize );
        void processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize );
        static FieldRange *trivialRange_;
        static FieldRange &trivialRange();
        mutable map< string, FieldRange > _ranges;
        const char *_ns;
        // make sure memory for FieldRange BSONElements is owned
        vector< BSONObj > _queries;
    };

    class FieldRangeVector {
    public:
        FieldRangeVector( const FieldRangeSet &frs, const BSONObj &keyPattern, int direction )
        :_keyPattern( keyPattern ), _direction( direction >= 0 ? 1 : -1 )
        {
            _queries = frs._queries;
            BSONObjIterator i( _keyPattern );
            while( i.more() ) {
                BSONElement e = i.next();
                int number = (int) e.number(); // returns 0.0 if not numeric
                bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
                if ( forward ) {
                    _ranges.push_back( frs.range( e.fieldName() ) );
                } else {
                    _ranges.push_back( FieldRange() );
                    frs.range( e.fieldName() ).reverse( _ranges.back() );
                }
                assert( !_ranges.back().empty() );
            }
            uassert( 13385, "combinatorial limit of $in partitioning of result set exceeded", size() < 1000000 );
        }
        long long size() {
            long long ret = 1;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                ret *= i->intervals().size();
            }
            return ret;
        }        
        BSONObj startKey() const {
            BSONObjBuilder b;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                const FieldInterval &fi = i->intervals().front();
                b.appendAs( fi._lower._bound, "" );
            }
            return b.obj();            
        }
        BSONObj endKey() const {
            BSONObjBuilder b;
            for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                const FieldInterval &fi = i->intervals().back();
                b.appendAs( fi._upper._bound, "" );
            }
            return b.obj();            
        }
        BSONObj obj() const {
            BSONObjBuilder b;
            BSONObjIterator k( _keyPattern );
            for( int i = 0; i < (int)_ranges.size(); ++i ) {
                BSONArrayBuilder a( b.subarrayStart( k.next().fieldName() ) );
                for( vector< FieldInterval >::const_iterator j = _ranges[ i ].intervals().begin();
                    j != _ranges[ i ].intervals().end(); ++j ) {
                    a << BSONArray( BSON_ARRAY( j->_lower._bound << j->_upper._bound ).clientReadable() );
                }
                a.done();
            }
            return b.obj();
        }
        bool matches( const BSONObj &obj ) const;
        class Iterator {
        public:
            Iterator( const FieldRangeVector &v ) : _v( v ), _i( _v._ranges.size(), -1 ), _cmp( _v._ranges.size(), 0 ), _inc( _v._ranges.size(), false ), _after() {
            }
            static BSONObj minObject() {
                BSONObjBuilder b;
                b.appendMinKey( "" );
                return b.obj();
            }
            static BSONObj maxObject() {
                BSONObjBuilder b;
                b.appendMaxKey( "" );
                return b.obj();
            }
            bool advance() {
                int i = _i.size() - 1;
                while( i >= 0 && _i[ i ] >= ( (int)_v._ranges[ i ].intervals().size() - 1 ) ) {
                    --i;
                }
                if( i >= 0 ) {
                    _i[ i ]++;
                    for( unsigned j = i + 1; j < _i.size(); ++j ) {
                        _i[ j ] = 0;
                    }
                } else {
                    _i[ 0 ] = _v._ranges[ 0 ].intervals().size();
                }
                return ok();
            }
            // return value
            // -2 end of iteration
            // -1 no skipping
            // >= 0 skip parameter
            int advance( const BSONObj &curr );
            const vector< const BSONElement * > &cmp() const { return _cmp; }
            const vector< bool > &inc() const { return _inc; }
            bool after() const { return _after; }
            void prepDive();
            void setZero( int i ) {
                for( int j = i; j < (int)_i.size(); ++j ) {
                    _i[ j ] = 0;
                }
            }
            void setMinus( int i ) {
                for( int j = i; j < (int)_i.size(); ++j ) {
                    _i[ j ] = -1;
                }
            }
            bool ok() {
                return _i[ 0 ] < (int)_v._ranges[ 0 ].intervals().size();
            }
            BSONObj startKey() {
                BSONObjBuilder b;
                for( int unsigned i = 0; i < _i.size(); ++i ) {
                    const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
                    b.appendAs( fi._lower._bound, "" );
                }
                return b.obj();
            }
            // temp
            BSONObj endKey() {
                BSONObjBuilder b;
                for( int unsigned i = 0; i < _i.size(); ++i ) {
                    const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
                    b.appendAs( fi._upper._bound, "" );
                }
                return b.obj();            
            }
            // check
        private:
            const FieldRangeVector &_v;
            vector< int > _i;
            vector< const BSONElement* > _cmp;
            vector< bool > _inc;
            bool _after;
        };
    private:
        int matchingLowElement( const BSONElement &e, int i, bool direction, bool &lowEquality ) const;
        bool matchesElement( const BSONElement &e, int i, bool direction ) const;
        vector< FieldRange > _ranges;
        BSONObj _keyPattern;
        int _direction;
        vector< BSONObj > _queries; // make sure mem owned
    };
        
    // generages FieldRangeSet objects, accounting for or clauses
    class FieldRangeOrSet {
    public:
        FieldRangeOrSet( const char *ns, const BSONObj &query , bool optimize=true );
        // if there's a useless or clause, we won't use or ranges to help with scanning
        bool orFinished() const { return _orFound && _orSets.empty(); }
        // removes first or clause, and removes the field ranges it covers from all subsequent or clauses
        // this could invalidate the result of the last topFrs()
        void popOrClause() {
            massert( 13274, "no or clause to pop", !orFinished() );
            const FieldRangeSet &toPop = _orSets.front();
            list< FieldRangeSet >::iterator i = _orSets.begin();
            ++i;
            while( i != _orSets.end() ) {
                *i -= toPop;
                if( !i->matchPossible() ) {
                    i = _orSets.erase( i );
                } else {    
                    ++i;
                }
            }
            _oldOrSets.push_front( toPop );
            _orSets.pop_front();
        }
        FieldRangeSet *topFrs() const {
            FieldRangeSet *ret = new FieldRangeSet( _baseSet );
            if (_orSets.size()){
                *ret &= _orSets.front();
            }
            return ret;
        }
        void allClausesSimplified( vector< BSONObj > &ret ) const {
            for( list< FieldRangeSet >::const_iterator i = _orSets.begin(); i != _orSets.end(); ++i ) {
                if ( i->matchPossible() ) {
                    ret.push_back( i->simplifiedQuery() );
                }
            }
        }
        string getSpecial() const { return _baseSet.getSpecial(); }

        bool moreOrClauses() const { return !_orSets.empty(); }
    private:
        FieldRangeSet _baseSet;
        list< FieldRangeSet > _orSets;
        list< FieldRangeSet > _oldOrSets; // make sure memory is owned
        bool _orFound;
    };
    
    /**
       used for doing field limiting
     */
    class FieldMatcher {
    public:
        FieldMatcher()
            : _include(true)
            , _special(false)
            , _includeID(true)
            , _skip(0)
            , _limit(-1)
        {}
        
        void add( const BSONObj& o );

        void append( BSONObjBuilder& b , const BSONElement& e ) const;

        BSONObj getSpec() const;
        bool includeID() { return _includeID; }
    private:

        void add( const string& field, bool include );
        void add( const string& field, int skip, int limit );
        void appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested=false) const;

        bool _include; // true if default at this level is to include
        bool _special; // true if this level can't be skipped or included without recursing
        //TODO: benchmark vector<pair> vs map
        typedef map<string, boost::shared_ptr<FieldMatcher> > FieldMap;
        FieldMap _fields;
        BSONObj _source;
        bool _includeID;

        // used for $slice operator
        int _skip;
        int _limit;
    };

    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix=NULL);

    /** returns the upper bound of a query that matches prefix */
    string simpleRegexEnd( string prefix );

    long long applySkipLimit( long long num , const BSONObj& cmd );

} // namespace mongo
