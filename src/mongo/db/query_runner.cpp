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
