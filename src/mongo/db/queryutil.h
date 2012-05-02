// @file queryutil.h - Utility classes representing ranges of valid BSONElement values for a query.

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
#include "projection.h"

namespace mongo {
    
    extern const int MaxBytesToReturnToClientAtOnce;
    
    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
     [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
     */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";
        
        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert( 10102 , "bad order array", !e.eoo());
            uassert( 10103 , "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert( 10104 , "too many ordering elements", *p <= '9');
        }
        
        return b.obj();
    }

    class QueryMessage;
    
    /**
     * this represents a total user query
     * includes fields from the query message, both possible query levels
     * parses everything up front
     */
    class ParsedQuery : boost::noncopyable {
    public:
        ParsedQuery( QueryMessage& qm );
        ParsedQuery( const char* ns , int ntoskip , int ntoreturn , int queryoptions , const BSONObj& query , const BSONObj& fields )
        : _ns( ns ) , _ntoskip( ntoskip ) , _ntoreturn( ntoreturn ) , _options( queryoptions ) {
            init( query );
            initFields( fields );
        }
        
        const char * ns() const { return _ns; }
        bool isLocalDB() const { return strncmp(_ns, "local.", 6) == 0; }
        
        const BSONObj& getFilter() const { return _filter; }
        Projection* getFields() const { return _fields.get(); }
        shared_ptr<Projection> getFieldPtr() const { return _fields; }
        
        int getSkip() const { return _ntoskip; }
        int getNumToReturn() const { return _ntoreturn; }
        bool wantMore() const { return _wantMore; }
        int getOptions() const { return _options; }
        bool hasOption( int x ) const { return ( x & _options ) != 0; }
        
        bool isExplain() const { return _explain; }
        bool isSnapshot() const { return _snapshot; }
        bool returnKey() const { return _returnKey; }
        bool showDiskLoc() const { return _showDiskLoc; }
        
        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        const BSONObj& getOrder() const { return _order; }
        const BSONObj& getHint() const { return _hint; }
        int getMaxScan() const { return _maxScan; }
        
        bool couldBeCommand() const {
            /* we assume you are using findOne() for running a cmd... */
            return _ntoreturn == 1 && strstr( _ns , ".$cmd" );
        }
        
        bool hasIndexSpecifier() const {
            return ! _hint.isEmpty() || ! _min.isEmpty() || ! _max.isEmpty();
        }
        
        /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
         is only a size limit.  The idea is that on a find() where one doesn't use much results,
         we don't return much, but once getmore kicks in, we start pushing significant quantities.
         
         The n limit (vs. size) is important when someone fetches only one small field from big
         objects, which causes massive scanning server-side.
         */
        bool enoughForFirstBatch( int n , int len ) const {
            if ( _ntoreturn == 0 )
                return ( len > 1024 * 1024 ) || n >= 101;
            return n >= _ntoreturn || len > MaxBytesToReturnToClientAtOnce;
        }
        
        bool enough( int n ) const {
            if ( _ntoreturn == 0 )
                return false;
            return n >= _ntoreturn;
        }
        
        bool enoughForExplain( long long n ) const {
            if ( _wantMore || _ntoreturn == 0 ) {
                return false;
            }
            return n >= _ntoreturn;            
        }
        
    private:
        void init( const BSONObj& q ) {
            _reset();
            uassert( 10105 , "bad skip value in query", _ntoskip >= 0);
            
            if ( _ntoreturn < 0 ) {
                /* _ntoreturn greater than zero is simply a hint on how many objects to send back per
                 "cursor batch".
                 A negative number indicates a hard limit.
                 */
                _wantMore = false;
                _ntoreturn = -_ntoreturn;
            }
            
            
            BSONElement e = q["query"];
            if ( ! e.isABSONObj() )
                e = q["$query"];
            
            if ( e.isABSONObj() ) {
                _filter = e.embeddedObject();
                _initTop( q );
            }
            else {
                _filter = q;
            }
        }
        
        void _reset() {
            _wantMore = true;
            _explain = false;
            _snapshot = false;
            _returnKey = false;
            _showDiskLoc = false;
            _maxScan = 0;
        }
        
        void _initTop( const BSONObj& top ) {
            BSONObjIterator i( top );
            while ( i.more() ) {
                BSONElement e = i.next();
                const char * name = e.fieldName();
                
                if ( strcmp( "$orderby" , name ) == 0 ||
                    strcmp( "orderby" , name ) == 0 ) {
                    if ( e.type() == Object ) {
                        _order = e.embeddedObject();
                    }
                    else if ( e.type() == Array ) {
                        _order = transformOrderFromArrayFormat( _order );
                    }
                    else {
                        uasserted(13513, "sort must be an object or array");
                    }
                    continue;
                }
                
                if( *name == '$' ) {
                    name++;
                    if ( strcmp( "explain" , name ) == 0 )
                        _explain = e.trueValue();
                    else if ( strcmp( "snapshot" , name ) == 0 )
                        _snapshot = e.trueValue();
                    else if ( strcmp( "min" , name ) == 0 )
                        _min = e.embeddedObject();
                    else if ( strcmp( "max" , name ) == 0 )
                        _max = e.embeddedObject();
                    else if ( strcmp( "hint" , name ) == 0 )
                        _hint = e.wrap();
                    else if ( strcmp( "returnKey" , name ) == 0 )
                        _returnKey = e.trueValue();
                    else if ( strcmp( "maxScan" , name ) == 0 )
                        _maxScan = e.numberInt();
                    else if ( strcmp( "showDiskLoc" , name ) == 0 )
                        _showDiskLoc = e.trueValue();
                    else if ( strcmp( "comment" , name ) == 0 ) {
                        ; // no-op
                    }
                }
            }
            
            if ( _snapshot ) {
                uassert( 12001 , "E12001 can't sort with $snapshot", _order.isEmpty() );
                uassert( 12002 , "E12002 can't use hint with $snapshot", _hint.isEmpty() );
            }
            
        }
        
        void initFields( const BSONObj& fields ) {
            if ( fields.isEmpty() )
                return;
            _fields.reset( new Projection() );
            _fields->init( fields );
        }
        
        const char * const _ns;
        const int _ntoskip;
        int _ntoreturn;
        BSONObj _filter;
        BSONObj _order;
        const int _options;
        shared_ptr< Projection > _fields;
        bool _wantMore;
        bool _explain;
        bool _snapshot;
        bool _returnKey;
        bool _showDiskLoc;
        BSONObj _min;
        BSONObj _max;
        BSONObj _hint;
        int _maxScan;
    };
    
    /**
     * One side of an interval of BSONElements, defined by a value and a boolean indicating if the
     * interval includes the value.
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

    /** An interval defined between a lower and an upper FieldBound. */
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

        string toString() const;
    };

    /**
     * An ordered list of FieldIntervals expressing constraints on valid
     * BSONElement values for a field.
     */
    class FieldRange {
    public:
        FieldRange( const BSONElement &e , bool singleKey , bool isNot=false , bool optimize=true );

        /** @return Range intersection with 'other'. */
        const FieldRange &operator&=( const FieldRange &other );
        /** @return Range union with 'other'. */
        const FieldRange &operator|=( const FieldRange &other );
        /** @return Range of elements elements included in 'this' but not 'other'. */
        const FieldRange &operator-=( const FieldRange &other );
        /** @return true iff this range is a subset of 'other'. */
        bool operator<=( const FieldRange &other ) const;

        /**
         * If there are any valid values for this range, the extreme values can
         * be extracted.
         */
        
        BSONElement min() const { verify( !empty() ); return _intervals[ 0 ]._lower._bound; }
        BSONElement max() const { verify( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._bound; }
        bool minInclusive() const { verify( !empty() ); return _intervals[ 0 ]._lower._inclusive; }
        bool maxInclusive() const { verify( !empty() ); return _intervals[ _intervals.size() - 1 ]._upper._inclusive; }

        /** @return true iff this range expresses a single equality interval. */
        bool equality() const;
        /**
         * @return true iff this range includes all BSONElements
         * (the range is the universal set of BSONElements).
         */
        bool universal() const;
        /** @return true iff this range includes no BSONElements. */
        bool empty() const { return _intervals.empty(); }
        /**
         * @return true in many cases when this FieldRange describes a finite set of BSONElements,
         * all of which will be matched by the query BSONElement that generated this FieldRange.
         * This attribute is used to implement higher level optimizations and is computed with a
         * simple implementation that identifies common (but not all) cases satisfying the stated
         * properties.
         */
        bool simpleFiniteSet() const { return _simpleFiniteSet; }
        
        /** Empty the range so it includes no BSONElements. */
        void makeEmpty() { _intervals.clear(); }
        const vector<FieldInterval> &intervals() const { return _intervals; }
        string getSpecial() const { return _special; }
        /** Make component intervals noninclusive. */
        void setExclusiveBounds();
        /**
         * Constructs a range where all FieldIntervals and FieldBounds are in
         * the opposite order of the current range.
         * NOTE the resulting intervals might not be strictValid().
         */
        void reverse( FieldRange &ret ) const;

        string toString() const;
    private:
        BSONObj addObj( const BSONObj &o );
        void finishOperation( const vector<FieldInterval> &newIntervals, const FieldRange &other,
                             bool simpleFiniteSet );
        vector<FieldInterval> _intervals;
        // Owns memory for our BSONElements.
        vector<BSONObj> _objData;
        string _special;
        bool _singleKey;
        bool _simpleFiniteSet;
    };
    
    /**
     * A BoundList contains intervals specified by inclusive start
     * and end bounds.  The intervals should be nonoverlapping and occur in
     * the specified direction of traversal.  For example, given a simple index {i:1}
     * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
     * would be valid for index {i:-1} with direction -1.
     */
    typedef vector<pair<BSONObj,BSONObj> > BoundList;

    class QueryPattern;
    
    /**
     * A set of FieldRanges determined from constraints on the fields of a query,
     * that may be used to determine index bounds.
     */
    class FieldRangeSet {
    public:
        friend class OrRangeGenerator;
        friend class FieldRangeVector;
        FieldRangeSet( const char *ns, const BSONObj &query , bool singleKey , bool optimize=true );
        
        /** @return range for the given field. */
        const FieldRange &range( const char *fieldName ) const;
        /** @return range for the given field. */
        FieldRange &range( const char *fieldName );
        /** @return the number of non universal ranges. */
        int numNonUniversalRanges() const;
        /** @return the field ranges comprising this set. */
        const map<string,FieldRange> &ranges() const { return _ranges; }
        /** 
         * @return true if a match could be possible on every field. Generally this
         * is not useful information for a single key FieldRangeSet and
         * matchPossibleForIndex() should be used instead.
         */
        bool matchPossible() const;
        /**
         * @return true if a match could be possible given the value of _singleKey
         * and index key 'keyPattern'.
         * @param keyPattern May be {} or {$natural:1} for a non index scan.
         */
        bool matchPossibleForIndex( const BSONObj &keyPattern ) const;
        /**
         * @return true in many cases when this FieldRangeSet describes a finite set of BSONObjs,
         * all of which will be matched by the query BSONObj that generated this FieldRangeSet.
         * This attribute is used to implement higher level optimizations and is computed with a
         * simple implementation that identifies common (but not all) cases satisfying the stated
         * properties.
         */
        bool simpleFiniteSet() const { return _simpleFiniteSet; }
        
        const char *ns() const { return _ns.c_str(); }
        
        /**
         * @return a simplified query from the extreme values of the non universal
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
         * The value of matchPossible() should be true, otherwise this function
         * may @throw.
         *
         * NOTE This function is deprecated in the query optimizer and only
         * currently used by sharding code.
         */
        BoundList indexBounds( const BSONObj &keyPattern, int direction ) const;

        /**
         * @return - A new FieldRangeSet based on this FieldRangeSet, but with only
         * a subset of the fields.
         * @param fields - Only fields which are represented as field names in this object
         * will be included in the returned FieldRangeSet.
         */
        FieldRangeSet *subset( const BSONObj &fields ) const;
        
        bool singleKey() const { return _singleKey; }
        
        BSONObj originalQuery() const { return _queries[ 0 ]; }
        
        string toString() const;
    private:
        void appendQueries( const FieldRangeSet &other );
        void makeEmpty();
        void processQueryField( const BSONElement &e, bool optimize );
        void processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize );
        /** Must be called when a match element is skipped or modified to generate a FieldRange. */
        void adjustMatchField();
        void intersectMatchField( const char *fieldName, const BSONElement &matchElement,
                                 bool isNot, bool optimize );
        static FieldRange *__singleKeyUniversalRange;
        static FieldRange *__multiKeyUniversalRange;
        const FieldRange &universalRange() const;
        map<string,FieldRange> _ranges;
        string _ns;
        // Owns memory for FieldRange BSONElements.
        vector<BSONObj> _queries;
        bool _singleKey;
        bool _simpleFiniteSet;
    };

    class NamespaceDetails;
    
    /**
     * A pair of FieldRangeSets, one representing constraints for single key
     * indexes and the other representing constraints for multi key indexes and
     * unindexed scans.  In several member functions the caller is asked to
     * supply an index so that the implementation may utilize the proper
     * FieldRangeSet and return results that are appropriate with respect to that
     * supplied index.
     */
    class FieldRangeSetPair {
    public:
        FieldRangeSetPair( const char *ns, const BSONObj &query, bool optimize=true )
        :_singleKey( ns, query, true, optimize ), _multiKey( ns, query, false, optimize ) {}

        /**
         * @return the appropriate single or multi key FieldRangeSet for the specified index.
         * @param idxNo -1 for non index scan.
         */
        const FieldRangeSet &frsForIndex( const NamespaceDetails* nsd, int idxNo ) const;

        /** @return a field range in the single key FieldRangeSet. */
        const FieldRange &shardKeyRange( const char *fieldName ) const {
            return _singleKey.range( fieldName );
        }
        /** @return true if the range limits are equivalent to an empty query. */
        bool noNonUniversalRanges() const;
        /** @return false if a match is impossible regardless of index. */
        bool matchPossible() const { return _multiKey.matchPossible(); }
        /**
         * @return false if a match is impossible on the specified index.
         * @param idxNo -1 for non index scan.
         */
        bool matchPossibleForIndex( NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) const;
        
        const char *ns() const { return _singleKey.ns(); }

        string getSpecial() const { return _singleKey.getSpecial(); }

        /** Intersect with another FieldRangeSetPair. */
        FieldRangeSetPair &operator&=( const FieldRangeSetPair &other );
        /**
         * Subtract a FieldRangeSet, generally one expressing a range that has
         * already been scanned.
         */
        FieldRangeSetPair &operator-=( const FieldRangeSet &scanned );

        BoundList shardKeyIndexBounds( const BSONObj &keyPattern ) const {
            return _singleKey.indexBounds( keyPattern, 1 );
        }
        
        bool matchPossibleForShardKey( const BSONObj &keyPattern ) const {
            return _singleKey.matchPossibleForIndex( keyPattern );
        }
        
        BSONObj originalQuery() const { return _singleKey.originalQuery(); }

        string toString() const;
    private:
        FieldRangeSetPair( const FieldRangeSet &singleKey, const FieldRangeSet &multiKey )
        :_singleKey( singleKey ), _multiKey( multiKey ) {}
        void assertValidIndex( const NamespaceDetails *d, int idxNo ) const;
        void assertValidIndexOrNoIndex( const NamespaceDetails *d, int idxNo ) const;
        /** matchPossibleForIndex() must be true. */
        BSONObj simplifiedQueryForIndex( NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) const;        
        FieldRangeSet _singleKey;
        FieldRangeSet _multiKey;
        friend class OrRangeGenerator;
        friend struct QueryUtilIndexed;
    };
    
    class IndexSpec;

    /**
     * An ordered list of fields and their FieldRanges, corresponding to valid
     * index keys for a given index spec.
     */
    class FieldRangeVector {
    public:
        /**
         * @param frs The valid ranges for all fields, as defined by the query spec.  None of the
         * fields in indexSpec may be empty() ranges of frs.
         * @param indexSpec The index spec (key pattern and info)
         * @param direction The direction of index traversal
         */
        FieldRangeVector( const FieldRangeSet &frs, const IndexSpec &indexSpec, int direction );

        /** @return the number of index ranges represented by 'this' */
        unsigned size();
        /** @return starting point for an index traversal. */
        BSONObj startKey() const;
        /** @return end point for an index traversal. */
        BSONObj endKey() const;
        /** @return a client readable representation of 'this' */
        BSONObj obj() const;
        
        const IndexSpec& getSpec(){ return _indexSpec; }

        /**
         * @return true iff the provided document matches valid ranges on all
         * of this FieldRangeVector's fields, which is the case iff this document
         * would be returned while scanning the index corresponding to this
         * FieldRangeVector.  This function is used for $or clause deduping.
         */
        bool matches( const BSONObj &obj ) const;
        
        /**
         * @return first key of 'obj' that would be encountered by a forward
         * index scan using this FieldRangeVector, BSONObj() if no such key.
         */
        BSONObj firstMatch( const BSONObj &obj ) const;
        
        string toString() const;
        
    private:
        int matchingLowElement( const BSONElement &e, int i, bool direction, bool &lowEquality ) const;
        bool matchesElement( const BSONElement &e, int i, bool direction ) const;
        bool matchesKey( const BSONObj &key ) const;
        vector<FieldRange> _ranges;
        const IndexSpec _indexSpec;
        int _direction;
        vector<BSONObj> _queries; // make sure mem owned
        friend class FieldRangeVectorIterator;
    };
    
    /**
     * Helper class for iterating through an ordered representation of keys
     * to find those keys that match a specified FieldRangeVector.
     */
    class FieldRangeVectorIterator {
    public:
        /**
         * @param v - a FieldRangeVector representing matching keys.
         * @param singleIntervalLimit - The maximum number of keys to match a single (compound)
         *     interval before advancing to the next interval.  Limit checking is disabled if 0 and
         *     must be disabled if v contains FieldIntervals that are not equality().
         */
        FieldRangeVectorIterator( const FieldRangeVector &v, int singleIntervalLimit );

        /**
         * @return Suggested advance method through an ordered list of keys with lookup support
         *      (generally a btree).
         *   -2 Iteration is complete, no need to advance further.
         *   -1 Advance to the next ordered key, without skipping.
         *  >=0 Skip parameter, let's call it 'r'.  If after() is true, skip past the key prefix
         *      comprised of the first r elements of curr.  For example, if curr is {a:1,b:1}, the
         *      index is {a:1,b:1}, the direction is 1, and r == 1, skip past {a:1,b:MaxKey}.  If
         *      after() is false, skip to the key comprised of the first r elements of curr followed
         *      by the (r+1)th and greater elements of cmp() (with inclusivity specified by the
         *      (r+1)th and greater elements of inc()).  For example, if curr is {a:1,b:1}, the
         *      index is {a:1,b:1}, the direction is 1, r == 1, cmp()[1] == b:4, and inc()[1] ==
         *      true, then skip to {a:1,b:4}.  Note that the element field names in curr and cmp()
         *      should generally be ignored when performing index key comparisons.
         * @param curr The key at the current position in the list of keys.  Values of curr must be
         *      supplied in order.
         */
        int advance( const BSONObj &curr );
        const vector<const BSONElement *> &cmp() const { return _cmp; }
        const vector<bool> &inc() const { return _inc; }
        bool after() const { return _after; }
        void prepDive();

        /**
         * Helper class representing a position within a vector of ranges.  Public for testing.
         */
        class CompoundRangeCounter {
        public:
            CompoundRangeCounter( int size, int singleIntervalLimit );
            int size() const { return (int)_i.size(); }
            int get( int i ) const { return _i[ i ]; }
            void set( int i, int newVal );
            void inc( int i );
            void setZeroes( int i );
            void setUnknowns( int i );
            void incSingleIntervalCount() {
                if ( isTrackingIntervalCounts() ) ++_singleIntervalCount;
            }
            bool hasSingleIntervalCountReachedLimit() const {
                return isTrackingIntervalCounts() && _singleIntervalCount >= _singleIntervalLimit;
            }
            void resetIntervalCount() { _singleIntervalCount = 0; }
        private:
            bool isTrackingIntervalCounts() const { return _singleIntervalLimit > 0; }
            vector<int> _i;
            int _singleIntervalCount;
            int _singleIntervalLimit;
        };

        /**
         * Helper class for matching a BSONElement with the bounds of a FieldInterval.  Some
         * internal comparison results are cached. Public for testing.
         */
        class FieldIntervalMatcher {
        public:
            FieldIntervalMatcher( const FieldInterval &interval, const BSONElement &element,
                                 bool reverse );
            bool isEqInclusiveUpperBound() const {
                return upperCmp() == 0 && _interval._upper._inclusive;
            }
            bool isGteUpperBound() const { return upperCmp() >= 0; }
            bool isEqExclusiveLowerBound() const {
                return lowerCmp() == 0 && !_interval._lower._inclusive;
            }
            bool isLtLowerBound() const { return lowerCmp() < 0; }
        private:
            struct BoundCmp {
                BoundCmp() : _cmp(), _valid() {}
                void set( int cmp ) { _cmp = cmp; _valid = true; }
                int _cmp;
                bool _valid;
            };
            int mayReverse( int val ) const { return _reverse ? -val : val; }
            int cmp( const BSONElement &bound ) const {
                return mayReverse( _element.woCompare( bound, false ) );
            }
            void setCmp( BoundCmp &boundCmp, const BSONElement &bound ) const {
                boundCmp.set( cmp( bound ) );
            }
            int lowerCmp() const;
            int upperCmp() const;
            const FieldInterval &_interval;
            const BSONElement &_element;
            bool _reverse;
            mutable BoundCmp _lowerCmp;
            mutable BoundCmp _upperCmp;
        };

    private:
        /**
         * @return values similar to advance()
         *   -2 Iteration is complete for the current interval.
         *   -1 Iteration is not complete for the current interval.
         *  >=0 Return value to be forwarded by advance().
         */
        int validateCurrentInterval( int intervalIdx, const BSONElement &currElt,
                                    bool reverse, bool first, bool &eqInclusiveUpperBound );
        
        /** Skip to curr / i / nextbounds. */
        int advanceToLowerBound( int i );
        /** Skip to curr / i / superlative. */
        int advancePast( int i );
        /** Skip to curr / i / superlative and reset following interval positions. */
        int advancePastZeroed( int i );

        bool hasReachedLimitForLastInterval( int intervalIdx ) const {
            return _i.hasSingleIntervalCountReachedLimit() && ( intervalIdx + 1 == _i.size() );
        }

        const FieldRangeVector &_v;
        CompoundRangeCounter _i;
        vector<const BSONElement*> _cmp;
        vector<bool> _inc;
        bool _after;
    };
    
    /**
     * As we iterate through $or clauses this class generates a FieldRangeSetPair
     * for the current $or clause, in some cases by excluding ranges that were
     * included in a previous clause.
     */
    class OrRangeGenerator {
    public:
        OrRangeGenerator( const char *ns, const BSONObj &query , bool optimize=true );

        /** @return true iff we are done scanning $or clauses, or if there are no $or clauses. */
        bool orRangesExhausted() const { return _orSets.empty(); }
        /** Iterates to the next $or clause by removing the current $or clause. */
        void popOrClause( NamespaceDetails *nsd, int idxNo, const BSONObj &keyPattern );
        void popOrClauseSingleKey();
        /** @return FieldRangeSetPair for the current $or clause. */
        FieldRangeSetPair *topFrsp() const;
        /**
         * @return original FieldRangeSetPair for the current $or clause. While the
         * original bounds are looser, they are composed of fewer ranges and it
         * is faster to do operations with them; when they can be used instead of
         * more precise bounds, they should.
         */
        FieldRangeSetPair *topFrspOriginal() const;
        
        string getSpecial() const { return _baseSet.getSpecial(); }
    private:
        void assertMayPopOrClause();
        void _popOrClause( const FieldRangeSet *toDiff, NamespaceDetails *d, int idxNo, const BSONObj &keyPattern );
        FieldRangeSetPair _baseSet;
        list<FieldRangeSetPair> _orSets;
        list<FieldRangeSetPair> _originalOrSets;
        // ensure memory is owned
        list<FieldRangeSetPair> _oldOrSets;
        bool _orFound;
        friend struct QueryUtilIndexed;
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
    
    bool isSimpleIdQuery( const BSONObj& query );

} // namespace mongo

#include "queryutil-inl.h"
