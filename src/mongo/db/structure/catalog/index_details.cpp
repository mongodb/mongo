// index_details.cpp

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

#include "mongo/pch.h"

#include "mongo/db/structure/catalog/index_details.h"

#include <boost/checked_delete.hpp>

#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    int IndexDetails::keyPatternOffset( const string& key ) const {
        BSONObjIterator i( keyPattern() );
        int n = 0;
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( key == e.fieldName() )
                return n;
            n++;
        }
        return -1;
    }

    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
       TOOD: above comment is wrong, also, document durability assumptions
    */
    void IndexDetails::_reset() {
        head.setInvalid();
        info.setInvalid();
    }

    bool IndexDetails::areIndexOptionsEquivalent(const BSONObj& newSpec ) const {
        if ( dropDups() != newSpec["dropDups"].trueValue() ) {
            return false;
        }

        const BSONElement sparseSpecs = info.obj().getField("sparse");

        if ( sparseSpecs.trueValue() != newSpec["sparse"].trueValue() ) {
            return false;
        }

        // Note: { _id: 1 } or { _id: -1 } implies unique: true.
        if ( !isIdIndex() &&
             unique() != newSpec["unique"].trueValue() ) {
            return false;
        }

        const BSONElement existingExpireSecs =
                info.obj().getField("expireAfterSeconds");
        const BSONElement newExpireSecs = newSpec["expireAfterSeconds"];

        return existingExpireSecs == newExpireSecs;
    }

}
