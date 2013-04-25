/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query_runner.h"
#include "mongo/db/btree.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    // static
    DiskLoc QueryRunner::fastFindSingle(const IndexDetails &indexdetails, const BSONObj& key) {
        const int version = indexdetails.version();
        if (0 == version) {
            return indexdetails.head.btree<V0>()->findSingle(indexdetails, indexdetails.head, key);
        } else {
            verify(1 == version);
            return indexdetails.head.btree<V1>()->findSingle(indexdetails, indexdetails.head, key);
        }
    }

}  // namespace mongo
