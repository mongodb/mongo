/** jsobjManipulator.h */

/**
 *    Copyright (C) 2009 10gen Inc.
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

#pragma once

#include "jsobj.h"

namespace mongo {

/** Manipulate the binary representation of a BSONElement in-place.
 Careful, this casts away const.
 */
class BSONElementManipulator {
public:
    BSONElementManipulator( const BSONElement &element ) :
    element_( element ) {
        assert( !element_.eoo() );
    }
    /** Replace a Timestamp type with a Date type initialized to
     OpTime::now().asDate()
     */
    void initTimestamp();

    /** Change the value, in place, of the number. */
    void setNumber(double d) {
        if ( element_.type() == NumberDouble ) *reinterpret_cast< double * >( value() )  = d;
        else if ( element_.type() == NumberInt ) *reinterpret_cast< int * >( value() ) = (int) d;
    }
    void setLong(long long n) { 
        if( element_.type() == NumberLong ) *reinterpret_cast< long long * >( value() ) = n;
    }

    /** Replace the type and value of the element with the type and value of e,
        preserving the original fieldName */
    void replaceTypeAndValue( const BSONElement &e ) {
        *data() = e.type();
        memcpy( value(), e.value(), e.valuesize() );
    }
    
    static void lookForTimestamps( const BSONObj& obj ){
        // If have a Timestamp field as the first or second element,
        // update it to a Date field set to OpTime::now().asDate().  The
        // replacement policy is a work in progress.
        
        BSONObjIterator i( obj );
        for( int j = 0; i.moreWithEOO() && j < 2; ++j ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( e.type() == Timestamp ){
                BSONElementManipulator( e ).initTimestamp();
                break;
            }
        }
    }
private:
    char *data() { return nonConst( element_.rawdata() ); }
    char *value() { return nonConst( element_.value() ); }
    static char *nonConst( const char *s ) { return const_cast< char * >( s ); }
    const BSONElement element_;
};

} // namespace mongo
