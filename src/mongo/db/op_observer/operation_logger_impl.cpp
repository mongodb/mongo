// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/operation_logger_impl.h"

#include "mongo/db/repl/oplog.h"

namespace mongo {

void OperationLoggerImpl::appendOplogEntryChainInfo(OperationContext* opCtx,
                                                    repl::MutableOplogEntry* oplogEntry,
                                                    repl::OplogLink* oplogLink,
                                                    const std::vector<StmtId>& stmtIds) {
    return repl::appendOplogEntryChainInfo(opCtx, oplogEntry, oplogLink, stmtIds);
}

repl::OpTime OperationLoggerImpl::logOp(OperationContext* opCtx,
                                        repl::MutableOplogEntry* oplogEntry) {
    return repl::logOp(opCtx, oplogEntry);
}

void OperationLoggerImpl::logOplogRecords(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          std::vector<Record>* records,
                                          const std::vector<Timestamp>& timestamps,
                                          const CollectionPtr& oplogCollection,
                                          repl::OpTime finalOpTime,
                                          Date_t wallTime) {
    repl::logOplogRecords(opCtx, nss, records, timestamps, oplogCollection, finalOpTime, wallTime);
}

std::vector<OplogSlot> OperationLoggerImpl::getNextOpTimes(OperationContext* opCtx,
                                                           std::size_t count,
                                                           std::size_t opTimeOffset) {
    return repl::getNextOpTimes(opCtx, count, opTimeOffset);
}

}  // namespace mongo
