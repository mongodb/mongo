/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/catalog/clustered_collection_util.h"

#include "mongo/db/namespace_string.h"

namespace mongo {
namespace clustered_util {

static constexpr StringData kDefaultClusteredIndexName = "_id_"_sd;

void ensureClusteredIndexName(ClusteredIndexSpec& indexSpec) {
    if (!indexSpec.getName()) {
        auto clusterKey = indexSpec.getKey().firstElement().fieldNameStringData();
        if (clusterKey == "_id") {
            indexSpec.setName(kDefaultClusteredIndexName);
        } else {
            indexSpec.setName(StringData(clusterKey + "_1"));
        }
    }
}

ClusteredCollectionInfo makeCanonicalClusteredInfoForLegacyFormat() {
    auto indexSpec = ClusteredIndexSpec{BSON("_id" << 1), true /* unique */};
    indexSpec.setName(kDefaultClusteredIndexName);
    return ClusteredCollectionInfo(std::move(indexSpec), true /* legacy */);
}

ClusteredCollectionInfo makeDefaultClusteredIdIndex() {
    auto indexSpec = ClusteredIndexSpec{BSON("_id" << 1), true /* unique */};
    indexSpec.setName(kDefaultClusteredIndexName);
    return makeCanonicalClusteredInfo(indexSpec);
}

ClusteredCollectionInfo makeCanonicalClusteredInfo(ClusteredIndexSpec indexSpec) {
    ensureClusteredIndexName(indexSpec);
    return ClusteredCollectionInfo(std::move(indexSpec), false);
}

boost::optional<ClusteredCollectionInfo> parseClusteredInfo(const BSONElement& elem) {
    uassert(5979702,
            "'clusteredIndex' has to be a boolean or object.",
            elem.type() == mongo::Bool || elem.type() == mongo::Object);

    bool isLegacyFormat = elem.type() == mongo::Bool;
    if (isLegacyFormat) {
        // Legacy format implies the collection was created with format {clusteredIndex: <bool>}.
        // The legacy format is maintained for backward compatibility with time series buckets
        // collection creation.
        if (!elem.Bool()) {
            // clusteredIndex was specified as false.
            return boost::none;
        }
        return makeCanonicalClusteredInfoForLegacyFormat();
    }

    auto indexSpec = ClusteredIndexSpec::parse({"ClusteredUtil::parseClusteredInfo"}, elem.Obj());
    ensureClusteredIndexName(indexSpec);
    return makeCanonicalClusteredInfo(std::move(indexSpec));
}

bool requiresLegacyFormat(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() || nss.isChangeStreamPreImagesCollection();
}

BSONObj formatClusterKeyForListIndexes(const ClusteredCollectionInfo& collInfo) {
    BSONObjBuilder bob;
    collInfo.getIndexSpec().serialize(&bob);
    bob.append("clustered", true);
    return bob.obj();
}


bool isClusteredOnId(const boost::optional<ClusteredCollectionInfo>& collInfo) {
    if (!collInfo) {
        return false;
    }
    return clustered_util::matchesClusterKey(BSON("_id" << 1), collInfo);
}

bool matchesClusterKey(const BSONObj& obj,
                       const boost::optional<ClusteredCollectionInfo>& collInfo) {
    const auto nFields = obj.nFields();
    invariant(nFields > 0);
    if (nFields > 1) {
        // Clustered key cannot be compound.
        return false;
    }
    return obj.firstElement().fieldNameStringData() ==
        collInfo->getIndexSpec().getKey().firstElement().fieldNameStringData();
}

StringData getClusterKeyFieldName(const ClusteredIndexSpec& indexSpec) {
    return indexSpec.getKey().firstElement().fieldNameStringData();
}

}  // namespace clustered_util
}  // namespace mongo
