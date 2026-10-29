// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_2dsphere_index_version_lookup.h"

#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#include <string_view>

namespace mongo::timeseries {

StringMap<int> build2dsphereIndexVersionMap(const Collection& coll) {
    StringMap<int> result;
    const std::string dataPrefix = str::stream() << kBucketDataFieldName << ".";
    auto it = coll.getIndexCatalog()->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (it->more()) {
        const auto* entry = it->next();
        const auto* desc = entry->descriptor();
        BSONElement versionElt =
            desc->infoObj().getField(IndexDescriptor::k2dsphereVersionFieldName);
        if (!versionElt.isNumber()) {
            continue;
        }
        long long version = versionElt.numberLong();
        if (version < 1 || version > 4) {
            continue;
        }
        for (auto&& keyElt : desc->keyPattern()) {
            if (keyElt.valueStringDataSafe() != IndexNames::GEO_2DSPHERE_BUCKET) {
                continue;
            }
            std::string_view keyPath = keyElt.fieldNameStringData();
            if (keyPath.size() <= dataPrefix.size() || !keyPath.starts_with(dataPrefix)) {
                continue;
            }
            result.emplace(std::string(keyPath.substr(dataPrefix.size())),
                           static_cast<int>(version));
        }
    }
    return result;
}

}  // namespace mongo::timeseries
