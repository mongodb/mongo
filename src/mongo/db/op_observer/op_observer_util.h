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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/router_role_api/sharding_write_router.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

// Common fail points for logOp() and logInsertOps().
extern FailPoint addDestinedRecipient;
extern FailPoint sleepBetweenInsertOpTimeGenerationAndLogOp;

/**
 * Returns true when local catalog identifiers should be replicated through the oplog.
 */
bool shouldReplicateLocalCatalogIdentifers(const rss::PersistenceProvider&,
                                           const VersionContext& vCtx);

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

/**
 * Provides access to the ShardingWriteRouter attached to the op accumulator.
 * The ShardingWriteRouter instance is created in OpObserverImpl and subsequently
 * destroyed in MigrationChunkClonerSourceOpObserver.
 *
 */
extern const OpStateAccumulator::Decoration<std::unique_ptr<ShardingWriteRouter>>
    shardingWriteRouterOpStateAccumulatorDecoration;

}  // namespace MONGO_MOD_PUB mongo
