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

#include "mongo/db/op_observer/op_observer_util.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const OpStateAccumulator::Decoration<std::unique_ptr<ShardingWriteRouter>>
    shardingWriteRouterOpStateAccumulatorDecoration =
        OpStateAccumulator::declareDecoration<std::unique_ptr<ShardingWriteRouter>>();

MONGO_FAIL_POINT_DEFINE(addDestinedRecipient);
MONGO_FAIL_POINT_DEFINE(sleepBetweenInsertOpTimeGenerationAndLogOp);

bool shouldReplicateLocalCatalogIdentifers(const rss::PersistenceProvider& provider,
                                           const VersionContext& vCtx) {
    if (provider.shouldUseReplicatedCatalogIdentifiers()) {
        return true;
    }
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagReplicateLocalCatalogIdentifiers.isEnabled(vCtx, fcvSnapshot);
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
