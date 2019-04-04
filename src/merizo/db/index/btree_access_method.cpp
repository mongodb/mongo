/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "merizo/db/index/btree_access_method.h"

#include <vector>

#include "merizo/base/status.h"
#include "merizo/base/status_with.h"
#include "merizo/db/catalog/index_catalog_entry.h"
#include "merizo/db/jsobj.h"
#include "merizo/db/keypattern.h"

namespace merizo {

using std::vector;

// Standard Btree implementation below.
BtreeAccessMethod::BtreeAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
    : AbstractIndexAccessMethod(btreeState, btree) {
    // The key generation wants these values.
    vector<const char*> fieldNames;
    vector<BSONElement> fixed;

    BSONObjIterator it(_descriptor->keyPattern());
    while (it.more()) {
        BSONElement elt = it.next();
        fieldNames.push_back(elt.fieldName());
        fixed.push_back(BSONElement());
    }

    _keyGenerator = std::make_unique<BtreeKeyGenerator>(
        fieldNames, fixed, _descriptor->isSparse(), btreeState->getCollator());
}

void BtreeAccessMethod::doGetKeys(const BSONObj& obj,
                                  BSONObjSet* keys,
                                  BSONObjSet* multikeyMetadataKeys,
                                  MultikeyPaths* multikeyPaths) const {
    _keyGenerator->getKeys(obj, keys, multikeyPaths);
}

}  // namespace merizo
