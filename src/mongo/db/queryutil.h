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
    
    //maximum number of intervals produced by $in queries.
    static const unsigned MAX_IN_COMBINATIONS = 4000000;

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
        bool hasReadPref() const { return _hasReadPref; }
        
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

            _filter = _filter.getOwned();

            _hasReadPref = q.hasField("$readPreference");
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
            _fields->init( fields.getOwned() );
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
        bool _hasReadPref;
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

    // Keep track of what special indices we're using.  This can be nontrivial
    // because an index could be required by one operator but not by another.
    struct SpecialIndices {
        // Unlike true/false, this is readable.  :)
        enum IndexRequired {
            INDEX_REQUIRED,
            NO_INDEX_REQUIRED,
        };
        map<string, bool> _indexRequired;

        bool has(const string& name) const {
            return _indexRequired.end() != _indexRequired.find(name);
        }

        SpecialIndices combineWith(const SpecialIndices& other) {
            SpecialIndices ret = *this;
            for (map<string, bool>::const_iterator it = other._indexRequired.begin();
                 it != other._indexRequired.end(); ++it) {
                ret._indexRequired[it->first] = ret._indexRequired[it->first] || it->second;
            }
            return ret;
        }


        void add(const string& name, IndexRequired req) {
            _indexRequired[name] = _indexRequired[name] || (req == INDEX_REQUIRED);
        }

        bool allRequireIndex() const {
            for (map<string, bool>::const_iterator it = _indexRequired.begin();
                 it != _indexRequired.end(); ++it) {
                if (!it->second) { return false; }
            }
            return true;
        }

        bool empty() const { return _indexRequired.empty(); }
        string toString() const {
            stringstream ss;
            for (map<string, bool>::const_iterator it = _indexRequired.begin();
                 it != _indexRequired.end(); ++it) {
                ss << it->first;
                ss << (it->second ? " (needs index)" : " (no index needed)");
                ss << ", ";
            }
            return ss.str();
        }
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
        /**
         * @return true when the interval can contain one or more values.
         * NOTE May also return true in certain 'empty' discrete cases like x > false && x < true.
         */
        bool isStrictValid() const {
            int cmp = _lower._bound.woCompare( _upper._bound, false );
            return ( cmp < 0 || ( cmp == 0 && _lower._inclusive && _upper._inclusive ) );
        }
        /** @return true if the interval is an equality constraint. */
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
        /**
         * Creates a FieldRange representing a superset of the BSONElement values matching a query
         *     expression element.
         * @param e - The query expression element.
         * @param isNot - Indicates that 'e' appears within a query $not clause and its matching
         *     semantics are inverted.
         * @param optimize - If true, the range may be bracketed by 'e''s data type.
         *     TODO It is unclear why 'optimize' is optional, see SERVER-5165.
         */
        FieldRange( const BSONElement &e , bool isNot, bool optimize );

        void setElemMatchContext( const BSONElement& elemMatchContext ) {
            _elemMatchContext = elemMatchContext;
        }

        /**
         * @return Range intersection with 'other'.
         * @param singleKey - Indicate whether intersection will be performed in a single value or
         *     multi value context.
         */
        const FieldRange &intersect( const FieldRange &other, bool singleKey );
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
         * @return true in many cases when this FieldRange represents the exact set of BSONElement
         * values matching the query expression element used to construct the FieldRange.  This
         * attribute is used to implement higher level optimizations and is computed with a simple
         * implementation that identifies common (but not all) cases of this property and may return
         * false negatives.
         */
        bool mustBeExactMatchRepresentation() const { return _exactMatchRepresentation; }
        /* Checks whether this FieldRange is a non-empty union of point-intervals.
         * Examples:
         *  FieldRange( { a:3 } ), isPointIntervalSet() -> true
         *  FieldRange( { a:{ $in:[ 1, 2 ] } } ), isPointIntervalSet() -> true
         *  FieldRange( { a:{ $gt:5 } } ), isPointIntervalSet() -> false
         *  FieldRange( {} ), isPointIntervalSet() -> false
         */
        bool isPointIntervalSet() const;
        const BSONElement& elemMatchContext() const { return _elemMatchContext; }
        
        /** Empty the range so it includes no BSONElements. */
        void makeEmpty() { _intervals.clear(); }
        const vector<FieldInterval> &intervals() const { return _intervals; }
        const SpecialIndices& getSpecial() const { return _special; }
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
                              bool exactMatchRepresentation );
        vector<FieldInterval> _intervals;
        // Owns memory for our BSONElements.
        vector<BSONObj> _objData;
        SpecialIndices _special; // Index type name of a non standard (eg '2d') index required by a
                              // parsed query operator (eg '$near').  Could be >1.
        bool _exactMatchRepresentation;
        BSONElement _elemMatchContext; // Parent $elemMatch object of the field constraint that
                                       // generated this FieldRange.  For example if the query is
                                       // { a:{ $elemMatch:{ b:1, c:1 } } }, then the
                                       // _elemMatchContext for the FieldRange on 'a.b' is the query
                                       // element having field name '$elemMatch'.
    };

    class QueryPattern;
    
    /**
     * A set of FieldRanges determined from constraints on the fields of a query,
     * that may be used to determine index bounds.
     */
    class FieldRangeSet {
    public:
        friend class OrRangeGenerator;
        friend class FieldRangeVector;
        /**
         * Creates a FieldRangeSet representing a superset of the documents matching a query.
         * @param ns - The query's namespace.
         * @param query - The query.
         * @param singleKey - Indicates that document fields contain single values (there are no
         *     multiply valued fields).
         * @param optimize - If true, each field's value range may be bracketed by data type.
         *     TODO It is unclear why 'optimize' is optional, see SERVER-5165.
         */
        FieldRangeSet( const char *ns, const BSONObj &query , bool singleKey , bool optimize );

        /** @return range for the given field. */
        const FieldRange &range( const char *fieldName ) const;
        /** @return range for the given field.  Public for testing. */
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
         * @return true in many cases when this FieldRangeSet represents the exact set of BSONObjs
         * matching the query expression used to construct the FieldRangeSet.  This attribute is
         * used to implement higher level optimizations and is computed with a simple implementation
         * that identifies common (but not all) cases of this property and may return false
         * negatives.
         */
        bool mustBeExactMatchRepresentation() const { return _exactMatchRepresentation; }
        
        /* Checks whether this FieldRangeSet is a non-empty union of point-intervals
         * on a given field.
         * Examples:
         *  FieldRangeSet({a : 3}), isPointIntervalSet("a") -> true
         *  FieldRangeSet({a : {$in : [1 , 2]}}), isPointIntervalSet("a") -> true
         *  FieldRangeSet({}), isPointIntervalSet("a") -> false
         *  FieldRangeSet({b : 1}), isPointIntervalSet("a") -> false
         *
         * Used in determining "suitability" for hashedindexes, and also in
         * sharding for determining the relevant shards for a query.
         *
         * TODO: move this into FieldRange instead of FieldRangeSet
         */
        bool isPointIntervalSet( const string& fieldname ) const;

        const char *ns() const { return _ns.c_str(); }
        
        QueryPattern pattern( const BSONObj &sort = BSONObj() ) const;
        SpecialIndices getSpecial() const;

        /**
         * @return a FieldRangeSet approximation of the documents in 'this' but
         * not in 'other'.  The approximation will be a superset of the documents
         * in 'this' but not 'other'.
         */
        const FieldRangeSet &operator-=( const FieldRangeSet &other );
        /** @return intersection of 'this' with 'other'. */
        const FieldRangeSet &operator&=( const FieldRangeSet &other );

        /**
         * @return - A new FieldRangeSet based on this FieldRangeSet, but with only
         * a subset of the fields.
         * @param fields - Only fields which are represented as field names in this object
         * will be included in the returned FieldRangeSet.
         */
        FieldRangeSet *subset( const BSONObj &fields ) const;
        
        /**
         * @return A new FieldRangeSet based on this FieldRangeSet, but with all field names
         * prefixed by the specified @param prefix field name.
         */
        FieldRangeSet* prefixed( const string& prefix ) const;
        
        bool singleKey() const { return _singleKey; }
        
        BSONObj originalQuery() const { return _queries[ 0 ]; }
        
        string toString() const;

    private:
        /**
         * Private constructor for implementation specific delegate objects.
         * @param boundElemMatch - If false, FieldRanges will not be computed for $elemMatch
         *     expressions.
         */
        FieldRangeSet( const char* ns, const BSONObj& query, bool singleKey, bool optimize,
                       bool boundElemMatch );
        /** Initializer shared by the constructors. */
        void init( bool optimize );

        void appendQueries( const FieldRangeSet &other );
        void makeEmpty();

        /**
         * Query parsing routines.
         * TODO integrate these with an external query parser shared by the matcher.  SERVER-1009
         */
        void handleMatchField( const BSONElement& matchElement, bool optimize );
        void handleConjunctionClauses( const BSONObj& clauses, bool optimize );
        void handleOp( const char* matchFieldName, const BSONElement& op, bool isNot,
                       bool optimize );
        void handleNotOp( const char* matchFieldName, const BSONElement& notOp, bool optimize );
        void handleElemMatch( const char* matchFieldName, const BSONElement& elemMatch, bool isNot,
                              bool optimize );

        /** Must be called when a match element is skipped or modified to generate a FieldRange. */
        void adjustMatchField();
        void intersectMatchField( const char *fieldName, const BSONElement &matchElement,
                                 bool isNot, bool optimize );
        static FieldRange *__universalRange;
        const FieldRange &universalRange() const;
        map<string,FieldRange> _ranges;
        string _ns;
        // Owns memory for FieldRange BSONElements.
        vector<BSONObj> _queries;
        bool _singleKey;
        bool _exactMatchRepresentation;
        bool _boundElemMatch;
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

        SpecialIndices getSpecial() const { return _singleKey.getSpecial(); }

        /** Intersect with another FieldRangeSetPair. */
        FieldRangeSetPair &operator&=( const FieldRangeSetPair &other );
        /**
         * Subtract a FieldRangeSet, generally one expressing a range that has
         * already been scanned.
         */
        FieldRangeSetPair &operator-=( const FieldRangeSet &scanned );
        
        bool matchPossibleForSingleKeyFRS( const BSONObj &keyPattern ) const {
            return _singleKey.matchPossibleForIndex( keyPattern );
        }
        
        BSONObj originalQuery() const { return _singleKey.originalQuery(); }

        const FieldRangeSet getSingleKeyFRS() const { return _singleKey; }
        const FieldRangeSet getMultiKeyFRS() const { return _singleKey; }

        string toString() const;
    private:
        FieldRangeSetPair( const FieldRangeSet &singleKey, const FieldRangeSet &multiKey )
        :_singleKey( singleKey ), _multiKey( multiKey ) {}
        void assertValidIndex( const NamespaceDetails *d, int idxNo ) const;
        void assertValidIndexOrNoIndex( const NamespaceDetails *d, int idxNo ) const;
        /** matchPossibleForIndex() must be true. */
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

        /**
         * Methods for identifying compound start and end btree bounds describing this field range
         * vector.
         *
         * A FieldRangeVector contains the FieldRange bounds for every field of an index.  A
         * FieldRangeVectorIterator may be used to efficiently search for btree keys within these
         * bounds.  Alternatively, a single compound field interval of the btree may be scanned,
         * between a compound field start point and end point.  If isSingleInterval() is true then
         * the interval between the start and end points will be an exact description of this
         * FieldRangeVector, otherwise the start/end interval will be a superset of this
         * FieldRangeVector.  For example:
         *
         * index { a:1 }, query { a:{ $gt:2, $lte:4 } }
         *     -> frv ( 2, 4 ]
         *         -> start/end bounds ( { '':2 }, { '':4 } ]
         *
         * index { a:1, b:1 }, query { a:2, b:{ $gte:7, $lt:9 } }
         *     -> frv [ 2, 2 ], [ 7, 9 )
         *         -> start/end bounds [ { '':2, '':7 }, { '':2, '':9 } )
         *
         * index { a:1, b:-1 }, query { a:2, b:{ $gte:7, $lt:9 } }
         *     -> frv [ 2, 2 ], ( 9, 7 ]
         *         -> start/end bounds ( { '':2, '':9 }, { '':2, '':7 } ]
         *
         * index { a:1, b:1 }, query { a:{ $gte:7, $lt:9 } }
         *     -> frv [ 7, 9 )
         *         -> start/end bounds [ { '':7, '':MinKey }, { '':9, '':MinKey } )
         *
         * index { a:1, b:1 }, query { a:{ $gte:2, $lte:5 }, b:{ $gte:7, $lte:9 } }
         *     -> frv [ 2, 5 ], [ 7, 9 ]
         *         -> start/end bounds [ { '':2, '':7 }, { '':5, '':9 } ]
         *            (isSingleInterval() == false)
         */

        /**
         * @return true if this FieldRangeVector represents a single interval within a btree,
         * comprised of all keys between a single start point and a single end point.
         */
        bool isSingleInterval() const;

        /**
         * @return a starting point for an index traversal, a lower bound on the ranges represented
         * by this FieldRangeVector according to the btree's native ordering.
         */
        BSONObj startKey() const;

        /** @return true if the startKey() bound is inclusive. */
        bool startKeyInclusive() const;

        /**
         * @return an end point for an index traversal, an upper bound on the ranges represented
         * by this FieldRangeVector according to the btree's native ordering.
         */
        BSONObj endKey() const;

        /** @return true if the endKey() bound is inclusive. */
        bool endKeyInclusive() const;

        /** @return the number of index ranges represented by 'this' */
        unsigned size() const;

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
         * @return true if all values in the provided index key are contained within the field
         * ranges of their respective fields in this FieldRangeVector.
         *
         * For example, given a query { a:3, b:4 } and index { a:1, b:1 }, the FieldRangeVector is
         * [ [[ 3, 3 ]], [[ 4, 4 ]] ], consisting of field range [[ 3, 3 ]] on field 'a' and
         * [[ 4, 4 ]] on field 'b'.  The index key { '':3, '':4 } matches, but the index key
         * { '':3, '':5 } does not match because the value 5 in the second field is not contained in
         * the field range [[ 4, 4 ]] for field 'b'.
         */
        bool matchesKey( const BSONObj& key ) const;

        /**
         * @return first key of 'obj' that would be encountered by a forward
         * index scan using this FieldRangeVector, BSONObj() if no such key.
         */
        BSONObj firstMatch( const BSONObj &obj ) const;

        /**
         * @return true if all ranges within the field range set on fields of this index are
         * represented in this field range vector.  May be false in certain multikey index cases
         * when intervals on two fields cannot both be used, see comments related to SERVER-958 in
         * FieldRangeVector().
         */
        bool hasAllIndexedRanges() const { return _hasAllIndexedRanges; }

        string toString() const;
        
    private:
        int matchingLowElement( const BSONElement &e, int i, bool direction, bool &lowEquality ) const;
        bool matchesElement( const BSONElement &e, int i, bool direction ) const;
        vector<FieldRange> _ranges;
        const IndexSpec _indexSpec;
        int _direction;
        vector<BSONObj> _queries; // make sure mem owned
        bool _hasAllIndexedRanges;
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
         *     interval before advancing to the next interval.  Limit checking is disabled if 0.
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
            string toString() const;
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
            return
                _i.hasSingleIntervalCountReachedLimit() &&
                ( intervalIdx + 1 == _endNonUniversalRanges );
        }

        /** @return the index of the last non universal range + 1. */
        int endNonUniversalRanges() const;

        const FieldRangeVector &_v;
        CompoundRangeCounter _i;
        vector<const BSONElement*> _cmp;
        vector<bool> _inc;
        bool _after;
        int _endNonUniversalRanges;
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
        
        SpecialIndices getSpecial() const { return _baseSet.getSpecial(); }
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
