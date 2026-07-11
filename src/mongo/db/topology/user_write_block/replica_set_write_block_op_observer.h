// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * OpObserver for replica set write blocking: enforces replica-set-level write
 * restrictions on mutating operations and applies critical-section document
 * updates from config.replicaSetWritesCriticalSections.
 */
// TODO (SERVER-125476): Change the class modularity to PRIVATE
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ReplicaSetWriteBlockOpObserver final : public OpObserverNoop {
    ReplicaSetWriteBlockOpObserver(const ReplicaSetWriteBlockOpObserver&) = delete;
    ReplicaSetWriteBlockOpObserver& operator=(const ReplicaSetWriteBlockOpObserver&) = delete;

public:
    ReplicaSetWriteBlockOpObserver() = default;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isTimeseries) final;

    void onStartIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) final;

private:
    bool _isReplSetAndCanAcceptWritesForNamespace(OperationContext* opCtx,
                                                  const NamespaceString& nss) const;

    void _checkReplicaSetWriteAllowed(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      ReplicaSetWriteBlockRejectedWriteOp opType);

    void _checkReplicaSetDeleteAllowed(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
