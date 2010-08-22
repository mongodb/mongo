// queryutil.cpp

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

namespace mongo {
    extern BSONObj staticNull;
    
    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix){
        string r = "";

        if (purePrefix) *purePrefix = false;

        bool multilineOK;
        if ( regex[0] == '\\' && regex[1] == 'A'){
            multilineOK = true;
            regex += 2;
        } else if (regex[0] == '^') {
            multilineOK = false;
            regex += 1;
        } else {
            return r;
        }

        bool extended = false;
        while (*flags){
            switch (*(flags++)){
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

        while(*regex){
            char c = *(regex++);
            if ( c == '*' || c == '?' ){
                // These are the only two symbols that make the last char optional
                r = ss.str();
                r = r.substr( 0 , r.size() - 1 );
                return r; //breaking here fails with /^a?/
            } else if (c == '\\'){
                // slash followed by non-alphanumeric represents the following char
                c = *(regex++);
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '0') ||
                    (c == '\0'))
                {
                    r = ss.str();
                    break;
                } else {
                    ss << c;
                }
            } else if (strchr("^$.[|()+{", c)){
                // list of "metacharacters" from man pcrepattern
                r = ss.str();
                break;
            } else if (extended && c == '#'){
                // comment
                r = ss.str();
                break;
            } else if (extended && isspace(c)){
                continue;
            } else {
                // self-matching char
                ss << c;
            }
        }

        if ( r.empty() && *regex == 0 ){
            r = ss.str();
            if (purePrefix) *purePrefix = !r.empty();
        }

        return r;
    }
    inline string simpleRegex(const BSONElement& e){
        switch(e.type()){
            case RegEx:
                return simpleRegex(e.regex(), e.regexFlags());
            case Object:{
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
    
    
    FieldRange::FieldRange( const BSONElement &e, bool isNot, bool optimize ) {
        // NOTE with $not, we could potentially form a complementary set of intervals.
        if ( !isNot && !e.eoo() && e.type() != RegEx && e.getGtLtOp() == BSONObj::opIN ) {
            set< BSONElement, element_lt > vals;
            vector< FieldRange > regexes;
            uassert( 12580 , "invalid query" , e.isABSONObj() );
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() ) {
                BSONElement ie = i.next();
                if ( ie.type() == RegEx ) {
                    regexes.push_back( FieldRange( ie, false, optimize ) );
                } else {
                    vals.insert( ie );
                }
            }

            for( set< BSONElement, element_lt >::const_iterator i = vals.begin(); i != vals.end(); ++i )
                _intervals.push_back( FieldInterval(*i) );

            for( vector< FieldRange >::const_iterator i = regexes.begin(); i != regexes.end(); ++i )
                *this |= *i;
            
            return;
        }
        
        if ( e.type() == Array && e.getGtLtOp() == BSONObj::Equality ){
            
            _intervals.push_back( FieldInterval(e) );
            
            const BSONElement& temp = e.embeddedObject().firstElement();
            if ( ! temp.eoo() ){
                if ( temp < e )
                    _intervals.insert( _intervals.begin() , temp );
                else
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
        if ( e.type() == RegEx
             || (e.type() == Object && !e.embeddedObject()["$regex"].eoo())
           )
        {
            if ( !isNot ) { // no optimization for negated regex - we could consider creating 2 intervals comprising all nonmatching prefixes
                const string r = simpleRegex(e);
                if ( r.size() ) {
                    lower = addObj( BSON( "" << r ) ).firstElement();
                    upper = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                    upperInclusive = false;
                } else {
                    BSONObjBuilder b1(32), b2(32);
                    b1.appendMinForType( "" , String );
                    lower = addObj( b1.obj() ).firstElement();

                    b2.appendMaxForType( "" , String );
                    upper = addObj( b2.obj() ).firstElement();
                    upperInclusive = false; //MaxForType String is an empty Object
                }

                // regex matches self - regex type > string type
                if (e.type() == RegEx){
                    BSONElement re = addObj( BSON( "" << e ) ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                } else {
                    BSONObj orig = e.embeddedObject();
                    BSONObjBuilder b;
                    b.appendRegex("", orig["$regex"].valuestrsafe(), orig["$options"].valuestrsafe());
                    BSONElement re = addObj( b.obj() ).firstElement();
                    _intervals.push_back( FieldInterval(re) );
                }

            }
            return;
        }
        int op = e.getGtLtOp();
        if ( isNot ) {
            switch( op ) {
                case BSONObj::Equality:
                case BSONObj::opALL:
                case BSONObj::opMOD: // NOTE for mod and type, we could consider having 1-2 intervals comprising the complementary types (multiple intervals already possible with $in)
                case BSONObj::opTYPE:
                    op = BSONObj::NE; // no bound calculation
                    break;
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
                default: // otherwise doesn't matter
                    break;
            }
        }
        switch( op ) {
        case BSONObj::Equality:
            lower = upper = e;
            break;
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
            massert( 10370 ,  "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            bool bound = false;
            while ( i.more() ){
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ){
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
        default:
            break;
        }
        
        if ( optimize ){
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
            }
        }

    }

    void FieldRange::finishOperation( const vector< FieldInterval > &newIntervals, const FieldRange &other ) {
        _intervals = newIntervals;
        for( vector< BSONObj >::const_iterator i = other._objData.begin(); i != other._objData.end(); ++i )
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
    
	// NOTE Not yet tested for complex $or bounds, just for simple bounds generated by $in
    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        vector< FieldInterval >::const_iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) )
                newIntervals.push_back( overlap );
            if ( i->_upper == minFieldBound( i->_upper, j->_upper ) )
                ++i;
            else
                ++j;      
        }
        finishOperation( newIntervals, other );
        return *this;
    }
    
    void handleInterval( const FieldInterval &lower, FieldBound &low, FieldBound &high, vector< FieldInterval > &newIntervals ) {
        if ( low._bound.eoo() ) {
            low = lower._lower; high = lower._upper;
        } else {
            if ( high._bound.woCompare( lower._lower._bound, false ) < 0 ) { // when equal but neither inclusive, just assume they overlap, since current btree scanning code just as efficient either way
                FieldInterval tmp;
                tmp._lower = low;
                tmp._upper = high;
                newIntervals.push_back( tmp );
                low = lower._lower; high = lower._upper;                    
            } else {
                high = lower._upper;
            }
        }        
    }
    
    const FieldRange &FieldRange::operator|=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        FieldBound low;
        FieldBound high;
        vector< FieldInterval >::const_iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( ( cmp == 0 && i->_lower._inclusive ) || cmp < 0 ) {
                handleInterval( *i, low, high, newIntervals );
                ++i;
            } else {
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
        vector< FieldInterval >::iterator i = _intervals.begin();
        vector< FieldInterval >::const_iterator j = other._intervals.begin();
        while( i != _intervals.end() && j != other._intervals.end() ) {
            int cmp = i->_lower._bound.woCompare( j->_lower._bound, false );
            if ( cmp < 0 ||
                ( cmp == 0 && i->_lower._inclusive && !j->_lower._inclusive ) ) {
                int cmp2 = i->_upper._bound.woCompare( j->_lower._bound, false );
                if ( cmp2 < 0 ) {
                    ++i;
                } else if ( cmp2 == 0 ) {
                    if ( i->_upper._inclusive && j->_lower._inclusive ) {
                        i->_upper._inclusive = false;
                    }
                    ++i;
                } else {
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                        ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        i->_upper = j->_lower;
                        i->_upper.flipInclusive();
                        ++i;
                    } else {
                        ++j;
                    }
                }
            } else {
                int cmp2 = i->_lower._bound.woCompare( j->_upper._bound, false );
                if ( cmp2 > 0 ||
                    ( cmp2 == 0 && ( !i->_lower._inclusive || !j->_lower._inclusive ) ) ) {
                    ++j;
                } else {
                    int cmp3 = i->_upper._bound.woCompare( j->_upper._bound, false );
                    if ( cmp3 < 0 ||
                        ( cmp3 == 0 && ( !i->_upper._inclusive || j->_upper._inclusive ) ) ) {
                        i = _intervals.erase( i );
                    } else {
                        i->_lower = j->_upper;
                        i->_lower.flipInclusive();                        
                        ++j;
                    }
                }                
            }
        }
        finishOperation( _intervals, other );
        return *this;        
    }
    
    // TODO write a proper implementation that doesn't do a full copy
    bool FieldRange::operator<=( const FieldRange &other ) {
        FieldRange temp = *this;
        temp -= other;
        return temp.empty();
    }
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        _objData.push_back( o );
        return o;
    }
        
    string FieldRangeSet::getSpecial() const {
        string s = "";
        for ( map<string,FieldRange>::iterator i=_ranges.begin(); i!=_ranges.end(); i++ ){
            if ( i->second.getSpecial().size() == 0 )
                continue;
            uassert( 13033 , "can't have 2 special fields" , s.size() == 0 );
            s = i->second.getSpecial();
        }
        return s;
    }

    void FieldRangeSet::processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize ) {
        BSONElement g = f;
        int op2 = g.getGtLtOp();
        if ( op2 == BSONObj::opALL ) {
            BSONElement h = g;
            massert( 13050 ,  "$all requires array", h.type() == Array );
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
            while ( k.more() ){
                BSONElement h = k.next();
                StringBuilder buf(32);
                buf << fieldName << "." << h.fieldName();
                string fullname = buf.str();
                
                int op3 = getGtLtOp( h );
                if ( op3 == BSONObj::Equality ){
                    _ranges[ fullname ] &= FieldRange( h , isNot , optimize );
                }
                else {
                    BSONObjIterator l( h.embeddedObject() );
                    while ( l.more() ){
                        _ranges[ fullname ] &= FieldRange( l.next() , isNot , optimize );
                    }
                }
            }                        
        } else {
            _ranges[ fieldName ] &= FieldRange( f , isNot , optimize );
        }        
    }
    
    void FieldRangeSet::processQueryField( const BSONElement &e, bool optimize ) {
        bool equality = ( getGtLtOp( e ) == BSONObj::Equality );
        if ( equality && e.type() == Object ) {
            equality = ( strcmp( e.embeddedObject().firstElement().fieldName(), "$not" ) != 0 );
        }
        
        if ( equality || ( e.type() == Object && !e.embeddedObject()[ "$regex" ].eoo() ) ) {
            _ranges[ e.fieldName() ] &= FieldRange( e , false , optimize );
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
                } else {
                    processOpElement( e.fieldName(), f, false, optimize );
                }
            }                
        }   
    }
    
    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query , bool optimize )
        : _ns( ns ), _queries( 1, query.getOwned() ) {
            BSONObjIterator i( _queries[ 0 ] );
            
            while( i.more() ) {
                BSONElement e = i.next();
                // e could be x:1 or x:{$gt:1}
                
                if ( strcmp( e.fieldName(), "$where" ) == 0 ) {
                    continue;
                }
                
                if ( strcmp( e.fieldName(), "$or" ) == 0 ) {                                                                                                                                                        
                    continue;
                }
                
                if ( strcmp( e.fieldName(), "$nor" ) == 0 ) {
                    continue;
                }
                
                processQueryField( e, optimize );
            }   
        }

    FieldRangeOrSet::FieldRangeOrSet( const char *ns, const BSONObj &query , bool optimize )
        : _baseSet( ns, query, optimize ), _orFound() {

        BSONObjIterator i( _baseSet._queries[ 0 ] );
        
        while( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp( e.fieldName(), "$or" ) == 0 ) {                                                                                                                                                        
                massert( 13262, "$or requires nonempty array", e.type() == Array && e.embeddedObject().nFields() > 0 );                                                                                         
                BSONObjIterator j( e.embeddedObject() );                                                                                                                                                        
                while( j.more() ) {                                                                                                                                                                             
                    BSONElement f = j.next();                                                                                                                                                                   
                    massert( 13263, "$or array must contain objects", f.type() == Object );                                                                                                                     
                    _orSets.push_back( FieldRangeSet( ns, f.embeddedObject(), optimize ) );
                    massert( 13291, "$or may not contain 'special' query", _orSets.back().getSpecial().empty() );
                }
                _orFound = true;
                continue;
            }
        }
    }

    FieldRange *FieldRangeSet::trivialRange_ = 0;
    FieldRange &FieldRangeSet::trivialRange() {
        if ( trivialRange_ == 0 )
            trivialRange_ = new FieldRange();
        return *trivialRange_;
    }
    
    BSONObj FieldRangeSet::simplifiedQuery( const BSONObj &_fields ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
                b.append( i->first, 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            const char *name = e.fieldName();
            const FieldRange &range = _ranges[ name ];
            assert( !range.empty() );
            if ( range.equality() )
                b.appendAs( range.min(), name );
            else if ( range.nontrivial() ) {
                BSONObj o;
                BSONObjBuilder c;
                if ( range.min().type() != MinKey )
                    c.appendAs( range.min(), range.minInclusive() ? "$gte" : "$gt" );
                if ( range.max().type() != MaxKey )
                    c.appendAs( range.max(), range.maxInclusive() ? "$lte" : "$lt" );
                o = c.obj();
                b.append( name, o );
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            assert( !i->second.empty() );
            if ( i->second.equality() ) {
                qp._fieldTypes[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower )
                    qp._fieldTypes[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp._fieldTypes[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp._fieldTypes[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
    // TODO get rid of this
    BoundList FieldRangeSet::indexBounds( const BSONObj &keyPattern, int direction ) const {
        typedef vector< pair< shared_ptr< BSONObjBuilder >, shared_ptr< BSONObjBuilder > > > BoundBuilders;
        BoundBuilders builders;
        builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );        
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
                } else {
                    if ( !fr.inQuery() ) {
                        ineq = true;
                    }
                    BoundBuilders newBuilders;
                    const vector< FieldInterval > &intervals = fr.intervals();
                    for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i ) {
                        BSONObj first = i->first->obj();
                        BSONObj second = i->second->obj();
                        if ( forward ) {
                            for( vector< FieldInterval >::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                                uassert( 13303, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < 1000000 );
                                newBuilders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_lower._bound, "" );
                                newBuilders.back().second->appendAs( j->_upper._bound, "" );
                            }
                        } else {
                            for( vector< FieldInterval >::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                                uassert( 13304, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < 1000000 );
                                newBuilders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_upper._bound, "" );
                                newBuilders.back().second->appendAs( j->_lower._bound, "" );
                            }
                        }
                    }
                    builders = newBuilders;
                }
            } else {
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
    
    ///////////////////
    // FieldMatcher //
    ///////////////////
    
    void FieldMatcher::add( const BSONObj& o ){
        massert( 10371 , "can only add to FieldMatcher once", _source.isEmpty());
        _source = o;

        BSONObjIterator i( o );
        int true_false = -1;
        while ( i.more() ){
            BSONElement e = i.next();

            if (e.type() == Object){
                BSONObj obj = e.embeddedObject();
                BSONElement e2 = obj.firstElement();
                if ( strcmp(e2.fieldName(), "$slice") == 0 ){
                    if (e2.isNumber()){
                        int i = e2.numberInt();
                        if (i < 0)
                            add(e.fieldName(), i, -i); // limit is now positive
                        else
                            add(e.fieldName(), 0, i);

                    } else if (e2.type() == Array) {
                        BSONObj arr = e2.embeddedObject();
                        uassert(13099, "$slice array wrong size", arr.nFields() == 2 );

                        BSONObjIterator it(arr);
                        int skip = it.next().numberInt();
                        int limit = it.next().numberInt();
                        uassert(13100, "$slice limit must be positive", limit > 0 );
                        add(e.fieldName(), skip, limit);

                    } else {
                        uassert(13098, "$slice only supports numbers and [skip, limit] arrays", false);
                    }
                } else {
                    uassert(13097, string("Unsupported projection option: ") + obj.firstElement().fieldName(), false);
                }

            } else if (!strcmp(e.fieldName(), "_id") && !e.trueValue()){
                _includeID = false;

            } else {

                add (e.fieldName(), e.trueValue());

                // validate input
                if (true_false == -1){
                    true_false = e.trueValue();
                    _include = !e.trueValue();
                }
                else{
                    uassert( 10053 , "You cannot currently mix including and excluding fields. Contact us if this is an issue." , 
                             (bool)true_false == e.trueValue() );
                }
            }
        }
    }

    void FieldMatcher::add(const string& field, bool include){
        if (field.empty()){ // this is the field the user referred to
            _include = include;
        } else {
            _include = !include;

            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos)); 

            boost::shared_ptr<FieldMatcher>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new FieldMatcher());

            fm->add(rest, include);
        }
    }

    void FieldMatcher::add(const string& field, int skip, int limit){
        _special = true; // can't include or exclude whole object

        if (field.empty()){ // this is the field the user referred to
            _skip = skip;
            _limit = limit;
        } else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos));

            boost::shared_ptr<FieldMatcher>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new FieldMatcher());

            fm->add(rest, skip, limit);
        }
    }

    BSONObj FieldMatcher::getSpec() const{
        return _source;
    }

    //b will be the value part of an array-typed BSONElement
    void FieldMatcher::appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested) const {
        int skip  = nested ?  0 : _skip;
        int limit = nested ? -1 : _limit;

        if (skip < 0){
            skip = max(0, skip + a.nFields());
        }

        int i=0;
        BSONObjIterator it(a);
        while (it.more()){
            BSONElement e = it.next();

            if (skip){
                skip--;
                continue;
            }

            if (limit != -1 && (limit-- == 0)){
                break;
            }

            switch(e.type()){
                case Array:{
                    BSONObjBuilder subb;
                    appendArray(subb , e.embeddedObject(), true);
                    b.appendArray(b.numStr(i++), subb.obj());
                    break;
                }
                case Object:{
                    BSONObjBuilder subb;
                    BSONObjIterator jt(e.embeddedObject());
                    while (jt.more()){
                        append(subb , jt.next());
                    }
                    b.append(b.numStr(i++), subb.obj());
                    break;
                }
                default:
                    if (_include)
                        b.appendAs(e, b.numStr(i++));
            }
        }
    }

    void FieldMatcher::append( BSONObjBuilder& b , const BSONElement& e ) const {
        FieldMap::const_iterator field = _fields.find( e.fieldName() );
        
        if (field == _fields.end()){
            if (_include)
                b.append(e);
        } 
        else {
            FieldMatcher& subfm = *field->second;
            
            if ((subfm._fields.empty() && !subfm._special) || !(e.type()==Object || e.type()==Array) ){
                if (subfm._include)
                    b.append(e);
            }
            else if (e.type() == Object){ 
                BSONObjBuilder subb;
                BSONObjIterator it(e.embeddedObject());
                while (it.more()){
                    subfm.append(subb, it.next());
                }
                b.append(e.fieldName(), subb.obj());

            } 
            else { //Array
                BSONObjBuilder subb;
                subfm.appendArray(subb, e.embeddedObject());
                b.appendArray(e.fieldName(), subb.obj());
            }
        }
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
            } else {
                toCmp = interval._upper._bound;
                toCmpInclusive = interval._upper._inclusive;
            }
            int cmp = toCmp.woCompare( e, false );
            if ( !forward ) {
                cmp = -cmp;
            }
            if ( cmp < 0 ) {
                l = m;
            } else if ( cmp > 0 ) {
                h = m;
            } else {
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
    
    bool FieldRangeVector::matches( const BSONObj &obj ) const {
        BSONObjIterator k( _keyPattern );
        for( int i = 0; i < (int)_ranges.size(); ++i ) {
            if ( _ranges[ i ].empty() ) {
                return false;
            }
            BSONElement kk = k.next();
            int number = (int) kk.number();
            bool forward = ( number >= 0 ? 1 : -1 ) * ( _direction >= 0 ? 1 : -1 ) > 0;
            BSONElement e = obj.getField( kk.fieldName() );
            if ( e.eoo() ) {
                e = staticNull.firstElement();
            }
            if ( e.type() == Array ) {
                BSONObjIterator j( e.embeddedObject() );
                bool match = false;
                while( j.more() ) {
                    if ( matchesElement( j.next(), i, forward ) ) {
                        match = true;
                        break;
                    }
                }
                if ( !match ) {
                    return false;
                }
            } else if ( !matchesElement( e, i, forward ) ) {
                return false;
            }
        }
        return true;
    }
    
    // TODO optimize more
    int FieldRangeVector::Iterator::advance( const BSONObj &curr ) {
        BSONObjIterator j( curr );
        BSONObjIterator o( _v._keyPattern );
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
                    } else if ( diff == 1 ) {
                        int x = _v._ranges[ i ].intervals()[ _i[ i ] ]._upper._bound.woCompare( jj, false );
                        if ( x != 0 ) {
                            latestNonEndpoint = i;
                        }
                    }
                    continue;
                } else { // not in a valid range for this field - determine if and how to advance
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
                    } else {
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
            } else if ( diff == 0 ) { // check if we're past the last interval for this field
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
    
    void FieldRangeVector::Iterator::prepDive() {
        for( int j = 0; j < (int)_i.size(); ++j ) {
            _cmp[ j ] = &_v._ranges[ j ].intervals().front()._lower._bound;
            _inc[ j ] = _v._ranges[ j ].intervals().front()._lower._inclusive;
        }        
    }
    
    struct SimpleRegexUnitTest : UnitTest {
        void run(){
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
        }
    } simple_regex_unittest;


    long long applySkipLimit( long long num , const BSONObj& cmd ){
        BSONElement s = cmd["skip"];
        BSONElement l = cmd["limit"];
        
        if ( s.isNumber() ){
            num = num - s.numberLong();
            if ( num < 0 ) {
                num = 0;
            }
        }
        
        if ( l.isNumber() ){
            long long limit = l.numberLong();
            if ( limit < num ){
                num = limit;
            }
        }

        return num;        
    }

    string debugString( Message& m ){
        stringstream ss;
        ss << "op: " << opToString( m.operation() ) << " len: " << m.size();
        if ( m.operation() >= 2000 && m.operation() < 2100 ){
            DbMessage d(m);
            ss << " ns: " << d.getns();
            switch ( m.operation() ){
            case dbUpdate: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q;
                break;
            }
            case dbInsert:
                ss << d.nextJsObj();
                break;
            case dbDelete: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q;
                break;
            }
            default:
                ss << " CANNOT HANDLE YET";
            }
                    
                
        }
        return ss.str();
    }    

} // namespace mongo
