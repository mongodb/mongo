/* queryoptimizer.cpp */

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
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "btree.h"
#include "../util/lruishmap.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "scanandorder.h"
#include "queryoptimizer.h"

namespace mongo {

    FieldBound::FieldBound( BSONElement e ) :
    lower_( minKey.firstElement() ),
    upper_( maxKey.firstElement() ) {
        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const char *r = e.simpleRegex();
            cout << "r: " << r << endl;
            if ( r ) {
                lower_ = addObj( BSON( "" << r ) ).firstElement();
                upper_ = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                cout << "lower_: " << lower_ << endl;
                cout << "upper_: " << upper_ << endl;
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
            case JSMatcher::Equality:
                lower_ = e;
                upper_ = e;   
                break;
            case JSMatcher::LT:
            case JSMatcher::LTE:
                upper_ = e;
                break;
            case JSMatcher::GT:
            case JSMatcher::GTE:
                lower_ = e;
                break;
            default:
                break;
        }
        cout << "lower_: " << lower_ << endl;
        cout << "upper_: " << upper_ << endl;
    }
    
    FieldBound &FieldBound::operator&=( const FieldBound &other ) {
        if ( other.upper_.woCompare( upper_, false ) < 0 )
            upper_ = other.upper_;
        if ( other.lower_.woCompare( lower_, false ) > 0 )
            lower_ = other.lower_;
        massert( "Invalid bounds", lower_.woCompare( upper_, false ) <= 0 );
        return *this;
    }
    
    string FieldBound::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldBound::addObj( BSONObj o ) {
        cout << "o: " << o.toString() << endl;
        objData_.push_back( o );
        return o;
    }
    
    FieldBoundSet::FieldBoundSet( BSONObj query ) :
    query_( query.copy() ) {
        BSONObjIterator i( query_ );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( getGtLtOp( e ) == JSMatcher::Equality )
                bounds_[ e.fieldName() ] &= FieldBound( e );
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    bounds_[ e.fieldName() ] &= FieldBound( f );
                }                
            }
        }
    }
    
    QueryPlan QueryOptimizer::getPlan(
        const char *ns,
        BSONObj* query,
        BSONObj* order,
        BSONObj* hint)
    {
        QueryPlan plan;



        return plan;
    }

} // namespace mongo
