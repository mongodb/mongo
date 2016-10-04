
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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class Collection;
class IndexDescriptor;
class OperationContext;

/**
 * There has been some behavior concerning indexed access patterns -- both pre and post-index
 * construction -- that does not quite fit in the access pattern model implemented in
 * index/index_access_pattern.h. Such behavior can't be changed in the current implementation of
 * the code.
 *
 * We grouped such exception/legacy behavior here.
 */
class IndexLegacy {
public:
    /**
     * Adjust the provided index spec BSONObj depending on the type of index obj describes.
     *
     * This is a no-op unless the object describes a TEXT or a GEO_2DSPHERE index.  TEXT and
     * GEO_2DSPHERE provide additional validation on the index spec, and tweak the index spec
     * object to conform to their expected format.
     */
    static StatusWith<BSONObj> adjustIndexSpecObject(const BSONObj& obj);

    /**
     * Returns the BSONObj that is inserted into an index when the object is missing the keys
     * the index is over.
     *
     * For every index *except hash*, this is the BSON equivalent of jstNULL.
     * For the hash index, it's the hash of BSON("" << BSONNULL).
     *
     * s/d_split.cpp needs to know this.
     *
     * This is a significant leak of index functionality out of the index layer.
     */
    static BSONObj getMissingField(OperationContext* txn,
                                   Collection* collection,
                                   const BSONObj& infoObj);
};

}  // namespace mongo
