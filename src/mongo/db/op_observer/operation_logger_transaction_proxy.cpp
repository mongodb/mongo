// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/operation_logger_transaction_proxy.h"

#include <utility>

namespace mongo {

OperationLoggerTransactionProxy::OperationLoggerTransactionProxy(
    std::unique_ptr<OperationLogger> targetOperationLogger)
    : _targetOperationLogger(std::move(targetOperationLogger)) {}

void OperationLoggerTransactionProxy::appendOplogEntryChainInfo(
    OperationContext* opCtx,
    repl::MutableOplogEntry* oplogEntry,
    repl::OplogLink* oplogLink,
    const std::vector<StmtId>& stmtIds) {
    return _targetOperationLogger->appendOplogEntryChainInfo(opCtx, oplogEntry, oplogLink, stmtIds);
}

repl::OpTime OperationLoggerTransactionProxy::logOp(OperationContext* opCtx,
                                                    repl::MutableOplogEntry* oplogEntry) {
    return _targetOperationLogger->logOp(opCtx, oplogEntry);
}

void OperationLoggerTransactionProxy::logOplogRecords(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      std::vector<Record>* records,
                                                      const std::vector<Timestamp>& timestamps,
                                                      const CollectionPtr& oplogCollection,
                                                      repl::OpTime finalOpTime,
                                                      Date_t wallTime) {
    _targetOperationLogger->logOplogRecords(
        opCtx, nss, records, timestamps, oplogCollection, finalOpTime, wallTime);
}

std::vector<OplogSlot> OperationLoggerTransactionProxy::getNextOpTimes(OperationContext* opCtx,
                                                                       std::size_t count,
                                                                       std::size_t opTimeOffset) {
    return _targetOperationLogger->getNextOpTimes(opCtx, count, opTimeOffset);
}

}  // namespace mongo
