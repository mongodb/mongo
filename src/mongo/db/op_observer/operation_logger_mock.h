// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/op_observer/operation_logger.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] OperationLoggerMock : public OperationLogger {
    OperationLoggerMock(const OperationLoggerMock&) = delete;
    OperationLoggerMock& operator=(const OperationLoggerMock&) = delete;

public:
    OperationLoggerMock() = default;
    ~OperationLoggerMock() override = default;

    void appendOplogEntryChainInfo(OperationContext* opCtx,
                                   repl::MutableOplogEntry* oplogEntry,
                                   repl::OplogLink* oplogLink,
                                   const std::vector<StmtId>& stmtIds) override {}

    repl::OpTime logOp(OperationContext* opCtx, repl::MutableOplogEntry* oplogEntry) override {
        return {};
    }

    void logOplogRecords(OperationContext* opCtx,
                         const NamespaceString& nss,
                         std::vector<Record>* records,
                         const std::vector<Timestamp>& timestamps,
                         const CollectionPtr& oplogCollection,
                         repl::OpTime finalOpTime,
                         Date_t wallTime) override {}

    /**
     * Returns a vector of 'count' non-null OpTimes.
     * Some tests have to populate test collections, which may require OpObserverImpl::onInserts()
     * to be able to acquire non-null optimes for insert operations even though no oplog entries
     * are appended to the oplog.
     * If the test requires actual OpTimes to work, use OperationLoggerImpl instead.
     */
    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx,
                                          std::size_t count,
                                          std::size_t opTimeOffset = 0) override {
        return std::vector<OplogSlot>{count, OplogSlot(Timestamp(1, 1), /*term=*/1LL)};
    }
};

}  // namespace mongo
