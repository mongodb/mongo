/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

class S2BucketAccessMethod : public S2AccessMethod {
public:
    S2BucketAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree)
        : S2AccessMethod(btreeState, std::move(btree), IndexNames::GEO_2DSPHERE_BUCKET) {}

    /**
     * Takes an index spec object for this index and returns a copy tweaked to conform to the
     * expected format.  When an index build is initiated, this function is called on the spec
     * object the user provides, and the return value of this function is the final spec object
     * that gets saved in the index catalog.
     *
     * Returns a non-OK status if 'specObj' is invalid.
     */
    static StatusWith<BSONObj> fixSpec(const BSONObj& specObj) {
        return S2AccessMethod::_fixSpecHelper(specObj, /*expectedVersion*/ S2_INDEX_VERSION_3);
    }
};

}  // namespace mongo
