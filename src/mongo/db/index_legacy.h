
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

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

    class IndexDetails;
    class NamespaceDetails;

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
         * This is a no-op unless the object describes a FTS index.  To see what FTS does, look in
         * FTSSpec::fixSpec in fts/fts_spec.cpp.
         */
        static BSONObj adjustIndexSpecObject(const BSONObj& obj);

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
        static BSONObj getMissingField(const BSONObj& infoObj);

        /**
         * Perform any post-build steps for this index.
         *
         * This is a no-op unless the index is a FTS index.  In that case, we set the flag for using
         * power of 2 sizes for space allocation.
         */
        static void postBuildHook(NamespaceDetails* tableToIndex, const IndexDetails& idx);
    };

}  // namespace mongo
