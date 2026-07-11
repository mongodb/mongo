// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <vector>

namespace mongo {

/**
 * OpObserver which is installed on the op observers chain when the server is running as a config
 * server (--configsvr).
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ConfigServerOpObserver final : public OpObserverNoop {
    ConfigServerOpObserver(const ConfigServerOpObserver&) = delete;
    ConfigServerOpObserver& operator=(const ConfigServerOpObserver&) = delete;

public:
    ConfigServerOpObserver();
    ~ConfigServerOpObserver() override;

    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfigAndSystem, NamespaceFilter::kConfigAndSystem};
    }

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

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) override;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) override;
};

}  // namespace mongo
