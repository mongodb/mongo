// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"                           // for OplogLink
#include "mongo/db/repl/oplog_entry.h"                     // for MutableOplogEntry
#include "mongo/db/session/logical_session_id.h"           // for StmtId
#include "mongo/db/shard_role/shard_catalog/collection.h"  // for CollectionPtr
#include "mongo/db/storage/record_store.h"                 // for Record
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"  // for Date_t

#include <cstddef>  // for std::size_t
#include <vector>

namespace mongo {

/**
 * This interface provides methods to append entries to the oplog collection with additional
 * support for various buffering/write-through semantics depending on the caller's current
 * write context (multi-doc transaction, batched writes, or single-operation WriteUnitOfWork).
 *
 * The actual oplog entry inserts would be delegated to the repl::logOp() methods in repl/oplog.h.
 *
 * The primary consumer of this interface would be the OpObserverImpl implementation, which
 * used to append to the oplog directly once it has assembled the final oplog entry for single write
 * operation to be inserted immediately; a multi-doc transaction or a batched write (multiple write
 * ops encapsulated in a single applyOps entry).
 */
class OperationLogger {
public:
    virtual ~OperationLogger() = default;

    /**
     * Set the "lsid", "txnNumber", "stmtId", "prevOpTime", "preImageOpTime" and "postImageOpTime"
     * fields of the oplogEntry based on the given oplogLink for retryable writes (i.e. when
     * stmtIds.front() != kUninitializedStmtId).
     *
     * Refer to repl::appendOplogEntryChainInfo() in repl/oplog.h.
     */
    virtual void appendOplogEntryChainInfo(OperationContext* opCtx,
                                           repl::MutableOplogEntry* oplogEntry,
                                           repl::OplogLink* oplogLink,
                                           const std::vector<StmtId>& stmtIds) = 0;

    /**
     * Returns the optime of the oplog entry written to the oplog.
     * Returns a null optime if oplog was not modified.
     */
    virtual repl::OpTime logOp(OperationContext* opCtx, repl::MutableOplogEntry* oplogEntry) = 0;

    /**
     * Low level oplog function used by logOp() and similar functions to append
     * storage engine records to the oplog collection.
     *
     * This function has to be called within the scope of a WriteUnitOfWork with
     * a valid CollectionPtr reference to the oplog.
     *
     * @param records a vector of oplog records to be written. Records hold references
     * to unowned BSONObj data.
     * @param timestamps a vector of respective Timestamp objects for each oplog record.
     * @param oplogCollection collection to be written to.
     * @param finalOpTime the OpTime of the last oplog record.
     * @param wallTime the wall clock time of the last oplog record.
     */
    virtual void logOplogRecords(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 std::vector<Record>* records,
                                 const std::vector<Timestamp>& timestamps,
                                 const CollectionPtr& oplogCollection,
                                 repl::OpTime finalOpTime,
                                 Date_t wallTime) = 0;

    /**
     * Allocates optimes for new entries in the oplog.  Returns a vector of OplogSlots, which
     * contain the new optimes along with their terms.
     */
    virtual std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx,
                                                  std::size_t count,
                                                  std::size_t opTimeOffset = 0) = 0;
};

}  // namespace mongo
