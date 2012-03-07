// @file namespace-inl.h

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

#include "mongo/db/namespace_details.h"

namespace mongo {

    inline IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected ) {
        if( idxNo < NIndexesBase ) {
            IndexDetails& id = _indexes[idxNo];
            return id;
        }
        Extra *e = extra();
        if ( ! e ) {
            if ( missingExpected )
                throw MsgAssertionException( 13283 , "Missing Extra" );
            massert(14045, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ) {
                if ( missingExpected )
                    throw MsgAssertionException( 14823 , "missing extra" );
                massert(14824, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }

    inline int NamespaceDetails::idxNo(IndexDetails& idx) {
        IndexIterator i = ii();
        while( i.more() ) {
            if( &i.next() == &idx )
                return i.pos()-1;
        }
        massert( 10349 , "E12000 idxNo fails", false);
        return -1;
    }

    inline int NamespaceDetails::findIndexByKeyPattern(const BSONObj& keyPattern) {
        IndexIterator i = ii();
        while( i.more() ) {
            if( i.next().keyPattern() == keyPattern )
                return i.pos()-1;
        }
        return -1;
    }

    // @return offset in indexes[]
    inline int NamespaceDetails::findIndexByName(const char *name) {
        IndexIterator i = ii();
        while( i.more() ) {
            if ( strcmp(i.next().info.obj().getStringField("name"),name) == 0 )
                return i.pos()-1;
        }
        return -1;
    }

    inline NamespaceDetails::IndexIterator::IndexIterator(NamespaceDetails *_d) {
        d = _d;
        i = 0;
        n = d->nIndexes;
    }

}
