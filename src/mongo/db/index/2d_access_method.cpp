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

#include "mongo/db/index/2d_access_method.h"

#include <string>
#include <vector>

#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

namespace mongo {

TwoDAccessMethod::TwoDAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
    : IndexAccessMethod(btreeState, btree) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    ExpressionParams::parseTwoDParams(descriptor->infoObj(), &_params);
}

/** Finds the key objects to put in an index */
void TwoDAccessMethod::getKeys(const BSONObj& obj,
                               BSONObjSet* keys,
                               MultikeyPaths* multikeyPaths) const {
    ExpressionKeysPrivate::get2DKeys(obj, _params, keys, NULL);
}

/** Finds all locations in a geo-indexed object */
void TwoDAccessMethod::getKeys(const BSONObj& obj, std::vector<BSONObj>& locs) const {
    ExpressionKeysPrivate::get2DKeys(obj, _params, NULL, &locs);
}

}  // namespace mongo
