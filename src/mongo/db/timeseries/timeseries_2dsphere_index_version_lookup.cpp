/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_2dsphere_index_version_lookup.h"

#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/timeseries/timeseries_constants.h"

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
            StringData keyPath = keyElt.fieldNameStringData();
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
