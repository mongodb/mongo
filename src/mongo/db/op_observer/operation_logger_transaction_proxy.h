// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/operation_logger.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Accumulates replicated operations for multi-document transactions and batched WUOW writes.
 * When the operations are ready to be replicated, we compose the final chain of applyOps oplog
 * entries and write it to the actual oplog referenced in '_targetOperationWriter'.
 */
class OperationLoggerTransactionProxy : public OperationLogger {
    OperationLoggerTransactionProxy(const OperationLoggerTransactionProxy&) = delete;
    OperationLoggerTransactionProxy& operator=(const OperationLoggerTransactionProxy&) = delete;

public:
    OperationLoggerTransactionProxy(std::unique_ptr<OperationLogger> targetOperationLogger);
    ~OperationLoggerTransactionProxy() override = default;

    void appendOplogEntryChainInfo(OperationContext* opCtx,
                                   repl::MutableOplogEntry* oplogEntry,
                                   repl::OplogLink* oplogLink,
                                   const std::vector<StmtId>& stmtIds) override;

    repl::OpTime logOp(OperationContext* opCtx, repl::MutableOplogEntry* oplogEntry) override;

    void logOplogRecords(OperationContext* opCtx,
                         const NamespaceString& nss,
                         std::vector<Record>* records,
                         const std::vector<Timestamp>& timestamps,
                         const CollectionPtr& oplogCollection,
                         repl::OpTime finalOpTime,
                         Date_t wallTime) override;

    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx,
                                          std::size_t count,
                                          std::size_t opTimeOffset = 0) override;

private:
    std::unique_ptr<OperationLogger> _targetOperationLogger;
};

}  // namespace mongo
