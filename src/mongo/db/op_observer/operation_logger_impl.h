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
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationLoggerImpl : public OperationLogger {
    OperationLoggerImpl(const OperationLoggerImpl&) = delete;
    OperationLoggerImpl& operator=(const OperationLoggerImpl&) = delete;

public:
    OperationLoggerImpl() = default;
    ~OperationLoggerImpl() override = default;

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
};

}  // namespace mongo
