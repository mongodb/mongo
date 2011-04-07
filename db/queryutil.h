// queryutil.h - Utility classes representing ranges of valid BSONElement values for a query.

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
#include "indexkey.h"

namespace mongo {

    /**
     * One side of an interval of valid BSONElements, specified by a value and a
     * boolean indicating whether the interval includes the value.
     */
    struct FieldBound {
        BSONElement _bound;
        bool _inclusive;
        bool operator==( const FieldBound &other ) const {
            return _bound.woCompare( other._bound ) == 0 &&
                   _inclusive == other._inclusive;
        }
        void flipInclusive() { _inclusive = !_inclusive; }
    };

    /** A closed interval composed of a lower and an upper FieldBound. */
    struct FieldInterval {
        FieldInterval() : _cachedEquality( -1 ) {}
        FieldInterval( const BSONElement& e ) : _cachedEquality( -1 ) {
            _lower._bound = _upper._bound = e;
            _lower._inclusive = _upper._inclusive = true;
        }
        FieldBound _lower;
        FieldBound _upper;
        /** @return true iff no single element can be contained in the interval. */
        bool strictValid() const {
            int cmp = _lower._bound.woCompare( _upper._bound, false );
            return ( cmp < 0 || ( cmp == 0 && _lower._inclusive && _upper._inclusive ) );
        }
        /** @return true iff the interval is an equality constraint. */
        bool equality() const;
        mutable int _cachedEquality;
    };

    /**
     * An ordered list of FieldIntervals expressing constraints on valid
     * BSONElement values for a field.
     */
    class FieldRange {
    public:
        FieldRange( const BSONElement &e = BSONObj().firstElement() , bool isNot=false , bool optimize=true );

        /** @return Range intersection with 'other'. */
        const FieldRange &operator&=( const FieldRange &other );
        /** @return Range union with 'other'. */
        const FieldRange &operator|=( const FieldRange &other );
        /** @return Range of elements elements included in 'this' but not 'other'. */
        const FieldRange &operator-=( const FieldRange &other );
        /** @return true iff this range is a subset of 'other'. */
        bool operator<=( const FieldRange &other );

        /**
         * If there are any valid values for this range, the extreme values can
         * be extracted.
         */
        
        BSONElement min() const { assert( !empty() ); return _intervals[ 0 ]._lower._bound; }
        BSONElement max() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._bound; }
        bool minInclusive() const { assert( !empty() ); return _intervals[ 0 ]._lower._inclusive; }
        bool maxInclusive() const { assert( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._inclusive; }

        /** @return true iff this range expresses a single equality interval. */
        bool equality() const;
        /** @return true if all the intervals for this range are equalities */
        bool inQuery() const;
        /** @return true iff this range does not include every BSONElement */
        bool nontrivial() const;
        /** @return true iff this range matches no BSONElements. */
        bool empty() const { return _intervals.empty(); }
        
        /** Empty the range so it matches no BSONElements. */
        void makeEmpty() { _intervals.clear(); }
        const vector< FieldInterval > &intervals() const { return _intervals; }
        string getSpecial() const { return _special; }
        /** Make component intervals noninclusive. */
        void setExclusiveBounds();
        /**
         * Constructs a range where all FieldIntervals and FieldBounds are in
         * the opposite order of the current range.
         * NOTE the resulting intervals may not be strictValid().
         */
        void reverse( FieldRange &ret ) const;
    private:
        BSONObj addObj( const BSONObj &o );
        void finishOperation( const vector< FieldInterval > &newIntervals, const FieldRange &other );
        vector< FieldInterval > _intervals;
        // BSONObj references to keep our BSONElement memory valid.
        vector< BSONObj > _objData;
        string _special;
    };

    /**
     * Implements query pattern matching, used to determine if a query is
     * similar to an earlier query and should use the same plan.
     *
     * Two queries will generate the same QueryPattern, and therefore match each
     * other, if their fields have the same Types and they have the same sort
     * spec.
     */
    class QueryPattern {
    public:
        friend class FieldRangeSet;
        enum Type {
            Equality,
            LowerBound,
            UpperBound,
            UpperAndLowerBound
        };
        bool operator<( const QueryPattern &other ) const;
        /** for testing only */
        bool operator==( const QueryPattern &other ) const;
        /** for testing only */
        bool operator!=( const QueryPattern &other ) const;
    private:
        QueryPattern() {}
        void setSort( const BSONObj sort );
        static BSONObj normalizeSort( const BSONObj &spec );
        map< string, Type > _fieldTypes;
        BSONObj _sort;
    };

    /**
     * A BoundList contains intervals specified by inclusive start
     * and end bounds.  The intervals should be nonoverlapping and occur in
     * the specified direction of traversal.  For example, given a simple index {i:1}
     * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
     * would be valid for index {i:-1} with direction -1.
     */
    typedef vector< pair< BSONObj, BSONObj > > BoundList;

    /**
     * A set of FieldRanges determined from constraints on the fields of a query,
     * that may be used to determine index bounds.
     */
    class FieldRangeSet {
    public:
        friend class FieldRangeOrSet;
        friend class FieldRangeVector;
        FieldRangeSet( const char *ns, const BSONObj &query , bool optimize=true );
        
        /** @return true if there is a nontrivial range for the given field. */
        bool hasRange( const char *fieldName ) const {
            map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
            return f != _ranges.end();
        }
        /** @return range for the given field. */
        const FieldRange &range( const char *fieldName ) const;
        /** @return range for the given field. */
        FieldRange &range( const char *fieldName );
        /** @return the number of nontrivial ranges. */
        int nNontrivialRanges() const;
        /**
         * @return true iff no FieldRanges are empty.
         */
        bool matchPossible() const;
        
        const char *ns() const { return _ns; }
        
        /**
         * @return a simplified query from the extreme values of the nontrivial
         * fields.
         * @param fields If specified, the fields of the returned object are
         * ordered to match those of 'fields'.
         */
        BSONObj simplifiedQuery( const BSONObj &fields = BSONObj() ) const;
        
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
        string getSpecial() const;

        /**
         * @return a FieldRangeSet approximation of the documents in 'this' but
         * not in 'other'.  The approximation will be a superset of the documents
         * in 'this' but not 'other'.
         */
        const FieldRangeSet &operator-=( const FieldRangeSet &other );
        /** @return intersection of 'this' with 'other'. */
        const FieldRangeSet &operator&=( const FieldRangeSet &other );
        
        /**
         * @return an ordered list of bounds generated using an index key pattern
         * and traversal direction.
         *
         * NOTE This function is deprecated in the query optimizer and only
         * currently used by the sharding code.
         */
        BoundList indexBounds( const BSONObj &keyPattern, int direction ) const;

        /**
         * @return - A new FieldRangeSet based on this FieldRangeSet, but with only
         * a subset of the fields.
         * @param fields - Only fields which are represented as field names in this object
         * will be included in the returned FieldRangeSet.
         */
        FieldRangeSet *subset( const BSONObj &fields ) const;
    private:
        void appendQueries( const FieldRangeSet &other );
        void makeEmpty();
        void processQueryField( const BSONElement &e, bool optimize );
        void processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize );
        static FieldRange *__trivialRange;
        static FieldRange &trivialRange();
        mutable map< string, FieldRange > _ranges;
        const char *_ns;
        // make sure memory for FieldRange BSONElements is owned
        vector< BSONObj > _queries;
    };

    class IndexSpec;

    /**
     * An ordered list of fields and their FieldRanges, correspoinding to valid
     * index keys for a given index spec.
     */
    class FieldRangeVector {
    public:
        /**
         * @param frs The valid ranges for all fields, as defined by the query spec
         * @param indexSpec The index spec (key pattern and info)
         * @param direction The direction of index traversal
         */
        FieldRangeVector( const FieldRangeSet &frs, const IndexSpec &indexSpec, int direction );

        /** @return the number of index ranges represented by 'this' */
        long long size();
        /** @return starting point for an index traversal. */
        BSONObj startKey() const;
        /** @return end point for an index traversal. */
        BSONObj endKey() const;
        /** @return a client readable representation of 'this' */
        BSONObj obj() const;
        
        /**
         * @return true iff the provided document matches valid ranges on all
         * of this FieldRangeVector's fields, which is the case iff this document
         * would be returned while scanning the index corresponding to this
         * FieldRangeVector.  This function is used for $or clause deduping.
         */
        bool matches( const BSONObj &obj ) const;
        
        /**
         * Helper class for iterating through an ordered representation of keys
         * to find those keys that match a specified FieldRangeVector.
         */
        class Iterator {
        public:
            Iterator( const FieldRangeVector &v ) : _v( v ), _i( _v._ranges.size(), -1 ), _cmp( _v._ranges.size(), 0 ), _inc( _v._ranges.size(), false ), _after() {
            }
            static BSONObj minObject() {
                BSONObjBuilder b; b.appendMinKey( "" );
                return b.obj();
            }
            static BSONObj maxObject() {
                BSONObjBuilder b; b.appendMaxKey( "" );
                return b.obj();
            }
            /**
             * @return Suggested advance method, based on current key.
             *   -2 Iteration is complete, no need to advance.
             *   -1 Advance to the next key, without skipping.
             *  >=0 Skip parameter.  If @return is r, skip to the key comprised
             *      of the first r elements of curr followed by the (r+1)th and
             *      remaining elements of cmp() (with inclusivity specified by
             *      the (r+1)th and remaining elements of inc()).  If after() is
             *      true, skip past this key not to it.
             */
            int advance( const BSONObj &curr );
            const vector< const BSONElement * > &cmp() const { return _cmp; }
            const vector< bool > &inc() const { return _inc; }
            bool after() const { return _after; }
            void prepDive();
            void setZero( int i ) { for( int j = i; j < (int)_i.size(); ++j ) _i[ j ] = 0; }
            void setMinus( int i ) { for( int j = i; j < (int)_i.size(); ++j ) _i[ j ] = -1; }
            bool ok() { return _i[ 0 ] < (int)_v._ranges[ 0 ].intervals().size(); }
            BSONObj startKey();
            // temp
            BSONObj endKey();
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
        const IndexSpec &_indexSpec;
        int _direction;
        vector< BSONObj > _queries; // make sure mem owned
    };

    /**
     * As we iterate through $or clauses this class generates a FieldRangeSet
     * for the current $or clause, in some cases by excluding ranges that were
     * included in a previous clause.
     */
    class FieldRangeOrSet {
    public:
        FieldRangeOrSet( const char *ns, const BSONObj &query , bool optimize=true );

        /**
         * @return true iff we are done scanning $or clauses.  if there's a
         * useless or clause, we won't use or index ranges to help with scanning.
         */
        bool orFinished() const { return _orFound && _orSets.empty(); }
        /** Iterates to the next $or clause by removing the current $or clause. */
        void popOrClause( const BSONObj &indexSpec = BSONObj() );
        /** @return FieldRangeSet for the current $or clause. */
        FieldRangeSet *topFrs() const;
        /**
         * @return original FieldRangeSet for the current $or clause. While the
         * original bounds are looser, they are composed of fewer ranges and it
         * is faster to do operations with them; when they can be used instead of
         * more precise bounds, they should.
         */
        FieldRangeSet *topFrsOriginal() const;
        
        /** @ret a returned vector of simplified queries for all clauses. */
        void allClausesSimplified( vector< BSONObj > &ret ) const;
        string getSpecial() const { return _baseSet.getSpecial(); }

        bool moreOrClauses() const { return !_orSets.empty(); }
    private:
        FieldRangeSet _baseSet;
        list< FieldRangeSet > _orSets;
        list< FieldRangeSet > _originalOrSets;
        list< FieldRangeSet > _oldOrSets; // make sure memory is owned
        bool _orFound;
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

#include "queryutil-inl.h"
