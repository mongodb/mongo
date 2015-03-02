/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/jsobj.h"

#include "mongo/util/stringutils.h"

namespace mongo {
    /** Compare two bson elements, provided as const char *'s, by field name. */
    class BSONIteratorSorted::ElementFieldCmp {
    public:
        ElementFieldCmp( bool isArray );
        bool operator()( const char *s1, const char *s2 ) const;
    private:
        LexNumCmp _cmp;
    };
    
    BSONIteratorSorted::ElementFieldCmp::ElementFieldCmp( bool isArray ) :
    _cmp( !isArray ) {
    }

    bool BSONIteratorSorted::ElementFieldCmp::operator()( const char *s1, const char *s2 )
    const {
        // Skip the type byte and compare field names.
        return _cmp( s1 + 1, s2 + 1 );
    }        
    
    BSONIteratorSorted::BSONIteratorSorted( const BSONObj &o, const ElementFieldCmp &cmp )
        : _nfields(o.nFields()), _fields(new const char*[_nfields]) {
        int x = 0;
        BSONObjIterator i( o );
        while ( i.more() ) {
            _fields[x++] = i.next().rawdata();
            verify( _fields[x-1] );
        }
        verify( x == _nfields );
        std::sort( _fields.get() , _fields.get() + _nfields , cmp );
        _cur = 0;
    }
    
    BSONObjIteratorSorted::BSONObjIteratorSorted( const BSONObj &object ) :
    BSONIteratorSorted( object, ElementFieldCmp( false ) ) {
    }

    BSONArrayIteratorSorted::BSONArrayIteratorSorted( const BSONArray &array ) :
    BSONIteratorSorted( array, ElementFieldCmp( true ) ) {
    }
} // namespace mongo
