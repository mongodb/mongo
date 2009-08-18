// queryutil.cpp

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

#include "stdafx.h"

#include "btree.h"
#include "pdfile.h"
#include "queryoptimizer.h"

namespace mongo {
    
    FieldRange::FieldRange( const BSONElement &e, bool optimize ) : intervals_( 1 ) {
        lower() = minKey.firstElement();
        lowerInclusive() = true;
        upper() = maxKey.firstElement();
        upperInclusive() = true;
        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const string r = e.simpleRegex();
            if ( r.size() ) {
                lower() = addObj( BSON( "" << r ) ).firstElement();
                upper() = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                upperInclusive() = false;
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
        case BSONObj::Equality:
            lower() = e;
            upper() = e;
            break;
        case BSONObj::LT:
            upperInclusive() = false;
        case BSONObj::LTE:
            upper() = e;
            break;
        case BSONObj::GT:
            lowerInclusive() = false;
        case BSONObj::GTE:
            lower() = e;
            break;
	    case BSONObj::opALL: {
	        massert( "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            if ( i.moreWithEOO() ) {
                BSONElement f = i.next();
                if ( !f.eoo() )
                    lower() = upper() = f;
            }
            break;
	    }
        case BSONObj::opMOD: {
            break;
        }
	    case BSONObj::opIN: {
            massert( "$in requires array", e.type() == Array );
            BSONElement max = minKey.firstElement();
            BSONElement min = maxKey.firstElement();
            BSONObjIterator i( e.embeddedObject() );
            while( i.moreWithEOO() ) {
                BSONElement f = i.next();
                if ( f.eoo() )
                    break;
                if ( max.woCompare( f, false ) < 0 )
                    max = f;
                if ( min.woCompare( f, false ) > 0 )
                    min = f;
            }
            lower() = min;
            upper() = max;
        }
        default:
            break;
        }
        
        if ( optimize ){
            if ( lower().type() != MinKey && upper().type() == MaxKey && lower().isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower().fieldName() , lower().type() );
                upper() = addObj( b.obj() ).firstElement();
            }
            else if ( lower().type() == MinKey && upper().type() != MaxKey && upper().isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper().fieldName() , upper().type() );
                lower() = addObj( b.obj() ).firstElement();
            }
        }

    }

    // as called, these functions find the max/min of a bound in the
    // opposite direction, so exclusive bounds are considered less
    // superlative
    FieldBound maxFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp < 0 )
            return b;
        return a;
    }

    FieldBound minFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp > 0 )
            return b;
        return a;
    }

    bool fieldIntervalOverlap( const FieldInterval &one, const FieldInterval &two, FieldInterval &result ) {
        result.lower_ = maxFieldBound( one.lower_, two.lower_ );
        result.upper_ = minFieldBound( one.upper_, two.upper_ );
        return result.valid();
    }
    
    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        vector< FieldInterval >::const_iterator i = intervals_.begin();
        vector< FieldInterval >::const_iterator j = other.intervals_.begin();
        while( i != intervals_.end() && j != other.intervals_.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) )
                newIntervals.push_back( overlap );
            if ( i->upper_ == minFieldBound( i->upper_, j->upper_ ) )
                ++i;
            else
                ++j;      
        }
        intervals_ = newIntervals;
        // temporary
        if ( intervals_.size() == 0 ) {
            FieldInterval a;
            intervals_.push_back( a );
        }
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        return *this;
    }
    
    string FieldRange::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        objData_.push_back( o );
        return o;
    }
    
    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query , bool optimize ) :
    ns_( ns ),
    query_( query.getOwned() ) {
        BSONObjIterator i( query_ );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( strcmp( e.fieldName(), "$where" ) == 0 )
                continue;
            if ( getGtLtOp( e ) == BSONObj::Equality ) {
                ranges_[ e.fieldName() ] &= FieldRange( e , optimize );
            }
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.moreWithEOO() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    ranges_[ e.fieldName() ] &= FieldRange( f , optimize );
                }                
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
            for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
                b.append( i->first.c_str(), 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const char *name = e.fieldName();
            const FieldRange &range = ranges_[ name ];
            if ( range.equality() )
                b.appendAs( range.min(), name );
            else if ( range.nontrivial() ) {
                BSONObjBuilder c;
                if ( range.min().type() != MinKey )
                    c.appendAs( range.min(), range.minInclusive() ? "$gte" : "$gt" );
                if ( range.max().type() != MaxKey )
                    c.appendAs( range.max(), range.maxInclusive() ? "$lte" : "$lt" );
                b.append( name, c.done() );                
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
            if ( i->second.equality() ) {
                qp.fieldTypes_[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
    void FieldMatcher::add( const BSONObj& o ){
        BSONObjIterator i( o );
        while ( i.more() ){
            string s = i.next().fieldName();
            if ( s.find( "." ) == string::npos ){
                fields.insert( pair<string,string>( s , "" ) );
            }
            else {
                string sub = s.substr( 0 , s.find( "." ) );
                fields.insert(pair<string,string>( sub , s.substr( sub.size() + 1 ) ) );
            }
        }

    }
    
    int FieldMatcher::size() const {
        return fields.size();
    }

    bool FieldMatcher::matches( const string& s ) const {
        return fields.find( s ) != fields.end();
    }
    
    BSONObj FieldMatcher::getSpec() const{
        BSONObjBuilder b;
        for ( multimap<string,string>::const_iterator i=fields.begin(); i!=fields.end(); i++ ) {
            string s = i->first;
            if ( i->second.size() > 0 )
                s += "." + i->second;
            b.append( s.c_str() , 1 );
        }
        return b.obj();
    }

    void FieldMatcher::extractDotted( const string& path , const BSONObj& o , BSONObjBuilder& b ) const {
        string::size_type i = path.find( "." );
        if ( i == string::npos ){
            const BSONElement & e = o.getField( path.c_str() );
            if ( e.eoo() )
                return;
            b.append(e);
            return;
        }
        
        string left = path.substr( 0 , i );
        BSONElement e = o[left];
        if ( e.type() != Object )
            return;

        BSONObj sub = e.embeddedObject();
        if ( sub.isEmpty() )
            return;
        
        BSONObjBuilder sub_b(32);
        extractDotted( path.substr( i + 1 ) , sub , sub_b );
        b.append( left.c_str() , sub_b.obj() );
    }
    
    void FieldMatcher::append( BSONObjBuilder& b , const BSONElement& e ) const {
        pair<multimap<string,string>::const_iterator,multimap<string,string>::const_iterator> p = fields.equal_range( e.fieldName() );
        BSONObjBuilder sub_b(32);

        for( multimap<string,string>::const_iterator i = p.first; i != p.second; ++i ) {
            string next = i->second;

            if ( e.eoo() ){
            }
            else if ( next.size() == 0 || next == "." || e.type() != Object ){
                b.append( e );
                return;
            }
            else {
                extractDotted( next , e.embeddedObject() , sub_b );
            }
        }

        b.append( e.fieldName() , sub_b.obj() );
    }
    
} // namespace mongo
