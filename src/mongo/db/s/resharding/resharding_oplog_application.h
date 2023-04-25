/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <functional>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/shard_role.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/sharding_index_catalog_cache.h"

namespace mongo {
class Collection;
class CollectionPtr;
class NamespaceString;
class OperationContext;

/**
 * Applies an operation from an oplog entry using special rules that apply to resharding.
 */
class ReshardingOplogApplicationRules {
public:
    ReshardingOplogApplicationRules(NamespaceString outputNss,
                                    std::vector<NamespaceString> allStashNss,
                                    size_t myStashIdx,
                                    ShardId donorShardId,
                                    ChunkManager sourceChunkMgr,
                                    ReshardingOplogApplierMetrics* applierMetrics);

    const NamespaceString& getOutputNss() const {
        return _outputNss;
    }

    /**
     * Wraps the op application in a writeConflictRetry loop and is responsible for creating and
     * committing the WUOW.
     */
    Status applyOperation(OperationContext* opCtx,
                          const boost::optional<ShardingIndexesCatalogCache>& gii,
                          const repl::OplogEntry& op) const;

private:
    // Applies an insert operation
    void _applyInsert_inlock(OperationContext* opCtx,
                             Database* db,
                             const CollectionPtr& outputColl,
                             const CollectionPtr& stashColl,
                             const repl::OplogEntry& op) const;

    // Applies an update operation
    void _applyUpdate_inlock(OperationContext* opCtx,
                             Database* db,
                             const CollectionPtr& outputColl,
                             const CollectionPtr& stashColl,
                             const repl::OplogEntry& op) const;

    // Applies a delete operation
    void _applyDelete_inlock(OperationContext* opCtx,
                             const ScopedCollectionAcquisition& outputColl,
                             const ScopedCollectionAcquisition& stashColl,
                             const boost::optional<ShardingIndexesCatalogCache>& gii,
                             const repl::OplogEntry& op) const;

    // Queries '_stashNss' using 'idQuery'.
    BSONObj _queryStashCollById(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const BSONObj& idQuery) const;

    // Namespace where operations should be applied, unless there is an _id conflict.
    const NamespaceString _outputNss;

    // The namespaces of all stash collections, including the stash collection for this donor.
    const std::vector<NamespaceString> _allStashNss;

    // Index into _allStashNss for the namespace where operations are applied if there is an _id
    // conflict.
    const size_t _myStashIdx;

    // Namespace where operations are applied if there is an _id conflict.
    const NamespaceString& _myStashNss;

    // ShardId of the donor shard that the operations being applied originate from.
    const ShardId _donorShardId;

    // The chunk manager for the source namespace and original shard key.
    const ChunkManager _sourceChunkMgr;

    ReshardingOplogApplierMetrics* _applierMetrics;
};

}  // namespace mongo
