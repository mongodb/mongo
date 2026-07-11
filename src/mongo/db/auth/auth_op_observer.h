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
 * OpObserver for authentication. Observes all secondary replication traffic and filters down to
 * relevant entries for authentication.
 */
class [[MONGO_MOD_PUBLIC]] AuthOpObserver final : public OpObserverNoop {
    AuthOpObserver(const AuthOpObserver&) = delete;
    AuthOpObserver& operator=(const AuthOpObserver&) = delete;

public:
    AuthOpObserver();
    ~AuthOpObserver() override;

    // The filtering for this OpObserver is derived from the namespace filters in
    // AuthorizationManagerImpl and the various AuditInterface implementations, see SERVER-83383.
    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kSystem, NamespaceFilter::kSystem};
    }

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

    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) final;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;
};

}  // namespace mongo
