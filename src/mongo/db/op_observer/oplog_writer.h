/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <cstddef>  // for std::size_t
#include <functional>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_session_id.h"  // for StmtId
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"        // for InsertStatement and OplogLink
#include "mongo/db/repl/oplog_entry.h"  // for MutableOplogEntry
#include "mongo/s/shard_id.h"

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
class OplogWriter {
public:
    virtual ~OplogWriter() = default;

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
     * Log insert(s) to the local oplog. Returns the OpTime of every insert.
     * Refer to repl::logInsertOps() in repl/oplog.h.
     */
    virtual std::vector<repl::OpTime> logInsertOps(
        OperationContext* opCtx,
        repl::MutableOplogEntry* oplogEntryTemplate,
        std::vector<InsertStatement>::const_iterator begin,
        std::vector<InsertStatement>::const_iterator end,
        std::function<boost::optional<ShardId>(const BSONObj& doc)> getDestinedRecipientFn) = 0;

    /**
     * Returns the optime of the oplog entry written to the oplog.
     * Returns a null optime if oplog was not modified.
     */
    virtual repl::OpTime logOp(OperationContext* opCtx, repl::MutableOplogEntry* oplogEntry) = 0;

    /**
     * Allocates optimes for new entries in the oplog.  Returns a vector of OplogSlots, which
     * contain the new optimes along with their terms and newly calculated hash fields.
     */
    virtual std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) = 0;
};

}  // namespace mongo
