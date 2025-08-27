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


#include "mongo/db/local_catalog/clustered_collection_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace clustered_util {

void ensureClusteredIndexName(ClusteredIndexSpec& indexSpec) {
    if (!indexSpec.getName()) {
        auto clusterKey = indexSpec.getKey().firstElement().fieldNameStringData();
        if (clusterKey == "_id") {
            indexSpec.setName(IndexConstants::kIdIndexName);
        } else {
            indexSpec.setName(clusterKey + "_1");
        }
    }
}

ClusteredCollectionInfo makeCanonicalClusteredInfoForLegacyFormat() {
    auto indexSpec = ClusteredIndexSpec{BSON("_id" << 1), true /* unique */};
    indexSpec.setName(IndexConstants::kIdIndexName);
    return ClusteredCollectionInfo(std::move(indexSpec), true /* legacy */);
}

ClusteredCollectionInfo makeDefaultClusteredIdIndex() {
    auto indexSpec = ClusteredIndexSpec{BSON("_id" << 1), true /* unique */};
    indexSpec.setName(IndexConstants::kIdIndexName);
    return makeCanonicalClusteredInfo(indexSpec);
}

ClusteredCollectionInfo makeCanonicalClusteredInfo(ClusteredIndexSpec indexSpec) {
    ensureClusteredIndexName(indexSpec);
    return ClusteredCollectionInfo(std::move(indexSpec), false);
}

boost::optional<ClusteredCollectionInfo> parseClusteredInfo(const BSONElement& elem) {
    uassert(5979702,
            "'clusteredIndex' has to be a boolean or object.",
            elem.type() == BSONType::boolean || elem.type() == BSONType::object);

    bool isLegacyFormat = elem.type() == BSONType::boolean;
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

    auto indexSpec = ClusteredIndexSpec::parse(
        elem.Obj(), IDLParserContext{"ClusteredUtil::parseClusteredInfo"});
    ensureClusteredIndexName(indexSpec);
    return makeCanonicalClusteredInfo(std::move(indexSpec));
}

bool requiresLegacyFormat(const NamespaceString& nss, const CollectionOptions& collOptions) {
    return collOptions.timeseries || nss.isChangeStreamPreImagesCollection();
}

BSONObj formatClusterKeyForListIndexes(const ClusteredCollectionInfo& collInfo,
                                       const BSONObj& collation,
                                       const boost::optional<int64_t>& expireAfterSeconds) {
    BSONObjBuilder bob;
    collInfo.getIndexSpec().serialize(&bob);
    if (!collation.isEmpty()) {
        bob.append("collation", collation);
    }
    if (expireAfterSeconds) {
        bob.append("expireAfterSeconds", expireAfterSeconds.value());
    }
    bob.append("clustered", true);
    return bob.obj();
}

bool isClusteredOnId(const boost::optional<ClusteredCollectionInfo>& collInfo) {
    return collInfo && "_id"_sd == getClusterKeyFieldName(collInfo->getIndexSpec());
}

bool matchesClusterKey(const BSONObj& keyPatternObj,
                       const boost::optional<ClusteredCollectionInfo>& collInfo) {
    if (!collInfo) {
        return false;
    }

    const auto nFields = keyPatternObj.nFields();
    invariant(nFields > 0);
    if (nFields > 1) {
        // Clustered key cannot be compound.
        return false;
    }

    if (!keyPatternObj.firstElement().isNumber()) {
        // Clustered index can't be of any special type.
        return false;
    }

    return keyPatternObj.firstElement().fieldNameStringData() ==
        collInfo->getIndexSpec().getKey().firstElement().fieldNameStringData();
}

StringData getClusterKeyFieldName(const ClusteredIndexSpec& indexSpec) {
    return indexSpec.getKey().firstElement().fieldNameStringData();
}

BSONObj getSortPattern(const ClusteredIndexSpec& indexSpec) {
    return indexSpec.getKey();
}

void checkCreationOptions(const CreateCommand& cmd) {
    uassert(ErrorCodes::Error(6049200),
            str::stream() << "'size' field for capped collections is not allowed on clustered "
                             "collections. Did you mean 'capped: true' with 'expireAfterSeconds'?",
            !cmd.getSize());

    uassert(ErrorCodes::Error(6049204),
            str::stream() << "'max' field for capped collections is not allowed on clustered "
                             "collections. Did you mean 'capped: true' with 'expireAfterSeconds'?",
            !cmd.getMax());

    if (cmd.getCapped()) {
        uassert(
            ErrorCodes::Error(6127800),
            "Clustered capped collection only available with 'enableTestCommands' server parameter",
            getTestCommandsEnabled());

        uassert(ErrorCodes::Error(6049201),
                "A capped clustered collection requires the 'expireAfterSeconds' field",
                cmd.getExpireAfterSeconds());
    }

    if (cmd.getTimeseries()) {
        uassert(ErrorCodes::InvalidOptions,
                "Invalid option 'clusteredIndex: false': clustered index can't be disabled for "
                "timeseries collection",
                !cmd.getClusteredIndex() || !holds_alternative<bool>(*cmd.getClusteredIndex()) ||
                    get<bool>(*cmd.getClusteredIndex()));
    }
}

}  // namespace clustered_util
}  // namespace mongo
