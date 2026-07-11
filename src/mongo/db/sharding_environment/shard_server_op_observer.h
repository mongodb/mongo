// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * OpObserver which is installed on the op observers chain when the server is running as a shard
 * server (--shardsvr).
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardServerOpObserver final : public OpObserverNoop {
    ShardServerOpObserver(const ShardServerOpObserver&) = delete;
    ShardServerOpObserver& operator=(const ShardServerOpObserver&) = delete;

public:
    ShardServerOpObserver();
    ~ShardServerOpObserver() override;


    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfigAndSystem, NamespaceFilter::kConfigAndSystem};
    }

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isTimeseries) override;

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isTimeseries) override;

    void onStartIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) override;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isTimeseries,
        bool recordIdsReplicated) override;

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo,
                   bool isTimeseries) override;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) override;

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& indexInfo,
                     bool isTimeseries) override;

    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override;

    void onCreateDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override;

    void onDropDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override;

    void onInvalidateCollectionMetadata(OperationContext* opCtx,
                                        const repl::OplogEntry& op) override;

    void onSetAllowChunkOperations(OperationContext* opCtx, const repl::OplogEntry& op) override;

    void onUpdateCollectionMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override;
};

}  // namespace mongo
