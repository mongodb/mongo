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
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <vector>

namespace mongo {

/**
 * Update in-memory cluster server parameters on insert/update/remove.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ClusterServerParameterOpObserver final
    : public OpObserverNoop {
public:
    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfig, NamespaceFilter::kConfig};
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

    void onDropDatabase(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        bool markFromMigrate) final;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) final;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;
};

}  // namespace mongo
