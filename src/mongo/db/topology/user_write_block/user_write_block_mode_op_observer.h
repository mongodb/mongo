// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
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
 * OpObserver for user write blocking. On write operations, checks whether the current global user
 * write blocking state allows the write, and uasserts if not.
 */
// TODO (SERVER-125476): Change the class modularity to PRIVATE
class [[MONGO_MOD_NEEDS_REPLACEMENT]] UserWriteBlockModeOpObserver final : public OpObserverNoop {
    UserWriteBlockModeOpObserver(const UserWriteBlockModeOpObserver&) = delete;
    UserWriteBlockModeOpObserver& operator=(const UserWriteBlockModeOpObserver&) = delete;

public:
    UserWriteBlockModeOpObserver() = default;
    ~UserWriteBlockModeOpObserver() override = default;

    // Operations to check for allowed writes.

    // CUD operations

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

    // DDL operations
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isTimeseries) final;

    // We need to check the startIndexBuild ops because onCreateIndex is only called for empty
    // collections.
    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isTimeseries) final;

    void onStartIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) final;

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isTimeseries,
        bool recordIdsReplicated) final;

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo,
                   bool isTimeseries) final;

    void onDropDatabase(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        bool markFromMigrate) final;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) final;

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& indexInfo,
                     bool isTimeseries) final;

    // onRenameCollection is only for renaming to a nonexistent target NS, so we need
    // preRenameCollection too.
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp,
                                     bool markFromMigrate,
                                     bool isTimeseries) final;

    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate,
                            bool isTimeseries) final;

    void onImportCollection(OperationContext* opCtx,
                            const UUID& importUUID,
                            const NamespaceString& nss,
                            long long numRecords,
                            long long dataSize,
                            const BSONObj& catalogEntry,
                            const BSONObj& storageMetadata,
                            bool isDryRun,
                            bool isTimeseries) final;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

    // Noop operations below with explanations (don't perform any check).

    // Index builds committing (onCommitIndexBuild()) can be left unchecked since we kill any active
    // index builds before enabling write blocking. This means any index build which gets to the
    // commit phase while write blocking is active was started and hit the onStartIndexBuild hook
    // with write blocking active, and thus must be allowed under user write blocking.

    // At the moment we are leaving the onAbortIndexBuild() as
    // unchecked. This is because they can be called from both user and internal codepaths, and
    // we don't want to risk throwing an assert for the internal paths.

    // We don't need to check postRenameCollection() and preRenameCollection (they are in the same
    // WUOW).

    // The on*Transaction*() transaction commit related hooks don't need to be checked, because all
    // of the operations inside the transaction are checked and they all execute in one WUOW.
private:
    // uasserts that a write to the given namespace is allowed under the current user write blocking
    // setting.
    void _checkWriteAllowed(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
