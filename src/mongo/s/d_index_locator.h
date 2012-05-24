// @file  d_index_locator.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/index.h"
#include <string>

namespace mongo {

    /* Given the minimum and maximum bounds of a chunk range,
     * and the shard key pattern, this locates a relevant index
     * and outputs its details.
     *
     * Requirements:
     *   min and max cannot be empty.
     *   min and max should have the same fields in the same order
     *     (i.e. min = {a : 4} , max = { b : 6} not allowed )
     *   the fields in min and max should be compatible with the keyPattern
     *
     * modifies min, max, and keyPattern to be consistent with the
     * selected index.
     */

     IndexDetails *locateIndexForChunkRange( const char *ns,
                                             string &errmsg,
                                             BSONObj &min,
                                             BSONObj &max,
                                             BSONObj &keyPattern );
}
