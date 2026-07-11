// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/op_observer_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(addDestinedRecipient);
MONGO_FAIL_POINT_DEFINE(sleepBetweenInsertOpTimeGenerationAndLogOp);

bool shouldReplicateLocalCatalogIdentifiers(const rss::PersistenceProvider& provider) {
    if (provider.shouldUseReplicatedCatalogIdentifiers()) {
        return true;
    }
    return feature_flags::gFeatureFlagReplicateLocalCatalogIdentifiers.isEnabled();
}

bool shouldReplicateRangeTruncates(const rss::PersistenceProvider& provider,
                                   const VersionContext& vCtx) {
    if (provider.shouldUseReplicatedTruncates()) {
        return true;
    }
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagUseReplicatedTruncatesForDeletions.isEnabled(vCtx, fcvSnapshot);
}

bool shouldSetIsTimeseriesField(const VersionContext& vCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return gFeatureFlagMarkTimeseriesEventsInOplog.isEnabled(vCtx, fcvSnapshot);
}

bool isPrimaryDrivenIndexBuildEnabled(const VersionContext& vCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(vCtx, fcvSnapshot);
}

/**
 * Given a raw collMod command object and associated collection metadata, create and return the
 * object for the 'o' field of a collMod oplog entry. For TTL index updates, we make sure the oplog
 * entry always stores the index name, instead of a key pattern.
 */
BSONObj makeCollModCmdObj(const BSONObj& collModCmd,
                          const CollectionOptions& oldCollOptions,
                          boost::optional<IndexCollModInfo> indexInfo) {
    BSONObjBuilder cmdObjBuilder;
    std::string indexFieldName = "index";

    // Add all fields from the original collMod command.
    for (auto elem : collModCmd) {
        // We normalize all TTL collMod oplog entry objects to use the index name, even if the
        // command used an index key pattern.
        if (elem.fieldNameStringData() == indexFieldName && indexInfo) {
            BSONObjBuilder indexObjBuilder;
            indexObjBuilder.append("name", indexInfo->indexName);
            if (indexInfo->expireAfterSeconds)
                indexObjBuilder.append(
                    "expireAfterSeconds",
                    durationCount<Seconds>(indexInfo->expireAfterSeconds.value()));
            if (indexInfo->hidden)
                indexObjBuilder.append("hidden", indexInfo->hidden.value());

            if (indexInfo->unique)
                indexObjBuilder.append("unique", indexInfo->unique.value());

            if (indexInfo->prepareUnique)
                indexObjBuilder.append("prepareUnique", indexInfo->prepareUnique.value());

            if (indexInfo->forceNonUnique)
                indexObjBuilder.append("forceNonUnique", indexInfo->forceNonUnique.value());

            cmdObjBuilder.append(indexFieldName, indexObjBuilder.obj());
        } else {
            cmdObjBuilder.append(elem);
        }
    }

    return cmdObjBuilder.obj();
}

BSONObj DocumentKey::getId() const {
    return _id;
}

boost::optional<BSONObj> DocumentKey::getShardKey() const {
    return _shardKey;
}

BSONObj DocumentKey::getShardKeyAndId() const {
    if (_shardKey) {
        BSONObjBuilder builder(_shardKey.value());
        builder.appendElementsUnique(_id);
        return builder.obj();
    }

    // _shardKey is not set so just return the _id.
    return getId();
}

DocumentKey getDocumentKey(const CollectionPtr& coll, BSONObj const& doc) {
    auto idField = doc["_id"];
    BSONObj id = idField ? idField.wrap() : doc;
    boost::optional<BSONObj> shardKey;

    if (coll.isSharded_DEPRECATED()) {
        shardKey = bson::extractElementsBasedOnTemplate(doc, coll.getShardKeyPattern().toBSON())
                       .getOwned();
    }

    return {std::move(id), std::move(shardKey)};
}

DocumentKey getDocumentKey(const ShardKeyPattern& shardKeyPattern, BSONObj const& doc) {
    auto idField = doc["_id"];
    BSONObj id = idField ? idField.wrap() : doc;

    return {std::move(id),
            bson::extractElementsBasedOnTemplate(doc, shardKeyPattern.toBSON()).getOwned()};
}

}  // namespace mongo
