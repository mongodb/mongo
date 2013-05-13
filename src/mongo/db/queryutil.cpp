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

#include "mongo/pch.h"

#include "mongo/db/queryutil.h"

#include "mongo/db/index_names.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    extern BSONObj staticNull;
    extern BSONObj staticUndefined;

    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix) {
        string r = "";

        if (purePrefix) *purePrefix = false;

        bool multilineOK;
        if ( regex[0] == '\\' && regex[1] == 'A') {
            multilineOK = true;
            regex += 2;
        }
        else if (regex[0] == '^') {
            multilineOK = false;
            regex += 1;
        }
        else {
            return r;
        }

        bool extended = false;
        while (*flags) {
            switch (*(flags++)) {
            case 'm': // multiline
                if (multilineOK)
                    continue;
                else
                    return r;
            case 'x': // extended
                extended = true;
                break;
            default:
                return r; // cant use index
            }
        }

        stringstream ss;

        while(*regex) {
            char c = *(regex++);
            if ( c == '*' || c == '?' ) {
                // These are the only two symbols that make the last char optional
                r = ss.str();
                r = r.substr( 0 , r.size() - 1 );
                return r; //breaking here fails with /^a?/
            }
            else if (c == '|') {
                // whole match so far is optional. Nothing we can do here.
                return string();
            }
            else if (c == '\\') {
                c = *(regex++);
                if (c == 'Q'){
                    // \Q...\E quotes everything inside
                    while (*regex) {
                        c = (*regex++);
                        if (c == '\\' && (*regex == 'E')){
                            regex++; //skip the 'E'
                            break; // go back to start of outer loop
                        }
                        else {
                            ss << c; // character should match itself
                        }
                    }
                }
                else if ((c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '0') ||
                        (c == '\0')) {
                    // don't know what to do with these
                    r = ss.str();
                    break;
                }
                else {
                    // slash followed by non-alphanumeric represents the following char
                    ss << c;
                }
            }
            else if (strchr("^$.[()+{", c)) {
                // list of "metacharacters" from man pcrepattern
                r = ss.str();
                break;
            }
            else if (extended && c == '#') {
                // comment
                r = ss.str();
                break;
            }
            else if (extended && isspace(c)) {
                continue;
            }
            else {
                // self-matching char
                ss << c;
            }
        }

        if ( r.empty() && *regex == 0 ) {
            r = ss.str();
            if (purePrefix) *purePrefix = !r.empty();
        }

        return r;
    }
    inline string simpleRegex(const BSONElement& e) {
        switch(e.type()) {
        case RegEx:
            return simpleRegex(e.regex(), e.regexFlags());
        case Object: {
            BSONObj o = e.embeddedObject();
            return simpleRegex(o["$regex"].valuestrsafe(), o["$options"].valuestrsafe());
        }
        default: verify(false); return ""; //return squashes compiler warning
        }
    }

    string simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }


    FieldRange::FieldRange( const BSONElement &e, bool isNot, bool optimize ) :
    _exactMatchRepresentation() {
        int op = e.getGtLtOp();

        // NOTE with $not, we could potentially form a complementary set of intervals.
        if ( !isNot && !e.eoo() && e.type() != RegEx && op == BSONObj::opIN ) {
            bool exactMatchesOnly = true;
            set<BSONElement,element_lt> vals;
            vector<FieldRange> regexes;
            uassert( 12580 , "invalid query" , e.isABSONObj() );
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() ) {
                BSONElement ie = i.next();
                uassert( 15881, "$elemMatch not allowed within $in",
                         ie.type() != Object ||
                         ie.embeddedObject().firstElement().getGtLtOp() != BSONObj::opELEM_MATCH );
                if ( ie.type() == RegEx ) {
                    exactMatchesOnly = false;
                    regexes.push_back( FieldRange( ie, false, optimize ) );
                }
                else {
                    // A document array may be indexed by its first element, by undefined
                    // if it is empty, or as a full array if it is embedded within another
                    // array.
                    vals.insert( ie );                        
                    if ( ie.type() == Array ) {
                        exactMatchesOnly = false;
                        BSONElement temp = ie.embeddedObject().firstElement();
                        if ( temp.eoo() ) {
                            temp = staticUndefined.firstElement();
                        }                        
                        vals.insert( temp );
                    }
                    if ( ie.isNull() ) {
                        // A null index key will not always match a null query value (eg
                        // SERVER-4529).  As a result, a field range containing null cannot be an
                        // exact match representation.
                        exactMatchesOnly = false;
                    }
                }
            }

            _exactMatchRepresentation = exactMatchesOnly;
            for( set<BSONElement,element_lt>::const_iterator i = vals.begin(); i != vals.end(); ++i )
                _intervals.push_back( FieldInterval(*i) );

            for( vector<FieldRange>::const_iterator i = regexes.begin(); i != regexes.end(); ++i )
                *this |= *i;

            return;
        }

        // A document array may be indexed by its first element, by undefined
        // if it is empty, or as a full array if it is embedded within another
        // array.
        if ( e.type() == Array && op == BSONObj::Equality ) {

            _intervals.push_back( FieldInterval(e) );
            BSONElement temp = e.embeddedObject().firstElement();
            if ( temp.eoo() ) {
             	temp = staticUndefined.firstElement();
            }
            if ( temp < e ) {
                _intervals.insert( _intervals.begin() , temp );
            }
            else {
                _intervals.push_back( FieldInterval(temp) );
            }

            return;
        }

        _intervals.push_back( FieldInterval() );
        FieldInterval &initial = _intervals[ 0 ];
        BSONElement &lower = initial._lower._bound;
        bool &lowerInclusive = initial._lower._inclusive;
        BSONElement &upper = initial._upper._bound;
        bool &upperInclusive = initial._upper._inclusive;
        lower = minKey.firstElement();
        lowerInclusive = true;
        upper = maxKey.firstElement();
        upperInclusive = true;

        if ( e.eoo() ) {
            _exactMatchRepresentation = true;
            return;
        }

        bool existsSpec = false;
        if ( op == BSONObj::opEXISTS ) {
            existsSpec = e.trueValue();
        }
        
        if ( e.type() == RegEx
                || (e.type() == Object && !e.embeddedObject()["$regex"].eoo())
           ) {
            uassert( 13454, "invalid regular expression operator", op == BSONObj::Equality || op == BSONObj::opREGEX );
            if ( !isNot ) { // no optimization for negated regex - we could consider creating 2 intervals comprising all nonmatching prefixes
                const string r = simpleRegex(e);
                if ( r.size() ) {
                    lower = addObj( BSON( "" << r ) ).firstElement();
                    upper = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                    upperInclusive = false;
                }
                else {
                    BSONObjBuilder b1(32), b2(32);
                    b1.appendMinForType( "" , String );
                    lower = addObj( b1.obj() ).firstElement();

                    b2.appendMaxForType( "" , String );
                    upper = addObj( b2.obj() ).firstElement();
                    upperInclusive = false; //MaxForType String is an empty Object
                }

                // regex matches self - regex type > string type
                if (e.type() == RegEx) {
                    BSONElement re = addObj( BSON( "" << e ) ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                }
                else {
                    BSONObj orig = e.embeddedObject();
                    BSONObjBuilder b;
                    b.appendRegex("", orig["$regex"].valuestrsafe(), orig["$options"].valuestrsafe());
                    BSONElement re = addObj( b.obj() ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                }

            }
            return;
        }

        // Identify simple cases where this FieldRange represents the exact set of BSONElement
        // values matching the query expression element used to construct the FieldRange.

        if ( // If type bracketing is enabled (see 'optimize' case at the end of this function) ...
             optimize &&
             // ... and the operator isn't within a $not clause ...
             !isNot &&
             // ... and the operand is of a type that implements exact type bracketing and will be
             // exactly represented in an index key (eg is not null or an array) ...
             e.isSimpleType() ) {
            switch( op ) {
                // ... and the operator is one for which this constructor will determine exact
                // bounds on the values that match ...
                case BSONObj::Equality:
                case BSONObj::LT:
                case BSONObj::LTE:
                case BSONObj::GT:
                case BSONObj::GTE:
                    // ... then this FieldRange exactly characterizes those documents that match the
                    // operator.
                    _exactMatchRepresentation = true;
                default:
                    break;
            }
        }

        if ( isNot ) {
            switch( op ) {
            case BSONObj::Equality:
                return;
//                    op = BSONObj::NE;
//                    break;
            case BSONObj::opALL:
            case BSONObj::opMOD: // NOTE for mod and type, we could consider having 1-2 intervals comprising the complementary types (multiple intervals already possible with $in)
            case BSONObj::opTYPE:
                // no bound calculation
                return;
            case BSONObj::NE:
                op = BSONObj::Equality;
                break;
            case BSONObj::LT:
                op = BSONObj::GTE;
                break;
            case BSONObj::LTE:
                op = BSONObj::GT;
                break;
            case BSONObj::GT:
                op = BSONObj::LTE;
                break;
            case BSONObj::GTE:
                op = BSONObj::LT;
                break;
            case BSONObj::opEXISTS:
                existsSpec = !existsSpec;
                break;
            default: // otherwise doesn't matter
                break;
            }
        }
        switch( op ) {
        case BSONObj::Equality:
            lower = upper = e;
            break;
        case BSONObj::NE: {
            // this will invalidate the upper/lower references above
            _intervals.push_back( FieldInterval() );
            // optimize doesn't make sense for negative ranges
            _intervals[ 0 ]._upper._bound = e;
            _intervals[ 0 ]._upper._inclusive = false;
            _intervals[ 1 ]._lower._bound = e;
            _intervals[ 1 ]._lower._inclusive = false;
            _intervals[ 1 ]._upper._bound = maxKey.firstElement();
            _intervals[ 1 ]._upper._inclusive = true;
            optimize = false; // don't run optimize code below
            break;
        }
        case BSONObj::LT:
            upperInclusive = false;
        case BSONObj::LTE:
            upper = e;
            break;
        case BSONObj::GT:
            lowerInclusive = false;
        case BSONObj::GTE:
            lower = e;
            break;
        case BSONObj::opALL: {
            uassert( 10370 ,  "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            bool bound = false;
            while ( i.more() ) {
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ) {
                    // taken care of elsewhere
                }
                else if ( x.type() != RegEx ) {
                    lower = upper = x;
                    bound = true;
                    break;
                }
            }
            if ( !bound ) { // if no good non regex bound found, try regex bounds
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement x = i.next();
                    if ( x.type() != RegEx )
                        continue;
                    string simple = simpleRegex( x.regex(), x.regexFlags() );
                    if ( !simple.empty() ) {
                        lower = addObj( BSON( "" << simple ) ).firstElement();
                        upper = addObj( BSON( "" << simpleRegexEnd( simple ) ) ).firstElement();
                        break;
                    }
                }
            }
            break;
        }
        case BSONObj::opMOD: {
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , NumberDouble );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , NumberDouble );
                upper = addObj( b.obj() ).firstElement();
            }
            break;
        }
        case BSONObj::opTYPE: {
            BSONType t = (BSONType)e.numberInt();
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , t );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , t );
                upper = addObj( b.obj() ).firstElement();
            }

            break;
        }
        case BSONObj::opREGEX:
        case BSONObj::opOPTIONS:
            // These operators are handled via their encapsulating object, so no bounds are
            // generated for them individually.
            break;
        case BSONObj::opELEM_MATCH: {
            log() << "warning: shouldn't get here?" << endl;
            break;
        }
        case BSONObj::opWITHIN:
            _special.add(IndexNames::GEO_2D, SpecialIndices::NO_INDEX_REQUIRED);
            _special.add(IndexNames::GEO_2DSPHERE, SpecialIndices::NO_INDEX_REQUIRED);
            break;
        case BSONObj::opNEAR:
            _special.add(IndexNames::GEO_2D, SpecialIndices::INDEX_REQUIRED);
            _special.add(IndexNames::GEO_2DSPHERE, SpecialIndices::INDEX_REQUIRED);
            break;
        case BSONObj::opGEO_INTERSECTS:
            _special.add(IndexNames::GEO_2DSPHERE, SpecialIndices::NO_INDEX_REQUIRED);
            break;
        case BSONObj::opEXISTS: {
            if ( !existsSpec ) {
                lower = upper = staticNull.firstElement();
            }
            optimize = false;
            break;
        }
        default:
            break;
        }

        // If 'optimize' is set, then bracket the field range by bson type.  For example, if this
        // FieldRange is constructed with the operator { $gt:5 }, then the lower bound will be 5
        // at this point but the upper bound will be MaxKey.  If 'optimize' is true, the upper bound
        // is bracketed to the highest possible bson numeric value.  This is consistent with the
        // Matcher's $gt implementation.

        if ( optimize ) {
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ) { // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
                if ( upper.canonicalType() != lower.canonicalType() ) {
                    // _exactMatchRepresentation will be set if lower.isSimpleType(), requiring that
                    // this field range exactly describe the values matching its query operator.  If
                    // lower's max for type is not of the same canonical type as lower, it is
                    // assumed to be the lowest value of the next canonical type meaning the upper
                    // bound should be exclusive.
                    upperInclusive = false;
                }
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ) { // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
                if ( lower.canonicalType() != upper.canonicalType() ) {
                    // _exactMatchRepresentation will be set if upper.isSimpleType(), requiring that
                    // this field range exactly describe the values matching its query operator.  If
                    // upper's min for type is not of the same canonical type as upper, it is
                    // assumed to be the highest value of the previous canonical type meaning the
                    // lower bound should be exclusive.
                    lowerInclusive = false;
                }
            }
        }

    }

    void FieldRange::finishOperation( const vector<FieldInterval> &newIntervals,
                                     const FieldRange &other, bool exactMatchRepresentation ) {
        _intervals = newIntervals;
        for( vector<BSONObj>::const_iterator i = other._objData.begin(); i != other._objData.end(); ++i )
            _objData.push_back( *i );
        if (_special.empty() && !other._special.empty())
            _special = other._special;
        _exactMatchRepresentation = exactMatchRepresentation;
        // A manipulated FieldRange may no longer be valid within a parent context.
        _elemMatchContext = BSONElement();
    }

    /** @return the maximum of two lower bounds, considering inclusivity. */
    static FieldBound maxLowerBound( const FieldBound& a, const FieldBound& b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp < 0 )
            return b;
        return a;
    }

    /** @return the minimum of two upper bounds, considering inclusivity. */
    static FieldBound minUpperBound( const FieldBound& a, const FieldBound& b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp > 0 )
            return b;
        return a;
    }

    /**
     * @return true when the overlap of two intervals is a valid interval.
     * @param one, @param two - The intervals.
     * @param result - The resulting overlap.
     */
    static bool fieldIntervalOverlap( const FieldInterval& one, const FieldInterval& two,
                                      FieldInterval& result ) {
        result._lower = maxLowerBound( one._lower, two._lower );
        result._upper = minUpperBound( one._upper, two._upper );
        return result.isStrictValid();
    }

    const FieldRange &FieldRange::intersect( const FieldRange &other, bool singleKey ) {
        // If 'this' FieldRange is universal(), intersect by copying the 'other' range into 'this'.
        if ( universal() ) {
            SpecialIndices intersectSpecial = _special.combineWith(other._special);
            *this = other;
            _special = intersectSpecial;
            return *this;
        }
        // Range intersections are not taken for multikey indexes.  See SERVER-958.
        if ( !singleKey && !universal() ) {
            SpecialIndices intersectSpecial = _special.combineWith(other._special);
            // Pick 'other' range if it is smaller than or equal to 'this'.
            if ( other <= *this ) {
             	*this = other;
            }
            _exactMatchRepresentation = false;
            _special = intersectSpecial;
            return *this;
        }
        vector<FieldInterval> newIntervals;
        vector<FieldInterval>::const_iterator i = _intervals.begin();
        vector<FieldInterval>::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) ) {
                // If the two intervals overlap, add the overlap to the result.
                newIntervals.push_back( overlap );
            }
            // Increment the iterator with the current interval having the lower upper bound.  The
            // next interval of this iterator may overlap with the current interval of the other
            // iterator.
            if ( i->_upper == minUpperBound( i->_upper, j->_upper ) ) {
                ++i;
            }
            else {
                ++j;
            }
        }
        finishOperation( newIntervals, other,
                         mustBeExactMatchRepresentation() &&
                         other.mustBeExactMatchRepresentation() );
        return *this;
    }

    /** Helper class for assembling a union of FieldRange objects. */
    class RangeUnionBuilder : boost::noncopyable {
    public:
        RangeUnionBuilder() : _initial( true ) {}
        /** @param next: Supply next ordered interval, ordered by _lower FieldBound. */
        void nextOrderedInterval( const FieldInterval &next ) {
            if ( _initial ) {
                _tail = next;
                _initial = false;
                return;
            }
            if ( !handleDisjoint( next ) ) {
                handleExtend( next );
            }
        }
        void done() {
            if ( !_initial ) {
                _unionIntervals.push_back( _tail );
            }
        }
        const vector<FieldInterval> &unionIntervals() const { return _unionIntervals; }
    private:
        /** If _tail and next are disjoint, next becomes the new _tail. */
        bool handleDisjoint( const FieldInterval &next ) {
            int cmp = _tail._upper._bound.woCompare( next._lower._bound, false );
            if ( ( cmp < 0 ) ||
                ( cmp == 0 && !_tail._upper._inclusive && !next._lower._inclusive ) ) {
                _unionIntervals.push_back( _tail );
                _tail = next;
                return true;
            }
            return false;
        }
        /** Extend _tail to upper bound of next if necessary. */
        void handleExtend( const FieldInterval &next ) {
            int cmp = _tail._upper._bound.woCompare( next._upper._bound, false );
            if ( ( cmp < 0 ) ||
                ( cmp == 0 && !_tail._upper._inclusive && next._upper._inclusive ) ) {
                _tail._upper = next._upper;
            }            
        }
        bool _initial;
        FieldInterval _tail;
        vector<FieldInterval> _unionIntervals;
    };

    const FieldRange &FieldRange::operator|=( const FieldRange &other ) {
        RangeUnionBuilder b;
        vector<FieldInterval>::const_iterator i = _intervals.begin();
        vector<FieldInterval>::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( cmp < 0 || ( cmp == 0 && i->_lower._inclusive ) ) {
                b.nextOrderedInterval( *i++ );
            }
            else {
                b.nextOrderedInterval( *j++ );
            }
        }
        while( i != _intervals.end() ) {
            b.nextOrderedInterval( *i++ );
        }
        while( j != other._intervals.end() ) {
            b.nextOrderedInterval( *j++ );
        }
        b.done();
        finishOperation( b.unionIntervals(), other, false );
        return *this;
    }

    const FieldRange &FieldRange::operator-=( const FieldRange &other ) {
        vector<FieldInterval> newIntervals;
        vector<FieldInterval>::iterator i = _intervals.begin();
        vector<FieldInterval>::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( cmp < 0 ||
                    ( cmp == 0 && i->_lower._inclusive && !j->_lower._inclusive ) ) {
                int cmp2 = i->_upper._bound.woCompare( j->_lower._bound, false );
                if ( cmp2 < 0 ) {
                    newIntervals.push_back( *i );
                    ++i;
                }
                else if ( cmp2 == 0 ) {
                    newIntervals.push_back( *i );
                    if ( newIntervals.back()._upper._inclusive && j->_lower._inclusive ) {
                        newIntervals.back()._upper._inclusive = false;
                    }
                    ++i;
                }
                else {
                    newIntervals.push_back( *i );
                    newIntervals.back()._upper = j->_lower;
                    newIntervals.back()._upper.flipInclusive();
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                            ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        ++i;
                    }
                    else {
                        i->_lower = j->_upper;
                        i->_lower.flipInclusive();
                        ++j;
                    }
                }
            }
            else {
                int cmp2 = i->_lower._bound.woCompare( j->_upper._bound, false );
                if ( cmp2 > 0 ||
                        ( cmp2 == 0 && ( !i->_lower._inclusive || !j->_upper._inclusive ) ) ) {
                    ++j;
                }
                else {
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                            ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        ++i;
                    }
                    else {
                        i->_lower = j->_upper;
                        i->_lower.flipInclusive();
                        ++j;
                    }
                }
            }
        }
        while( i != _intervals.end() ) {
            newIntervals.push_back( *i );
            ++i;
        }
        finishOperation( newIntervals, other, false );
        return *this;
    }

    // TODO write a proper implementation that doesn't do a full copy
    bool FieldRange::operator<=( const FieldRange &other ) const {
        FieldRange temp = *this;
        temp -= other;
        return temp.empty();
    }

    bool FieldRange::universal() const {
        if ( empty() ) {
            return false;
        }
        if ( minKey.firstElement().woCompare( min(), false ) != 0 ) {
            return false;
        }
        if ( maxKey.firstElement().woCompare( max(), false ) != 0 ) {
            return false;
        }
        // TODO ensure that adjacent intervals are not possible (the two intervals should be
        // merged), and just determine if the range is universal by testing _intervals.size() == 1.
        for ( unsigned i = 1; i < _intervals.size(); ++i ) {
            const FieldBound &prev = _intervals[ i-1 ]._upper;
            const FieldBound &curr = _intervals[ i ]._lower;
            if ( !prev._inclusive && !curr._inclusive ) {
                return false;
            }
            if ( prev._bound.woCompare( curr._bound ) < 0 ) {
                return false;
            }
        }
        return true;
    }

    bool FieldRange::isPointIntervalSet() const {
        if ( _intervals.empty() ) {
            return false;
        }
        for( vector<FieldInterval>::const_iterator i = _intervals.begin(); i != _intervals.end();
             ++i ) {
            if ( !i->equality() ) {
                return false;
            }
        }
        return true;
    }

    void FieldRange::setExclusiveBounds() {
        for( vector<FieldInterval>::iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            i->_lower._inclusive = false;
            i->_upper._inclusive = false;
        }
    }

    void FieldRange::reverse( FieldRange &ret ) const {
        verify( _special.empty() );
        ret._intervals.clear();
        ret._objData = _objData;
        for( vector<FieldInterval>::const_reverse_iterator i = _intervals.rbegin(); i != _intervals.rend(); ++i ) {
            FieldInterval fi;
            fi._lower = i->_upper;
            fi._upper = i->_lower;
            ret._intervals.push_back( fi );
        }
    }
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        _objData.push_back( o );
        return o;
    }

    string FieldInterval::toString() const {
        StringBuilder buf;
        buf << ( _lower._inclusive ? "[" : "(" ) << " ";
        buf << _lower._bound.toString( false );
        buf << " , ";
        buf << _upper._bound.toString( false );
        buf << " " << ( _upper._inclusive ? "]" : ")" );
        return buf.str();
    }


    string FieldRange::toString() const {
        StringBuilder buf;
        buf << "(FieldRange special: { " << _special.toString() << "} intervals: ";
        for (vector<FieldInterval>::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i) {
            buf << i->toString() << " ";
        }
        buf << ")";
        return buf.str();
    }

    SpecialIndices FieldRangeSet::getSpecial() const {
        for (map<string, FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); i++) {
            if (!i->second.getSpecial().empty()) {
                return i->second.getSpecial();
            }
        }
        return SpecialIndices();
    }

    /**
     * Btree scanning for a multidimentional key range will yield a
     * multidimensional box.  The idea here is that if an 'other'
     * multidimensional box contains the current box we don't have to scan
     * the current box.  If the 'other' box contains the current box in
     * all dimensions but one, we can safely subtract the values of 'other'
     * along that one dimension from the values for the current box on the
     * same dimension.  In other situations, subtracting the 'other'
     * box from the current box yields a result that is not a box (but
     * rather can be expressed as a union of boxes).  We don't support
     * such splitting currently in calculating index ranges.  Note that
     * where I have said 'box' above, I actually mean sets of boxes because
     * a field range can consist of multiple intervals.
     */    
    const FieldRangeSet &FieldRangeSet::operator-=( const FieldRangeSet &other ) {
        int nUnincluded = 0;
        string unincludedKey;
        map<string,FieldRange>::const_iterator i = _ranges.begin();
        map<string,FieldRange>::const_iterator j = other._ranges.begin();
        while( nUnincluded < 2 && i != _ranges.end() && j != other._ranges.end() ) {
            int cmp = i->first.compare( j->first );
            if ( cmp == 0 ) {
                if ( i->second <= j->second ) {
                    // nothing
                }
                else {
                    ++nUnincluded;
                    unincludedKey = i->first;
                }
                ++i;
                ++j;
            }
            else if ( cmp < 0 ) {
                ++i;
            }
            else {
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
        range( unincludedKey.c_str() ) -= other.range( unincludedKey.c_str() );
        appendQueries( other );
        return *this;
    }
    
    const FieldRangeSet &FieldRangeSet::operator&=( const FieldRangeSet &other ) {
        map<string,FieldRange>::iterator i = _ranges.begin();
        map<string,FieldRange>::const_iterator j = other._ranges.begin();
        while( i != _ranges.end() && j != other._ranges.end() ) {
            int cmp = i->first.compare( j->first );
            if ( cmp == 0 ) {
                // Same field name, so find range intersection.
                i->second.intersect( j->second, _singleKey );
                ++i;
                ++j;
            }
            else if ( cmp < 0 ) {
                // Field present in *this.
                ++i;
            }
            else {
                // Field not present in *this, so add it.
                range( j->first.c_str() ) = j->second;
                ++j;
            }
        }
        while( j != other._ranges.end() ) {
            // Field not present in *this, add it.
            range( j->first.c_str() ) = j->second;
            ++j;
        }
        appendQueries( other );
        return *this;
    }    
    
    void FieldRangeSet::appendQueries( const FieldRangeSet &other ) {
        for( vector<BSONObj>::const_iterator i = other._queries.begin(); i != other._queries.end(); ++i ) {
            _queries.push_back( *i );
        }
    }
    
    void FieldRangeSet::makeEmpty() {
        for( map<string,FieldRange>::iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            i->second.makeEmpty();
        }
    }    
    
    void FieldRangeSet::handleOp( const char* matchFieldName, const BSONElement& op, bool isNot,
                                  bool optimize ) {
        int opType = op.getGtLtOp();

        // If the first $all element's first op is an $elemMatch, generate bounds for it
        // and ignore the remaining $all elements.  SERVER-664
        if ( opType == BSONObj::opALL ) {
            uassert( 13050, "$all requires array", op.type() == Array );
            BSONElement firstAllClause = op.embeddedObject().firstElement();
            if ( firstAllClause.type() == Object ) {
                BSONElement firstAllClauseOp = firstAllClause.embeddedObject().firstElement();
                if ( firstAllClauseOp.getGtLtOp() == BSONObj::opELEM_MATCH ) {
                    handleElemMatch( matchFieldName, firstAllClauseOp, isNot, optimize );
                    return;
                }
            }
        }

        if ( opType == BSONObj::opELEM_MATCH ) {
            handleElemMatch( matchFieldName, op, isNot, optimize );
        }
        else {
            intersectMatchField( matchFieldName, op, isNot, optimize );
        }
    }

    void FieldRangeSet::handleNotOp( const char* matchFieldName, const BSONElement& notOp,
                                     bool optimize ) {
        switch( notOp.type() ) {
            case Object: {
                BSONObjIterator notOpIterator( notOp.embeddedObject() );
                while( notOpIterator.more() ) {
                    BSONElement opToNegate = notOpIterator.next();
                    uassert( 13034, "invalid use of $not",
                             opToNegate.getGtLtOp() != BSONObj::Equality );
                    handleOp( matchFieldName, opToNegate, true, optimize );
                }
                break;
            }
            case RegEx:
                handleOp( matchFieldName, notOp, true, optimize );
                break;
            default:
                uassert( 13041, "invalid use of $not", false );
        }
    }

    void FieldRangeSet::handleElemMatch( const char* matchFieldName, const BSONElement& elemMatch,
                                         bool isNot, bool optimize ) {
        adjustMatchField();
        if ( !_boundElemMatch ) {
            return;
        }
        if ( isNot ) {
            // SERVER-5740 $elemMatch queries may depend on multiple fields, so $not:$elemMatch
            // cannot in general generate range constraints for a single field.
            return;
        }
        BSONObj elemMatchObj = elemMatch.embeddedObjectUserCheck();
        BSONElement firstElemMatchElement = elemMatchObj.firstElement();
        if ( firstElemMatchElement.getGtLtOp() != BSONObj::Equality ||
            str::equals( firstElemMatchElement.fieldName(), "$not" ) ) {
            // Handle $elemMatch applied to top level elements (where $elemMatch fields are
            // operators).  SERVER-1264
            BSONObj namedMatchExpression = elemMatch.wrap( matchFieldName );
            FieldRangeSet elemMatchRanges( _ns.c_str(),
                                           namedMatchExpression,
                                           true, // SERVER-4180 Generate single key index bounds.
                                           optimize,
                                           false // Prevent computing FieldRanges for nested
                                                 // $elemMatch expressions, which the index key
                                                 // generation implementation does not support.
                                           );
            *this &= elemMatchRanges;
        }
        else {
            // Handle $elemMatch applied to nested elements ($elemMatch fields are not operators,
            // but full match specifications).

            FieldRangeSet elemMatchRanges( _ns.c_str(), elemMatchObj, _singleKey, optimize );
            // Set the parent $elemMatch context on direct $elemMatch children (those with undotted
            // field names).
            for( map<string,FieldRange>::iterator i = elemMatchRanges._ranges.begin();
                 i != elemMatchRanges._ranges.end(); ++i ) {
                if ( !str::contains( i->first, '.' ) ) {
                    i->second.setElemMatchContext( elemMatch );
                }
            }
            scoped_ptr<FieldRangeSet> prefixedRanges
                    ( elemMatchRanges.prefixed( matchFieldName ) );
            *this &= *prefixedRanges;
        }
    }

    void FieldRangeSet::handleConjunctionClauses( const BSONObj& clauses, bool optimize ) {
        BSONObjIterator clausesIterator( clauses );
        while( clausesIterator.more() ) {
            BSONElement clause = clausesIterator.next();
            uassert( 14817, "$and/$or elements must be objects", clause.type() == Object );
            BSONObjIterator clauseIterator( clause.embeddedObject() );
            while( clauseIterator.more() ) {
                BSONElement matchElement = clauseIterator.next();
                handleMatchField( matchElement, optimize );
            }
        }
    }

    void FieldRangeSet::handleMatchField( const BSONElement& matchElement, bool optimize ) {
        const char* matchFieldName = matchElement.fieldName();
        if ( matchFieldName[ 0 ] == '$' ) {
            if ( str::equals( matchFieldName, "$and" ) ) {
                uassert( 14816, "$and expression must be a nonempty array",
                         matchElement.type() == Array &&
                         matchElement.embeddedObject().nFields() > 0 );
                handleConjunctionClauses( matchElement.embeddedObject(), optimize );
                return;
            }
        
            adjustMatchField();

            if ( str::equals( matchFieldName, "$or" ) ) {
                // Check for a singleton $or expression.
                if ( matchElement.type() == Array &&
                     matchElement.embeddedObject().nFields() == 1 ) {
                    // Compute field bounds for a singleton $or expression as if it is a $and
                    // expression.  With only one clause, the matching semantics are the same.
                    // SERVER-6416
                    handleConjunctionClauses( matchElement.embeddedObject(), optimize );
                }
                return;
            }
        
            if ( str::equals( matchFieldName, "$nor" ) ) {
                return;
            }

            if ( str::equals( matchFieldName, "$where" ) ) {
                return;
            }

            if ( str::equals( matchFieldName, "$atomic" ) ||
                 str::equals( matchFieldName, "$isolated" ) ) {
                return;
            }
        }
        
        bool equality =
            // Check for a parsable '$' operator within a match element, indicating the object
            // should not be matched as is but parsed.
            // NOTE This only checks for a '$' prefix in the first embedded field whereas Matcher
            // checks all embedded fields.
            ( getGtLtOp( matchElement ) == BSONObj::Equality ) &&
            // Similarly check for the $not meta operator.
            !( matchElement.type() == Object &&
               str::equals( matchElement.embeddedObject().firstElementFieldName(), "$not" ) );

        if ( equality ) {
            intersectMatchField( matchFieldName, matchElement, false, optimize );
            return;
        }

        bool untypedRegex =
            ( matchElement.type() == Object ) &&
            matchElement.embeddedObject().hasField( "$regex" );

        if ( untypedRegex ) {
            // $regex/$options pairs must be handled together and so are passed via the
            // element encapsulating them.
            intersectMatchField( matchFieldName, matchElement, false, optimize );
            // Other elements may remain to be handled, below.
        }

        BSONObjIterator matchExpressionIterator( matchElement.embeddedObject() );
        while( matchExpressionIterator.more() ) {
            BSONElement opElement = matchExpressionIterator.next();
            if ( str::equals( opElement.fieldName(), "$not" ) ) {
                handleNotOp( matchFieldName, opElement, optimize );
            }
            else {
                handleOp( matchFieldName, opElement, false, optimize );
            }
        }
    }

    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query, bool singleKey,
                                 bool optimize ) :
    _ns( ns ),
    _queries( 1, query.getOwned() ),
    _singleKey( singleKey ),
    _exactMatchRepresentation( true ),
    _boundElemMatch( true ) {
        init( optimize );
    }

    FieldRangeSet::FieldRangeSet( const char* ns, const BSONObj& query, bool singleKey,
                                  bool optimize, bool boundElemMatch ) :
        _ns( ns ),
        _queries( 1, query.getOwned() ),
        _singleKey( singleKey ),
        _exactMatchRepresentation( true ),
        _boundElemMatch( boundElemMatch ) {
        init( optimize );
    }

    void FieldRangeSet::init( bool optimize ) {
        BSONObjIterator i( _queries[ 0 ] );
        while( i.more() ) {
            handleMatchField( i.next(), optimize );
        }
    }
    
    /**
     * TODO When operators are refactored to a standard interface, a version of this should be
     * part of that interface.
     */
    void FieldRangeSet::adjustMatchField() {
        _exactMatchRepresentation = false;
    }
    
    void FieldRangeSet::intersectMatchField( const char *fieldName, const BSONElement &matchElement,
                                            bool isNot, bool optimize ) {
        FieldRange &selectedRange = range( fieldName );
        selectedRange.intersect( FieldRange( matchElement, isNot, optimize ), _singleKey );
        if ( !selectedRange.mustBeExactMatchRepresentation() ) {
            _exactMatchRepresentation = false;
        }
    }

    /**
     * @return true if @param range is universal or can be easily identified as a reverse universal
     * range (see FieldRange::reverse()).
     */
    static bool universalOrReverseUniversalRange( const FieldRange& range ) {
        if ( range.universal() ) {
            return true;
        }
        if ( range.intervals().size() != 1 ) {
            return false;
        }
        if ( !range.min().valuesEqual( maxKey.firstElement() ) ) {
            return false;
        }
        if ( !range.max().valuesEqual( minKey.firstElement() ) ) {
            return false;
        }
        return true;
    }

    FieldRangeVector::FieldRangeVector( const FieldRangeSet &frs, BSONObj keyPattern,
                                        int direction ) :
        _keyPattern(keyPattern),
        _direction( direction >= 0 ? 1 : -1 ),
        _hasAllIndexedRanges( true ) {
        verify( frs.matchPossibleForIndex( keyPattern));
        _queries = frs._queries;

        // For key generation
        BSONObjIterator it(_keyPattern);
        while (it.more()) {
            BSONElement elt = it.next();
            _fieldNames.push_back(elt.fieldName());
            _fixed.push_back(BSONElement());
        }

        _keyGenerator.reset(new BtreeKeyGeneratorV1(_fieldNames, _fixed, false));

        map<string,BSONElement> topFieldElemMatchContexts;
        BSONObjIterator i(keyPattern);
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange *range = &frs.range( e.fieldName() );
            verify( !range->empty() );

            if ( !frs.singleKey() ) {
                // Some constraints across different fields cannot be correctly intersected on a
                // multikey index.  SERVER-958
                //
                // Given a multikey index { 'a.b':1, 'a.c':1 } and query { 'a.b':3, 'a.c':3 } only
                // the index field 'a.b' is constrained to the range [3, 3], while the index
                // field 'a.c' is just constrained to be within minkey and maxkey.  This
                // implementation ensures that the document { a:[ { b:3 }, { c:3 } ] }, which
                // generates index keys { 'a.b':3, 'a.c':null } and { 'a.b':null and 'a.c':3 } will
                // be retrieved for the query.
                //
                // However, if the query is instead { a:{ $elemMatch:{ b:3, c:3 } } } then the
                // document { a:[ { b:3 }, { c:3 } ] } should not match the query and both index
                // fields 'a.b' and 'a.c' are constrained to the range [3, 3].

                // If no other field with the same topField has been constrained ...
                string topField = str::before( e.fieldName(), '.' );
                if ( topFieldElemMatchContexts.count( topField ) == 0 ) {
                    // ... and this field is constrained ...
                    if ( !range->universal() ) {
                        // ... record this field's elemMatchContext for its topField.
                        topFieldElemMatchContexts[ topField ] = range->elemMatchContext();
                    }
                }

                // Else if an already constrained field is not an $elemMatch sibling of this field
                // (does not share its elemMatchContext) ...
                else if ( range->elemMatchContext().eoo() ||
                          range->elemMatchContext().rawdata() !=
                              topFieldElemMatchContexts[ topField ].rawdata() ) {
                    // ... this field's parsed range cannot be used.
                    range = &frs.universalRange();
                    _hasAllIndexedRanges = false;
                }
            }

            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( forward ) {
                _ranges.push_back( *range );
            }
            else {
                _ranges.push_back( FieldRange( BSONObj().firstElement(), false, true ) );
                range->reverse( _ranges.back() );
            }
            verify( !_ranges.back().empty() );
        }
        uassert( 13385, "combinatorial limit of $in partitioning of result set exceeded",
                size() < MAX_IN_COMBINATIONS );
    }    

    bool FieldRangeVector::isSingleInterval() const {
        size_t i = 0;

        // Skip all equality ranges.
        while( i < _ranges.size() && _ranges[ i ].equality() ) {
            ++i;
        }

        // If there are no ranges left ...
        if ( i >= _ranges.size() ) {
            // ... then all ranges are equalities.
            return true;
        }

        // If the first non equality range does not consist of a single interval ...
        if ( _ranges[ i ].intervals().size() != 1 ) {
            // ... then the full FieldRangeVector does not consist of a single interval.
            return false;
        }
        ++i;

        while( i < _ranges.size() ) {
            // If a range after the first non equality is not universal ...
            if ( !universalOrReverseUniversalRange( _ranges[ i ] ) ) {
                // ... then the full FieldRangeVector does not consist of a single interval.
                return false;
            }
            ++i;
        }

        // The FieldRangeVector consists of zero or more equalities, then zero or one inequality
        // with a single interval, then zero or more universal ranges.
        return true;
    }

    BSONObj FieldRangeVector::startKey() const {
        BSONObjBuilder b;
        BSONObjIterator keys(_keyPattern);
        vector<FieldRange>::const_iterator i = _ranges.begin();
        for( ; i != _ranges.end(); ++i, ++keys ) {
            // Append lower bounds until an exclusive bound is found.
            const FieldInterval &fi = i->intervals().front();
            b.appendAs( fi._lower._bound, "" );
            if ( !fi._lower._inclusive ) {
                ++i;
                ++keys;
                break;
            }
        }
        for( ; i != _ranges.end(); ++i, ++keys ) {
            // After the first exclusive bound is found, use extreme values for subsequent fields.
            // For example, on index { a:1, b:1 } with query { a:{ $gt: 5 } } the start key is
            // { '':5, '':MaxKey }.
            bool forward = ( ( (*keys).number() >= 0 ? 1 : -1 ) * _direction > 0 );
            if ( forward ) {
                b.appendMaxKey( "" );
            }
            else {
                b.appendMinKey( "" );
            }
        }
        return b.obj();
    }

    bool FieldRangeVector::startKeyInclusive() const {
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            if( !i->intervals().front()._lower._inclusive ) {
                return false;
            }
        }
        return true;
    }

    BSONObj FieldRangeVector::endKey() const {
        BSONObjBuilder b;
        BSONObjIterator keys(_keyPattern);
        vector<FieldRange>::const_iterator i = _ranges.begin();
        for( ; i != _ranges.end(); ++i, ++keys ) {
            // Append upper bounds until an exclusive bound is found.
            const FieldInterval &fi = i->intervals().back();
            b.appendAs( fi._upper._bound, "" );
            if ( !fi._upper._inclusive ) {
                ++i;
                ++keys;
                break;
            }
        }
        for( ; i != _ranges.end(); ++i, ++keys ) {
            // After the first exclusive bound is found, use extreme values for subsequent fields.
            // For example, on index { a:1, b:1 } with query { a:{ $lt: 5 } } the end key is
            // { '':5, '':MinKey }.
            bool forward = ( ( (*keys).number() >= 0 ? 1 : -1 ) * _direction > 0 );
            if ( forward ) {
                b.appendMinKey( "" );
            }
            else {
                b.appendMaxKey( "" );
            }
        }
        return b.obj();
    }

    bool FieldRangeVector::endKeyInclusive() const {
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            if( !i->intervals().front()._upper._inclusive ) {
                return false;
            }
        }
        return true;
    }

    BSONObj FieldRangeVector::obj() const {
        BSONObjBuilder b;
        BSONObjIterator k(_keyPattern);
        for( int i = 0; i < (int)_ranges.size(); ++i ) {
            BSONArrayBuilder a( b.subarrayStart( k.next().fieldName() ) );
            for( vector<FieldInterval>::const_iterator j = _ranges[ i ].intervals().begin();
                j != _ranges[ i ].intervals().end(); ++j ) {
                a << BSONArray( BSON_ARRAY( j->_lower._bound << j->_upper._bound ).clientReadable() );
            }
            a.done();
        }
        return b.obj();
    }
    
    FieldRange *FieldRangeSet::__universalRange = 0;
    const FieldRange &FieldRangeSet::universalRange() const {
        FieldRange *&ret = __universalRange;
        if ( ret == 0 ) {
            // TODO: SERVER-5112
            ret = new FieldRange( BSONObj().firstElement(), false, true );
        }
        return *ret;
    }

    bool FieldRangeSet::isPointIntervalSet( const string& fieldname ) const {

        const vector<FieldInterval>& intervals = range( fieldname.c_str() ).intervals();

        if ( intervals.empty() ) {
            return false;
        }

        vector<FieldInterval>::const_iterator i;
        for(i = intervals.begin() ; i != intervals.end(); ++i){
            if (! i->equality() ) {
                return false;
            }
        }
        return true;
    }

    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        return QueryPattern( *this, sort );
    }

    int FieldRangeSet::numNonUniversalRanges() const {
        int count = 0;
        for( map<string,FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            if ( !i->second.universal() )
                ++count;
        }
        return count;
    }

    FieldRangeSet *FieldRangeSet::subset( const BSONObj &fields ) const {
        FieldRangeSet *ret = new FieldRangeSet( ns(), BSONObj(), _singleKey, true );
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( !range( e.fieldName() ).universal() ) {
                ret->range( e.fieldName() ) = range( e.fieldName() );
            }
        }
        ret->_queries = _queries;
        return ret;
    }

    FieldRangeSet* FieldRangeSet::prefixed( const string& prefix ) const {
        auto_ptr<FieldRangeSet> ret( new FieldRangeSet( ns(), BSONObj(), _singleKey, true ) );
        for( map<string,FieldRange>::const_iterator i = ranges().begin(); i != ranges().end();
            ++i ) {
            string prefixedFieldName = prefix + "." + i->first;
            ret->range( prefixedFieldName.c_str() ) = i->second;
        }
        ret->_queries = _queries;
        return ret.release();
    }

    string FieldRangeSet::toString() const {
        BSONObjBuilder bob;
        for( map<string,FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            bob << i->first << i->second.toString();
        }
        return bob.obj().jsonString();
    }
    
    bool FieldRangeSetPair::noNonUniversalRanges() const {
        return _singleKey.numNonUniversalRanges() == 0 && _multiKey.numNonUniversalRanges() == 0;
    }

    FieldRangeSetPair &FieldRangeSetPair::operator&=( const FieldRangeSetPair &other ) {
        _singleKey &= other._singleKey;
        _multiKey &= other._multiKey;
        return *this;
    }

    FieldRangeSetPair &FieldRangeSetPair::operator-=( const FieldRangeSet &scanned ) {
        _singleKey -= scanned;
        _multiKey -= scanned;
        return *this;            
    }
    
    string FieldRangeSetPair::toString() const {
        return BSON(
                    "singleKey" << _singleKey.toString() <<
                    "multiKey" << _multiKey.toString()
                    ).jsonString();
    }

    void FieldRangeSetPair::assertValidIndex( const NamespaceDetails *d, int idxNo ) const {
        massert( 14048,
                 "FieldRangeSetPair invalid index specified",
                 idxNo >= 0 && idxNo < d->getCompletedIndexCount() );
    }

    const FieldRangeSet &FieldRangeSetPair::frsForIndex( const NamespaceDetails* nsd, int idxNo ) const {
        assertValidIndexOrNoIndex( nsd, idxNo );
        if ( idxNo < 0 ) {
            // An unindexed cursor cannot have a "single key" constraint.
            return _multiKey;
        }
        return nsd->isMultikey( idxNo ) ? _multiKey : _singleKey;
    }

    bool FieldRangeVector::matchesElement( const BSONElement &e, int i, bool forward ) const {
        bool eq;
        int l = matchingLowElement( e, i, forward, eq );
        return ( l % 2 == 0 ); // if we're inside an interval
    }

    // binary search for interval containing the specified element
    // an even return value indicates that the element is contained within a valid interval
    int FieldRangeVector::matchingLowElement( const BSONElement &e, int i, bool forward, bool &lowEquality ) const {
        lowEquality = false;
        int l = -1;
        int h = _ranges[ i ].intervals().size() * 2;
        while( l + 1 < h ) {
            int m = ( l + h ) / 2;
            BSONElement toCmp;
            bool toCmpInclusive;
            const FieldInterval &interval = _ranges[ i ].intervals()[ m / 2 ];
            if ( m % 2 == 0 ) {
                toCmp = interval._lower._bound;
                toCmpInclusive = interval._lower._inclusive;
            }
            else {
                toCmp = interval._upper._bound;
                toCmpInclusive = interval._upper._inclusive;
            }
            int cmp = toCmp.woCompare( e, false );
            if ( !forward ) {
                cmp = -cmp;
            }
            if ( cmp < 0 ) {
                l = m;
            }
            else if ( cmp > 0 ) {
                h = m;
            }
            else {
                if ( m % 2 == 0 ) {
                    lowEquality = true;
                }
                int ret = m;
                // if left match and inclusive, all good
                // if left match and not inclusive, return right before left bound
                // if right match and inclusive, return left bound
                // if right match and not inclusive, return right bound
                if ( ( m % 2 == 0 && !toCmpInclusive ) || ( m % 2 == 1 && toCmpInclusive ) ) {
                    --ret;
                }
                return ret;
            }
        }
        verify( l + 1 == h );
        return l;
    }

    bool FieldRangeVector::matchesKey( const BSONObj &key ) const {
        BSONObjIterator j( key );
        BSONObjIterator k(_keyPattern);
        for( int l = 0; l < (int)_ranges.size(); ++l ) {
            int number = (int) k.next().number();
            bool forward = ( number >= 0 ? 1 : -1 ) * ( _direction >= 0 ? 1 : -1 ) > 0;
            if ( !matchesElement( j.next(), l, forward ) ) {
                return false;
            }
        }
        return true;
    }
    
    bool FieldRangeVector::matches( const BSONObj &obj ) const {
        bool ok = false;

        BSONObjSet keys;

        /**
         * Key generation by design is behind the index interface.  There is an exception here
         * because $or uses key generation to dedup its results.  When $or is fixed to not require
         * this, key generation will be removed from here.
         */
        _keyGenerator->getKeys(obj, &keys);

        // TODO The representation of matching keys could potentially be optimized
        // more for the case at hand.  (For example, we can potentially consider
        // fields individually instead of constructing several bson objects using
        // multikey arrays.)  But getKeys() canonically defines the key set for a
        // given object and for now we are using it as is.
        for( BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i ) {
            if ( matchesKey( *i ) ) {
                ok = true;
                break;
            }
        }

        LOG(5) << "FieldRangeVector::matches() returns " << ok << endl;

        return ok;
    }

    BSONObj FieldRangeVector::firstMatch( const BSONObj &obj ) const {
        // NOTE Only works in forward direction.
        verify( _direction >= 0 );
        BSONObjCmp oc(_keyPattern);
        BSONObjSet keys(oc);
        // See FieldRangeVector::matches for comment on key generation.
        _keyGenerator->getKeys(obj, &keys);
        for( BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i ) {
            if ( matchesKey( *i ) ) {
                return *i;
            }
        }
        return BSONObj();
    }
    
    string FieldRangeVector::toString() const {
        BSONObjBuilder bob;
        BSONObjIterator i(_keyPattern);
        for( vector<FieldRange>::const_iterator r = _ranges.begin();
            r != _ranges.end() && i.more(); ++r ) {
            BSONElement e = i.next();
            bob << e.fieldName() << r->toString();
        }
        return bob.obj().jsonString();
    }
    
    FieldRangeVectorIterator::FieldRangeVectorIterator( const FieldRangeVector &v,
                                                       int singleIntervalLimit ) :
    _v( v ),
    _i( _v._ranges.size(), singleIntervalLimit ),
    _cmp( _v._ranges.size(), 0 ),
    _inc( _v._ranges.size(), false ),
    _after(),
    _endNonUniversalRanges( endNonUniversalRanges() ) {
    }
    
    // TODO optimize more SERVER-5450.
    int FieldRangeVectorIterator::advance( const BSONObj &curr ) {
        BSONObjIterator j( curr );
        BSONObjIterator o( _v._keyPattern);
        // track first field for which we are not at the end of the valid values,
        // since we may need to advance from the key prefix ending with this field
        int latestNonEndpoint = -1;
        // iterate over fields to determine appropriate advance method
        for( int i = 0; i < _endNonUniversalRanges; ++i ) {
            if ( i > 0 && !_v._ranges[ i - 1 ].intervals()[ _i.get( i - 1 ) ].equality() ) {
                // if last bound was inequality, we don't know anything about where we are for this field
                // TODO if possible avoid this certain cases when value in previous field of the previous
                // key is the same as value of previous field in current key
                _i.setUnknowns( i );
            }
            BSONElement oo = o.next();
            bool reverse = ( ( oo.number() < 0 ) ^ ( _v._direction < 0 ) );
            BSONElement jj = j.next();
            if ( _i.get( i ) == -1 ) { // unknown position for this field, do binary search
                bool lowEquality;
                int l = _v.matchingLowElement( jj, i, !reverse, lowEquality );
                if ( l % 2 == 0 ) { // we are in a valid range for this field
                    _i.set( i, l / 2 );
                    int diff = (int)_v._ranges[ i ].intervals().size() - _i.get( i );
                    if ( diff > 1 ) {
                        latestNonEndpoint = i;
                    }
                    else if ( diff == 1 ) {
                        int x = _v._ranges[ i ].intervals()[ _i.get( i ) ]._upper._bound.woCompare( jj, false );
                        if ( x != 0 ) {
                            latestNonEndpoint = i;
                        }
                    }
                    continue;
                }
                else {   // not in a valid range for this field - determine if and how to advance
                    // check if we're after the last interval for this field
                    if ( l == (int)_v._ranges[ i ].intervals().size() * 2 - 1 ) {
                        if ( latestNonEndpoint == -1 ) {
                            return -2;
                        }
                        return advancePastZeroed( latestNonEndpoint + 1 );
                    }
                    _i.set( i, ( l + 1 ) / 2 );
                    if ( lowEquality ) {
                        return advancePast( i + 1 );
                    }
                    return advanceToLowerBound( i );
                }
            }
            bool first = true;
            bool eq = false;
            // _i.get( i ) != -1, so we have a starting interval for this field
            // which serves as a lower/equal bound on the first iteration -
            // we advance from this interval to find a matching interval
            while( _i.get( i ) < (int)_v._ranges[ i ].intervals().size() ) {

                int advanceMethod = validateCurrentInterval( i, jj, reverse, first, eq );
                if ( advanceMethod >= 0 ) {
                    return advanceMethod;
                }
                if ( advanceMethod == -1 && !hasReachedLimitForLastInterval( i ) ) {
                    break;
                }
                // advance to next interval and reset remaining fields
                _i.inc( i );
                _i.setZeroes( i + 1 );
                first = false;
            }
            int diff = (int)_v._ranges[ i ].intervals().size() - _i.get( i );
            if ( diff > 1 || ( !eq && diff == 1 ) ) {
                // check if we're not at the end of valid values for this field
                latestNonEndpoint = i;
            }
            else if ( diff == 0 ) {   // check if we're past the last interval for this field
                if ( latestNonEndpoint == -1 ) {
                    return -2;
                }
                // more values possible, skip...
                return advancePastZeroed( latestNonEndpoint + 1 );
            }
        }
        _i.incSingleIntervalCount();
        return -1;
    }

    void FieldRangeVectorIterator::prepDive() {
        for( int j = 0; j < _i.size(); ++j ) {
            _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
            _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
        }
        _i.resetIntervalCount();
    }
    
    int FieldRangeVectorIterator::validateCurrentInterval( int intervalIdx,
                                                          const BSONElement &currElt,
                                                          bool reverse, bool first,
                                                          bool &eqInclusiveUpperBound ) {
        eqInclusiveUpperBound = false;
        FieldIntervalMatcher matcher
                ( _v._ranges[ intervalIdx ].intervals()[ _i.get( intervalIdx ) ], currElt,
                 reverse );

        if ( matcher.isEqInclusiveUpperBound() ) {
            eqInclusiveUpperBound = true;
            return -1;
        }
        if ( matcher.isGteUpperBound() ) {
            return -2;
        }

        // below the upper bound

        if ( intervalIdx == 0 && first ) {
            // the value of 1st field won't go backward, so don't check lower bound
            // TODO maybe we can check 'first' only?
            return -1;
        }

        if ( matcher.isEqExclusiveLowerBound() ) {
            return advancePastZeroed( intervalIdx + 1 );
        }
        if ( matcher.isLtLowerBound() ) {
            _i.setZeroes( intervalIdx + 1 );
            return advanceToLowerBound( intervalIdx );
        }

        return -1;
    }
    
    int FieldRangeVectorIterator::advanceToLowerBound( int i ) {
        _cmp[ i ] = &_v._ranges[ i ].intervals()[ _i.get( i ) ]._lower._bound;
        _inc[ i ] = _v._ranges[ i ].intervals()[ _i.get( i ) ]._lower._inclusive;
        for( int j = i + 1; j < _i.size(); ++j ) {
            _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
            _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
        }
        _after = false;
        return i;
    }
    
    int FieldRangeVectorIterator::advancePast( int i ) {
        _after = true;
        return i;
    }
    
    int FieldRangeVectorIterator::advancePastZeroed( int i ) {
        _i.setZeroes( i );
        return advancePast( i );
    }

    int FieldRangeVectorIterator::endNonUniversalRanges() const {
        int i = _v._ranges.size() - 1;
        while( i > -1 && universalOrReverseUniversalRange( _v._ranges[ i ] ) ) {
            --i;
        }
        return i + 1;
    }

    FieldRangeVectorIterator::CompoundRangeCounter::CompoundRangeCounter( int size,
                                                                         int singleIntervalLimit ) :
    _i( size, -1 ),
    _singleIntervalCount(),
    _singleIntervalLimit( singleIntervalLimit ) {
    }
    
    string FieldRangeVectorIterator::CompoundRangeCounter::toString() const {
        BSONArrayBuilder bab;
        for( vector<int>::const_iterator i = _i.begin(); i != _i.end(); ++i ) {
            bab << *i;
        }
        return BSON( "i" << bab.arr() <<
                     "count" << _singleIntervalCount <<
                     "limit" << _singleIntervalLimit
                     ).jsonString();
    }
    
    FieldRangeVectorIterator::FieldIntervalMatcher::FieldIntervalMatcher
    ( const FieldInterval &interval, const BSONElement &element, bool reverse ) :
    _interval( interval ),
    _element( element ),
    _reverse( reverse ) {
    }
    
    int FieldRangeVectorIterator::FieldIntervalMatcher::lowerCmp() const {
        if ( !_lowerCmp._valid ) {
            setCmp( _lowerCmp, _interval._lower._bound );
        }
        return _lowerCmp._cmp;
    }
    
    int FieldRangeVectorIterator::FieldIntervalMatcher::upperCmp() const {
        if ( !_upperCmp._valid ) {
            setCmp( _upperCmp, _interval._upper._bound );
            if ( _interval.equality() ) {
                _lowerCmp = _upperCmp;
            }
        }
        return _upperCmp._cmp;
    }

    OrRangeGenerator::OrRangeGenerator(const char *ns, const BSONObj &query, bool optimize)
                                      : _baseSet(ns, query, optimize), _orFound() {
        BSONObjIterator i( _baseSet.originalQuery() );
        
        while (i.more()) {
            BSONElement e = i.next();
            if (strcmp(e.fieldName(), "$or") == 0) {
                uassert( 13262, "$or requires nonempty array", e.type() == Array && e.embeddedObject().nFields() > 0 );
                BSONObjIterator j( e.embeddedObject() );
                while( j.more() ) {
                    BSONElement f = j.next();
                    uassert( 13263, "$or array must contain objects", f.type() == Object );
                    _orSets.push_back( FieldRangeSetPair( ns, f.embeddedObject(), optimize ) );
                    uassert( 13291, "$or may not contain 'special' query", _orSets.back().getSpecial().empty() );
                    _originalOrSets.push_back( _orSets.back() );
                }
                _orFound = true;
                continue;
            }
        }
    }

    void OrRangeGenerator::assertMayPopOrClause() {
        massert( 13274, "no or clause to pop", _orFound && !orRangesExhausted() );        
    }
    
    void OrRangeGenerator::popOrClause( NamespaceDetails *nsd, int idxNo, const BSONObj &keyPattern ) {
        assertMayPopOrClause();
        auto_ptr<FieldRangeSet> holder;
        const FieldRangeSet *toDiff = &_originalOrSets.front().frsForIndex( nsd, idxNo );
        BSONObj indexSpec = keyPattern;
        if ( !indexSpec.isEmpty() && toDiff->matchPossibleForIndex( indexSpec ) ) {
            holder.reset( toDiff->subset( indexSpec ) );
            toDiff = holder.get();
        }
        _popOrClause( toDiff, nsd, idxNo, keyPattern );
    }
    
    void OrRangeGenerator::popOrClauseSingleKey() {
        assertMayPopOrClause();
        FieldRangeSet *toDiff = &_originalOrSets.front()._singleKey;
        _popOrClause( toDiff, 0, -1, BSONObj() );
    }
    
    /**
     * Removes the top or clause, which would have been recently scanned, and
     * removes the field ranges it covers from all subsequent or clauses.  As a
     * side effect, this function may invalidate the return values of topFrs()
     * calls made before this function was called.
     * @param indexSpec - Keys of the index that was used to satisfy the last or
     * clause.  Used to determine the range of keys that were scanned.  If
     * empty we do not constrain the previous clause's ranges using index keys,
     * which may reduce opportunities for range elimination.
     */
    void OrRangeGenerator::_popOrClause( const FieldRangeSet *toDiff, NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) {
        list<FieldRangeSetPair>::iterator i = _orSets.begin();
        list<FieldRangeSetPair>::iterator j = _originalOrSets.begin();
        ++i;
        ++j;
        while( i != _orSets.end() ) {
            *i -= *toDiff;
            // Check if match is possible at all, and if it is possible for the recently scanned index.
            if( !i->matchPossible() || ( d && !i->matchPossibleForIndex( d, idxNo, keyPattern ) ) ) {
                i = _orSets.erase( i );
                j = _originalOrSets.erase( j );
            }
            else {
                ++i;
                ++j;
            }
        }
        _oldOrSets.push_front( _orSets.front() );
        _orSets.pop_front();
        _originalOrSets.pop_front();
    }
    
    struct SimpleRegexUnitTest : StartupTest {
        void run() {
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^foo");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "foo" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f?oo");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^fz?oo");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "m");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "m");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "mi");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af \t\vo\n\ro  \\ \\# #comment", "mx");
                BSONObj o = b.done();
                verify( simpleRegex(o.firstElement()) == "foo #" );
            }
            {
                verify( simpleRegex("^\\Qasdf\\E", "", NULL) == "asdf" );
                verify( simpleRegex("^\\Qasdf\\E.*", "", NULL) == "asdf" );
                verify( simpleRegex("^\\Qasdf", "", NULL) == "asdf" ); // PCRE supports this
                verify( simpleRegex("^\\Qasdf\\\\E", "", NULL) == "asdf\\" );
                verify( simpleRegex("^\\Qas.*df\\E", "", NULL) == "as.*df" );
                verify( simpleRegex("^\\Qas\\Q[df\\E", "", NULL) == "as\\Q[df" );
                verify( simpleRegex("^\\Qas\\E\\\\E\\Q$df\\E", "", NULL) == "as\\E$df" ); // quoted string containing \E
            }

        }
    } simple_regex_unittest;


    long long applySkipLimit( long long num , const BSONObj& cmd ) {
        BSONElement s = cmd["skip"];
        BSONElement l = cmd["limit"];

        if ( s.isNumber() ) {
            num = num - s.numberLong();
            if ( num < 0 ) {
                num = 0;
            }
        }

        if ( l.isNumber() ) {
            long long limit = l.numberLong();
            if( limit < 0 ){
                limit = -limit;
            }

            if ( limit < num && limit != 0 ) { // 0 limit means no limit
                num = limit;
            }
        }

        return num;
    }

    bool isSimpleIdQuery( const BSONObj& query ) {
        BSONObjIterator i(query);
        
        if( !i.more() ) 
            return false;
        
        BSONElement e = i.next();
        
        if( i.more() ) 
            return false;
        
        if( strcmp("_id", e.fieldName()) != 0 ) 
            return false;
        
        if ( e.isSimpleType() ) // e.g. not something like { _id : { $gt : ...
            return true;
        
        if ( e.type() == Object )
            return e.Obj().firstElementFieldName()[0] != '$';
        
        return false;
    }

} // namespace mongo
