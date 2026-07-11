// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/s/resharding/sharding_write_router.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// Common fail points for logOp() and logInsertOps().
extern FailPoint addDestinedRecipient;
extern FailPoint sleepBetweenInsertOpTimeGenerationAndLogOp;

/**
 * Returns true when local catalog identifiers should be replicated through the oplog.
 */
bool shouldReplicateLocalCatalogIdentifiers(const rss::PersistenceProvider&);

/**
 * Returns true when ranged truncates should be replicated through the oplog.
 */
bool shouldReplicateRangeTruncates(const rss::PersistenceProvider&, const VersionContext& vCtx);

/**
 * Return true when the 'isTimeseries' field should be set on oplog entries for events on
 * time-series collections.
 * TODO SERVER-127425: Remove this helper function as part of post-9.0 time-series cleanup work.
 */
bool shouldSetIsTimeseriesField(const VersionContext& vCtx);

/**
 * Returns true if gFeatureFlagPrimaryDrivenIndexBuilds is enabled.
 */
bool isPrimaryDrivenIndexBuildEnabled(const VersionContext& vCtx);

BSONObj makeCollModCmdObj(const BSONObj& collModCmd,
                          const CollectionOptions& oldCollOptions,
                          boost::optional<IndexCollModInfo> indexInfo);

class DocumentKey {
public:
    DocumentKey(BSONObj id, boost::optional<BSONObj> shardKey)
        : _id(id.getOwned()), _shardKey(std::move(shardKey)) {
        invariant(!id.isEmpty());

        if (_shardKey) {
            _shardKey = _shardKey->getOwned();
        }
    }

    BSONObj getId() const;
    boost::optional<BSONObj> getShardKey() const;

    BSONObj getShardKeyAndId() const;

private:
    BSONObj _id;
    boost::optional<BSONObj> _shardKey;
};

/**
 * Returns a DocumentKey constructed from the shard key fields, if the collection is sharded,
 * and the _id field, of the given document.
 */
DocumentKey getDocumentKey(const CollectionPtr& coll, BSONObj const& doc);


DocumentKey getDocumentKey(const ShardKeyPattern& shardKeyPattern, BSONObj const& doc);

}  // namespace mongo
