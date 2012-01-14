// @file queryutil.cpp

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

#include "pch.h"

#include "btree.h"
#include "matcher.h"
#include "pdfile.h"
#include "queryoptimizer.h"
#include "../util/unittest.h"
#include "dbmessage.h"
#include "indexkey.h"
#include "../util/mongoutils/str.h"

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
        default: assert(false); return ""; //return squashes compiler warning
        }
    }

    string simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }


    FieldRange::FieldRange( const BSONElement &e, bool singleKey, bool isNot, bool optimize )
    : _singleKey( singleKey ) {
        int op = e.getGtLtOp();

        // NOTE with $not, we could potentially form a complementary set of intervals.
        if ( !isNot && !e.eoo() && e.type() != RegEx && op == BSONObj::opIN ) {
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
                    regexes.push_back( FieldRange( ie, singleKey, false, optimize ) );
                }
                else {
                    // A document array may be indexed by its first element, by undefined
                    // if it is empty, or as a full array if it is embedded within another
                    // array.
                    vals.insert( ie );                        
                    if ( ie.type() == Array ) {
                        BSONElement temp = ie.embeddedObject().firstElement();
                        if ( temp.eoo() ) {
                            temp = staticUndefined.firstElement();
                        }                        
                        vals.insert( temp );
                    }
                }
            }

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

        if ( e.eoo() )
            return;

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
            // do nothing
            break;
        case BSONObj::opELEM_MATCH: {
            log() << "warning: shouldn't get here?" << endl;
            break;
        }
        case BSONObj::opNEAR:
        case BSONObj::opWITHIN:
            _special = "2d";
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

        if ( optimize ) {
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ) { // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ) { // TODO: get rid of isSimpleType
                if( upper.type() == Date ) 
                    lowerInclusive = false;
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
            }
        }

    }

    void FieldRange::finishOperation( const vector<FieldInterval> &newIntervals, const FieldRange &other ) {
        _intervals = newIntervals;
        for( vector<BSONObj>::const_iterator i = other._objData.begin(); i != other._objData.end(); ++i )
            _objData.push_back( *i );
        if ( _special.size() == 0 && other._special.size() )
            _special = other._special;
    }

    // as called, these functions find the max/min of a bound in the
    // opposite direction, so inclusive bounds are considered less
    // superlative
    FieldBound maxFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp < 0 )
            return b;
        return a;
    }

    FieldBound minFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a._bound.woCompare( b._bound, false );
        if ( ( cmp == 0 && !b._inclusive ) || cmp > 0 )
            return b;
        return a;
    }

    bool fieldIntervalOverlap( const FieldInterval &one, const FieldInterval &two, FieldInterval &result ) {
        result._lower = maxFieldBound( one._lower, two._lower );
        result._upper = minFieldBound( one._upper, two._upper );
        return result.strictValid();
    }

    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        if ( !_singleKey && nontrivial() ) {
            if ( other <= *this ) {
             	*this = other;
            }
            return *this;
        }
        vector<FieldInterval> newIntervals;
        vector<FieldInterval>::const_iterator i = _intervals.begin();
        vector<FieldInterval>::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) ) {
                newIntervals.push_back( overlap );
            }
            if ( i->_upper == minFieldBound( i->_upper, j->_upper ) ) {
                ++i;
            }
            else {
                ++j;
            }
        }
        finishOperation( newIntervals, other );
        return *this;
    }

    void handleInterval( const FieldInterval &lower, FieldBound &low, FieldBound &high, vector<FieldInterval> &newIntervals ) {
        if ( low._bound.eoo() ) {
            low = lower._lower; high = lower._upper;
        }
        else {
            int cmp = high._bound.woCompare( lower._lower._bound, false );
            if ( ( cmp < 0 ) || ( cmp == 0 && !high._inclusive && !lower._lower._inclusive ) ) {
                FieldInterval tmp;
                tmp._lower = low;
                tmp._upper = high;
                newIntervals.push_back( tmp );
                low = lower._lower; high = lower._upper;
            }
            else {
                high = lower._upper;
            }
        }
    }

    const FieldRange &FieldRange::operator|=( const FieldRange &other ) {
        vector<FieldInterval> newIntervals;
        FieldBound low;
        FieldBound high;
        vector<FieldInterval>::const_iterator i = _intervals.begin();
        vector<FieldInterval>::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( ( cmp == 0 && i->_lower._inclusive ) || cmp < 0 ) {
                handleInterval( *i, low, high, newIntervals );
                ++i;
            }
            else {
                handleInterval( *j, low, high, newIntervals );
                ++j;
            }
        }
        while( i != _intervals.end() ) {
            handleInterval( *i, low, high, newIntervals );
            ++i;
        }
        while( j != other._intervals.end() ) {
            handleInterval( *j, low, high, newIntervals );
            ++j;
        }
        FieldInterval tmp;
        tmp._lower = low;
        tmp._upper = high;
        newIntervals.push_back( tmp );
        finishOperation( newIntervals, other );
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
        finishOperation( newIntervals, other );
        return *this;
    }

    // TODO write a proper implementation that doesn't do a full copy
    bool FieldRange::operator<=( const FieldRange &other ) const {
        FieldRange temp = *this;
        temp -= other;
        return temp.empty();
    }

    void FieldRange::setExclusiveBounds() {
        for( vector<FieldInterval>::iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            i->_lower._inclusive = false;
            i->_upper._inclusive = false;
        }
    }

    void FieldRange::reverse( FieldRange &ret ) const {
        assert( _special.empty() );
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
        buf << ( _lower._inclusive ? "[" : "(" );
        buf << _lower._bound;
        buf << " , ";
        buf << _upper._bound;
        buf << ( _upper._inclusive ? "]" : ")" );
        return buf.str();
    }

    string FieldRange::toString() const {
        StringBuilder buf;
        buf << "(FieldRange special: " << _special << " singleKey: " << _special << " intervals: ";
        for( vector<FieldInterval>::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            buf << i->toString();
        }

        buf << ")";
        return buf.str();
    }

    string FieldRangeSet::getSpecial() const {
        string s = "";
        for ( map<string,FieldRange>::const_iterator i=_ranges.begin(); i!=_ranges.end(); i++ ) {
            if ( i->second.getSpecial().size() == 0 )
                continue;
            uassert( 13033 , "can't have 2 special fields" , s.size() == 0 );
            s = i->second.getSpecial();
        }
        return s;
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
        map<string,FieldRange>::iterator i = _ranges.begin();
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
                i->second &= j->second;
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
    
    void FieldRangeSet::processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize ) {
        BSONElement g = f;
        int op2 = g.getGtLtOp();
        if ( op2 == BSONObj::opALL ) {
            BSONElement h = g;
            uassert( 13050 ,  "$all requires array", h.type() == Array );
            BSONObjIterator i( h.embeddedObject() );
            if( i.more() ) {
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ) {
                    g = x.embeddedObject().firstElement();
                    op2 = g.getGtLtOp();
                }
            }
        }
        if ( op2 == BSONObj::opELEM_MATCH ) {
            BSONObjIterator k( g.embeddedObjectUserCheck() );
            while ( k.more() ) {
                BSONElement h = k.next();
                StringBuilder buf;
                buf << fieldName << "." << h.fieldName();
                string fullname = buf.str();

                int op3 = getGtLtOp( h );
                if ( op3 == BSONObj::Equality ) {
                    range( fullname.c_str() ) &= FieldRange( h , _singleKey , isNot , optimize );
                }
                else {
                    BSONObjIterator l( h.embeddedObject() );
                    while ( l.more() ) {
                        range( fullname.c_str() ) &= FieldRange( l.next() , _singleKey , isNot , optimize );
                    }
                }
            }
        }
        else {
            range( fieldName ) &= FieldRange( f , _singleKey , isNot , optimize );
        }
    }

    void FieldRangeSet::processQueryField( const BSONElement &e, bool optimize ) {
        if ( e.fieldName()[ 0 ] == '$' ) {
            if ( strcmp( e.fieldName(), "$and" ) == 0 ) {
                uassert( 14816 , "$and expression must be a nonempty array" , e.type() == Array && e.embeddedObject().nFields() > 0 );
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement e = i.next();
                    uassert( 14817 , "$and elements must be objects" , e.type() == Object );
                    BSONObjIterator j( e.embeddedObject() );
                    while( j.more() ) {
                        processQueryField( j.next(), optimize );
                    }
                }            
            }
        
            if ( strcmp( e.fieldName(), "$where" ) == 0 ) {
                return;
            }
        
            if ( strcmp( e.fieldName(), "$or" ) == 0 ) {
                return;
            }
        
            if ( strcmp( e.fieldName(), "$nor" ) == 0 ) {
                return;
            }
        }
        
        bool equality = ( getGtLtOp( e ) == BSONObj::Equality );
        if ( equality && e.type() == Object ) {
            equality = ( strcmp( e.embeddedObject().firstElementFieldName(), "$not" ) != 0 );
        }

        if ( equality || ( e.type() == Object && !e.embeddedObject()[ "$regex" ].eoo() ) ) {
            range( e.fieldName() ) &= FieldRange( e , _singleKey , false , optimize );
        }
        if ( !equality ) {
            BSONObjIterator j( e.embeddedObject() );
            while( j.more() ) {
                BSONElement f = j.next();
                if ( strcmp( f.fieldName(), "$not" ) == 0 ) {
                    switch( f.type() ) {
                    case Object: {
                        BSONObjIterator k( f.embeddedObject() );
                        while( k.more() ) {
                            BSONElement g = k.next();
                            uassert( 13034, "invalid use of $not", g.getGtLtOp() != BSONObj::Equality );
                            processOpElement( e.fieldName(), g, true, optimize );
                        }
                        break;
                    }
                    case RegEx:
                        processOpElement( e.fieldName(), f, true, optimize );
                        break;
                    default:
                        uassert( 13041, "invalid use of $not", false );
                    }
                }
                else {
                    processOpElement( e.fieldName(), f, false, optimize );
                }
            }
        }
    }

    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query, bool singleKey, bool optimize )
        : _ns( ns ), _queries( 1, query.getOwned() ), _singleKey( singleKey ) {
        BSONObjIterator i( _queries[ 0 ] );

        while( i.more() ) {
            processQueryField( i.next(), optimize );
        }
    }
    
    FieldRangeVector::FieldRangeVector( const FieldRangeSet &frs, const IndexSpec &indexSpec, int direction )
    :_indexSpec( indexSpec ), _direction( direction >= 0 ? 1 : -1 ) {
        _queries = frs._queries;
        BSONObjIterator i( _indexSpec.keyPattern );
        set< string > baseObjectNontrivialPrefixes;
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange *range = &frs.range( e.fieldName() );
            if ( !frs.singleKey() ) {
                string prefix = str::before( e.fieldName(), '.' );
                if ( baseObjectNontrivialPrefixes.count( prefix ) > 0 ) {
                    // A field with the same parent field has already been
                    // constrainted, and with a multikey index we cannot
                    // constrain this field.
                    range = &frs.trivialRange();
                } else {
                    if ( range->nontrivial() ) {
                        baseObjectNontrivialPrefixes.insert( prefix );
                    }
                }
            }
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( forward ) {
                _ranges.push_back( *range );
            }
            else {
                _ranges.push_back( FieldRange( BSONObj().firstElement(), frs.singleKey(), false, true ) );
                range->reverse( _ranges.back() );
            }
            assert( !_ranges.back().empty() );
        }
        uassert( 13385, "combinatorial limit of $in partitioning of result set exceeded", size() < 1000000 );
    }    

    BSONObj FieldRangeVector::startKey() const {
        BSONObjBuilder b;
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            const FieldInterval &fi = i->intervals().front();
            b.appendAs( fi._lower._bound, "" );
        }
        return b.obj();
    }

    BSONObj FieldRangeVector::endKey() const {
        BSONObjBuilder b;
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            const FieldInterval &fi = i->intervals().back();
            b.appendAs( fi._upper._bound, "" );
        }
        return b.obj();
    }

    BSONObj FieldRangeVector::obj() const {
        BSONObjBuilder b;
        BSONObjIterator k( _indexSpec.keyPattern );
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
    
    FieldRange *FieldRangeSet::__singleKeyTrivialRange = 0;
    FieldRange *FieldRangeSet::__multiKeyTrivialRange = 0;
    const FieldRange &FieldRangeSet::trivialRange() const {
        FieldRange *&ret = _singleKey ? __singleKeyTrivialRange : __multiKeyTrivialRange;
        if ( ret == 0 ) {
            ret = new FieldRange( BSONObj().firstElement(), _singleKey, false, true );
        }
        return *ret;
    }

    BSONObj FieldRangeSet::simplifiedQuery( const BSONObj &_fields ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map<string,FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                b.append( i->first, 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            const char *name = e.fieldName();
            const FieldRange &eRange = range( name );
            assert( !eRange.empty() );
            if ( eRange.equality() )
                b.appendAs( eRange.min(), name );
            else if ( eRange.nontrivial() ) {
                BSONObj o;
                BSONObjBuilder c;
                if ( eRange.min().type() != MinKey )
                    c.appendAs( eRange.min(), eRange.minInclusive() ? "$gte" : "$gt" );
                if ( eRange.max().type() != MaxKey )
                    c.appendAs( eRange.max(), eRange.maxInclusive() ? "$lte" : "$lt" );
                o = c.obj();
                b.append( name, o );
            }
        }
        return b.obj();
    }

    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        return QueryPattern( *this, sort );
    }

    // TODO get rid of this
    BoundList FieldRangeSet::indexBounds( const BSONObj &keyPattern, int direction ) const {
        typedef vector<pair<shared_ptr<BSONObjBuilder>, shared_ptr<BSONObjBuilder> > > BoundBuilders;
        BoundBuilders builders;
        builders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ), shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
        BSONObjIterator i( keyPattern );
        bool ineq = false; // until ineq is true, we are just dealing with equality and $in bounds
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange &fr = range( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( !ineq ) {
                if ( fr.equality() ) {
                    for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                        j->first->appendAs( fr.min(), "" );
                        j->second->appendAs( fr.min(), "" );
                    }
                }
                else {
                    if ( !fr.inQuery() ) {
                        ineq = true;
                    }
                    BoundBuilders newBuilders;
                    const vector<FieldInterval> &intervals = fr.intervals();
                    for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i ) {
                        BSONObj first = i->first->obj();
                        BSONObj second = i->second->obj();

                        const unsigned maxCombinations = 4000000;
                        if ( forward ) {
                            for( vector<FieldInterval>::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                                uassert( 13303, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < maxCombinations );
                                newBuilders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ), shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_lower._bound, "" );
                                newBuilders.back().second->appendAs( j->_upper._bound, "" );
                            }
                        }
                        else {
                            for( vector<FieldInterval>::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                                uassert( 13304, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < maxCombinations );
                                newBuilders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ), shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_upper._bound, "" );
                                newBuilders.back().second->appendAs( j->_lower._bound, "" );
                            }
                        }
                    }
                    builders = newBuilders;
                }
            }
            else {
                for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( forward ? fr.min() : fr.max(), "" );
                    j->second->appendAs( forward ? fr.max() : fr.min(), "" );
                }
            }
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

    FieldRangeSet *FieldRangeSet::subset( const BSONObj &fields ) const {
        FieldRangeSet *ret = new FieldRangeSet( _ns, BSONObj(), _singleKey, true );
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( range( e.fieldName() ).nontrivial() ) {
                ret->range( e.fieldName() ) = range( e.fieldName() );
            }
        }
        ret->_queries = _queries;
        return ret;
    }
    
    bool FieldRangeSetPair::noNontrivialRanges() const {
        return _singleKey.matchPossible() && _singleKey.nNontrivialRanges() == 0 &&
                 _multiKey.matchPossible() && _multiKey.nNontrivialRanges() == 0;
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
    
    BSONObj FieldRangeSetPair::simplifiedQueryForIndex( NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) const {
        return frsForIndex( d, idxNo ).simplifiedQuery( keyPattern );
    }    
    
    void FieldRangeSetPair::assertValidIndex( const NamespaceDetails *d, int idxNo ) const {
        massert( 14048, "FieldRangeSetPair invalid index specified", idxNo >= 0 && idxNo < d->nIndexes );   
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
        assert( l + 1 == h );
        return l;
    }

    bool FieldRangeVector::matchesKey( const BSONObj &key ) const {
        BSONObjIterator j( key );
        BSONObjIterator k( _indexSpec.keyPattern );
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

        // TODO The representation of matching keys could potentially be optimized
        // more for the case at hand.  (For example, we can potentially consider
        // fields individually instead of constructing several bson objects using
        // multikey arrays.)  But getKeys() canonically defines the key set for a
        // given object and for now we are using it as is.
        BSONObjSet keys;
        _indexSpec.getKeys( obj, keys );
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
        assert( _direction >= 0 );
        BSONObjSet keys( BSONObjCmp( _indexSpec.keyPattern ) );
        _indexSpec.getKeys( obj, keys );
        for( BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i ) {
            if ( matchesKey( *i ) ) {
                return *i;
            }
        }
        return BSONObj();
    }
    
    // TODO optimize more
    int FieldRangeVectorIterator::advance( const BSONObj &curr ) {
        BSONObjIterator j( curr );
        BSONObjIterator o( _v._indexSpec.keyPattern );
        // track first field for which we are not at the end of the valid values,
        // since we may need to advance from the key prefix ending with this field
        int latestNonEndpoint = -1;
        // iterate over fields to determine appropriate advance method
        for( int i = 0; i < (int)_i.size(); ++i ) {
            if ( i > 0 && !_v._ranges[ i - 1 ].intervals()[ _i[ i - 1 ] ].equality() ) {
                // if last bound was inequality, we don't know anything about where we are for this field
                // TODO if possible avoid this certain cases when value in previous field of the previous
                // key is the same as value of previous field in current key
                setMinus( i );
            }
            bool eq = false;
            BSONElement oo = o.next();
            bool reverse = ( ( oo.number() < 0 ) ^ ( _v._direction < 0 ) );
            BSONElement jj = j.next();
            if ( _i[ i ] == -1 ) { // unknown position for this field, do binary search
                bool lowEquality;
                int l = _v.matchingLowElement( jj, i, !reverse, lowEquality );
                if ( l % 2 == 0 ) { // we are in a valid range for this field
                    _i[ i ] = l / 2;
                    int diff = (int)_v._ranges[ i ].intervals().size() - _i[ i ];
                    if ( diff > 1 ) {
                        latestNonEndpoint = i;
                    }
                    else if ( diff == 1 ) {
                        int x = _v._ranges[ i ].intervals()[ _i[ i ] ]._upper._bound.woCompare( jj, false );
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
                        setZero( latestNonEndpoint + 1 );
                        // skip to curr / latestNonEndpoint + 1 / superlative
                        _after = true;
                        return latestNonEndpoint + 1;
                    }
                    _i[ i ] = ( l + 1 ) / 2;
                    if ( lowEquality ) {
                        // skip to curr / i + 1 / superlative
                        _after = true;
                        return i + 1;
                    }
                    // skip to curr / i / nextbounds
                    _cmp[ i ] = &_v._ranges[ i ].intervals()[ _i[ i ] ]._lower._bound;
                    _inc[ i ] = _v._ranges[ i ].intervals()[ _i[ i ] ]._lower._inclusive;
                    for( int j = i + 1; j < (int)_i.size(); ++j ) {
                        _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
                        _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
                    }
                    _after = false;
                    return i;
                }
            }
            bool first = true;
            // _i[ i ] != -1, so we have a starting interval for this field
            // which serves as a lower/equal bound on the first iteration -
            // we advance from this interval to find a matching interval
            while( _i[ i ] < (int)_v._ranges[ i ].intervals().size() ) {
                // compare to current interval's upper bound
                int x = _v._ranges[ i ].intervals()[ _i[ i ] ]._upper._bound.woCompare( jj, false );
                if ( reverse ) {
                    x = -x;
                }
                if ( x == 0 && _v._ranges[ i ].intervals()[ _i[ i ] ]._upper._inclusive ) {
                    eq = true;
                    break;
                }
                // see if we're less than the upper bound
                if ( x > 0 ) {
                    if ( i == 0 && first ) {
                        // the value of 1st field won't go backward, so don't check lower bound
                        // TODO maybe we can check first only?
                        break;
                    }
                    // if it's an equality interval, don't need to compare separately to lower bound
                    if ( !_v._ranges[ i ].intervals()[ _i[ i ] ].equality() ) {
                        // compare to current interval's lower bound
                        x = _v._ranges[ i ].intervals()[ _i[ i ] ]._lower._bound.woCompare( jj, false );
                        if ( reverse ) {
                            x = -x;
                        }
                    }
                    // if we're equal to and not inclusive the lower bound, advance
                    if ( ( x == 0 && !_v._ranges[ i ].intervals()[ _i[ i ] ]._lower._inclusive ) ) {
                        setZero( i + 1 );
                        // skip to curr / i + 1 / superlative
                        _after = true;
                        return i + 1;
                    }
                    // if we're less than the lower bound, advance
                    if ( x > 0 ) {
                        setZero( i + 1 );
                        // skip to curr / i / nextbounds
                        _cmp[ i ] = &_v._ranges[ i ].intervals()[ _i[ i ] ]._lower._bound;
                        _inc[ i ] = _v._ranges[ i ].intervals()[ _i[ i ] ]._lower._inclusive;
                        for( int j = i + 1; j < (int)_i.size(); ++j ) {
                            _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
                            _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
                        }
                        _after = false;
                        return i;
                    }
                    else {
                        break;
                    }
                }
                // we're above the upper bound, so try next interval and reset remaining fields
                ++_i[ i ];
                setZero( i + 1 );
                first = false;
            }
            int diff = (int)_v._ranges[ i ].intervals().size() - _i[ i ];
            if ( diff > 1 || ( !eq && diff == 1 ) ) {
                // check if we're not at the end of valid values for this field
                latestNonEndpoint = i;
            }
            else if ( diff == 0 ) {   // check if we're past the last interval for this field
                if ( latestNonEndpoint == -1 ) {
                    return -2;
                }
                // more values possible, skip...
                setZero( latestNonEndpoint + 1 );
                // skip to curr / latestNonEndpoint + 1 / superlative
                _after = true;
                return latestNonEndpoint + 1;
            }
        }
        return -1;
    }

    void FieldRangeVectorIterator::prepDive() {
        for( int j = 0; j < (int)_i.size(); ++j ) {
            _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
            _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
        }
    }

    BSONObj FieldRangeVectorIterator::startKey() {
        BSONObjBuilder b;
        for( int unsigned i = 0; i < _i.size(); ++i ) {
            const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
            b.appendAs( fi._lower._bound, "" );
        }
        return b.obj();
    }

    // temp
    BSONObj FieldRangeVectorIterator::endKey() {
        BSONObjBuilder b;
        for( int unsigned i = 0; i < _i.size(); ++i ) {
            const FieldInterval &fi = _v._ranges[ i ].intervals()[ _i[ i ] ];
            b.appendAs( fi._upper._bound, "" );
        }
        return b.obj();
    }
    
    OrRangeGenerator::OrRangeGenerator( const char *ns, const BSONObj &query , bool optimize )
    : _baseSet( ns, query, optimize ), _orFound() {
        
        BSONObjIterator i( _baseSet.originalQuery() );
        
        while( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp( e.fieldName(), "$or" ) == 0 ) {
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
        massert( 13274, "no or clause to pop", !orFinished() );        
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
        popOrClause( toDiff, nsd, idxNo, keyPattern );
    }
    
    void OrRangeGenerator::popOrClauseSingleKey() {
        assertMayPopOrClause();
        FieldRangeSet *toDiff = &_originalOrSets.front()._singleKey;
        popOrClause( toDiff );
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
    void OrRangeGenerator::popOrClause( const FieldRangeSet *toDiff, NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) {
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
    
    struct SimpleRegexUnitTest : UnitTest {
        void run() {
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^foo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^fz?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "m");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "m");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af", "mi");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "\\Af \t\vo\n\ro  \\ \\# #comment", "mx");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo #" );
            }
            {
                assert( simpleRegex("^\\Qasdf\\E", "", NULL) == "asdf" );
                assert( simpleRegex("^\\Qasdf\\E.*", "", NULL) == "asdf" );
                assert( simpleRegex("^\\Qasdf", "", NULL) == "asdf" ); // PCRE supports this
                assert( simpleRegex("^\\Qasdf\\\\E", "", NULL) == "asdf\\" );
                assert( simpleRegex("^\\Qas.*df\\E", "", NULL) == "as.*df" );
                assert( simpleRegex("^\\Qas\\Q[df\\E", "", NULL) == "as\\Q[df" );
                assert( simpleRegex("^\\Qas\\E\\\\E\\Q$df\\E", "", NULL) == "as\\E$df" ); // quoted string containing \E
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
            if ( limit < num ) {
                num = limit;
            }
        }

        return num;
    }


} // namespace mongo
