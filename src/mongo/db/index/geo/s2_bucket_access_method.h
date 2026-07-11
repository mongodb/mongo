// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/geo/s2_access_method.h"
#include "mongo/db/index/geo/s2_common.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"

#include <set>

namespace mongo {

// Public: instantiated in index_access_method.cpp (index_builds module) and fixSpec() called from
// index_catalog_impl.cpp (catalog_and_routing.shard_role module)
class [[MONGO_MOD_PUBLIC]] S2BucketAccessMethod : public S2AccessMethod {
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
        std::set<long long> allowedVersions = {S2_INDEX_VERSION_4, S2_INDEX_VERSION_3};
        return S2AccessMethod::_fixSpecHelper(specObj, allowedVersions);
    }
};

}  // namespace mongo
