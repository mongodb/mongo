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
#include <cstddef>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/repl/oplog_entry.h"  // for ReplOperation
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Container for ReplOperation used in multi-doc transactions and batched writer context.
 * Includes statistics on operations held in this container.
 * Provides methods for exporting ReplOperations in one or more applyOps oplog entries.
 * Concurrency control for this class is maintained by the TransactionParticipant.
 */
class TransactionOperations {
public:
    using TransactionOperation = repl::ReplOperation;
    using CollectionUUIDs = stdx::unordered_set<UUID, UUID::Hash>;

    TransactionOperations() = default;

    /**
     * Returns true if '_transactionsOperations' is empty.
     */
    bool isEmpty() const;

    /**
     * Returns number of items in '_transactionOperations'.
     */
    std::size_t numOperations() const;

    /**
     * Total size in bytes of all operations within the _transactionOperations vector.
     * See DurableOplogEntry::getDurableReplOperationSize().
     */
    std::size_t getTotalOperationBytes() const;

    /**
     * Returns number of operations that have pre-images or post-images to be written to
     * noop oplog entries or the image collection.
     */
    std::size_t getNumberOfPrePostImagesToWrite() const;

    /**
     * Clears the operations stored in this container along with corresponding statistics.
     */
    void clear();

    /**
     * Adds an operation to this container and updates relevant statistics.
     *
     * Ensures that statement ids in operation do not conflict with the operations
     * already added.
     *
     * Ensures that total size of collected operations after adding operation does not
     * exceed 'transactionSizeLimitBytes' (if provided).
     */
    Status addOperation(const TransactionOperation& operation,
                        boost::optional<std::size_t> transactionSizeLimitBytes = boost::none);

    /**
     * Returns a set of collection UUIDs for the operations stored in this container.
     *
     * This allows the caller to check which collections will be modified as a resulting of
     * executing this transaction. The set of UUIDs returned by this function does not include
     * collection UUIDs for no-op operations, e.g. {op: 'n', ...}.
     */
    CollectionUUIDs getCollectionUUIDs() const;

    /**
     * Returns pointer to vector of operations for integrating with
     * TransactionParticipant and OpObserver interfaces for multi-doc transactions.
     *
     * Caller assumes responsibility for keeping contents referenced by the pointer
     * in sync with statistics maintained in this container.
     */
    std::vector<TransactionOperation>* getMutableOperationsForTransactionParticipant();

    /**
     * Returns copy of operations for TransactionParticipant testing.
     */
    std::vector<TransactionOperation> getOperationsForTest() const;

private:
    std::vector<TransactionOperation> _transactionOperations;

    // Holds stmtIds for operations which have been applied in the current multi-document
    // transaction.
    stdx::unordered_set<StmtId> _transactionStmtIds;

    // Size of operations in _transactionOperations as calculated by
    // DurableOplogEntry::getDurableReplOperationSize().
    std::size_t _totalOperationBytes{0};

    // Number of operations that have pre-images or post-images to be written to noop oplog
    // entries or the image collection.
    std::size_t _numberOfPrePostImagesToWrite{0};
};

}  // namespace mongo
