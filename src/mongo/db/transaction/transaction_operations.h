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

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/oplog.h"        // for OplogSlot
#include "mongo/db/repl/oplog_entry.h"  // for ReplOperation
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/transaction/integer_interval_set.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

extern FailPoint hangAfterLoggingApplyOpsForTransaction;

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

    /**
     * Function type used by logOplogEntries() to write a formatted applyOps oplog entry
     * to the oplog.
     *
     * The 'oplogEntry' holds the current applyOps oplog entry formatted by logOplogEntries()
     * and is passed in as a pointer because downstream functions in the oplog generation code
     * may append additional information.
     *
     * The booleans 'firstOp' and 'lastOp' indicate where this entry is within the chain of
     * generated applyOps oplog entries. One use for these booleans is to determine if we have
     * a singleton oplog chain (firstOp == lastOp).
     *
     * The 'stmtIdsWritten' holds the complete list of statement ids extracted from the entire
     * chain of applyOps oplog entries. It will be empty for each entry in the chain except for
     * the last entry ('lastOp' == true). It may also be empty if there are no statement ids
     * contained in any of the replicated operations.
     *
     * The 'oplogGroupingFormat' indicates whether these applyOps make up a multi-document
     * transaction (kDontGroup), a potentially multi-oplog-entry transactional batched wrote
     * (kGroupForTransaction), or a multi-oplog-entry potentially retryable write
     * (kGroupForPossiblyRetryableOperations)
     *
     * This is based on the signature of the logApplyOps() function within the OpObserverImpl
     * implementation, which takes a few more arguments that can be derived from the caller's
     * context.
     */
    using LogApplyOpsFn =
        std::function<repl::OpTime(repl::MutableOplogEntry* oplogEntry,
                                   bool firstOp,
                                   bool lastOp,
                                   std::vector<StmtId> stmtIdsWritten,
                                   WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat)>;

    /**
     * Contains "applyOps" oplog entries for a transaction. "applyOps" entries are not actual
     * "applyOps" entries to be written to the oplog, but comprise certain parts of those entries -
     * BSON serialized operations, and the assigned oplog slot. The operations in field
     * 'ApplyOpsEntry::operations' should be considered opaque outside the OpObserver.
     */
    struct ApplyOpsInfo {
        // Conservative BSON array element overhead assuming maximum 6 digit array index.
        static constexpr std::size_t kBSONArrayElementOverhead = 8U;

        struct ApplyOpsEntry {
            OplogSlot oplogSlot;
            std::vector<BSONObj> operations;
        };

        ApplyOpsInfo(std::vector<ApplyOpsEntry> applyOpsEntries,
                     std::size_t numberOfOplogSlotsUsed,
                     std::size_t numOperationsWithNeedsRetryImage,
                     bool prepare)
            : applyOpsEntries(std::move(applyOpsEntries)),
              numberOfOplogSlotsUsed(numberOfOplogSlotsUsed),
              numOperationsWithNeedsRetryImage(numOperationsWithNeedsRetryImage),
              prepare(prepare) {}

        explicit ApplyOpsInfo(bool prepare)
            : applyOpsEntries(),
              numberOfOplogSlotsUsed(0),
              numOperationsWithNeedsRetryImage(0),
              prepare(prepare) {}

        // Representation of "applyOps" oplog entries.
        std::vector<ApplyOpsEntry> applyOpsEntries;

        // Number of oplog slots utilized.
        std::size_t numberOfOplogSlotsUsed;

        // Number of operations with 'needsRetryImage' set.
        std::size_t numOperationsWithNeedsRetryImage;

        // Indicates if we are generating "applyOps" oplog entries for a prepared transaction.
        // This is derived from the 'prepared' parameter passed to the getApplyOpsInfo() function.
        bool prepare;
    };

    /**
     * Accepts an empty BSON builder and appends the given transaction statements to an 'applyOps'
     * array field (and their corresponding statement ids to 'stmtIdsWritten'). The transaction
     * statements are represented as range ['stmtBegin', 'stmtEnd') and BSON serialized objects
     * 'operations'. If any of the statements has a pre-image or post-image that needs to be
     * stored in the image collection, stores it to 'imageToWrite'.
     *
     * Throws TransactionTooLarge if the size of the resulting oplog entry exceeds the BSON limit.
     * See BSONObjMaxUserSize (currently set to 16 MB).
     *
     * Used to implement logOplogEntries().
     */
    static void packTransactionStatementsForApplyOps(
        std::vector<TransactionOperation>::const_iterator stmtBegin,
        std::vector<TransactionOperation>::const_iterator stmtEnd,
        const std::vector<BSONObj>& operations,
        BSONObjBuilder* applyOpsBuilder,
        std::vector<StmtId>* stmtIdsWritten,
        boost::optional<repl::ReplOperation::ImageBundle>* imageToWrite);

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
     * Returns oplog slots to be used for "applyOps" oplog entries, BSON serialized operations,
     * their assignments to "applyOps" entries, and oplog slots to be used for writing pre- and
     * post- image oplog entries for the transaction consisting of 'operations'. Allocates oplog
     * slots from 'oplogSlots'. The 'prepare' indicates if the function is called when preparing a
     * transaction.
     */
    ApplyOpsInfo getApplyOpsInfo(const std::vector<OplogSlot>& oplogSlots,
                                 std::size_t oplogEntryCountLimit,
                                 std::size_t oplogEntrySizeLimitBytes,
                                 bool prepare) const;

    /**
     * Logs applyOps oplog entries for preparing a transaction, committing an unprepared
     * transaction, or committing a WUOW that is not necessarily related to a multi-document
     * transaction. This includes the in-progress 'partialTxn' oplog entries followed by the
     * implicit prepare or commit entry. If the 'prepare' argument is true, it will log entries
     * for a prepared transaction. Otherwise, it logs entries for an unprepared transaction.
     * The total number of oplog entries written will be <= the number of the operations in the
     * '_transactionOperations' vector, and will depend on how many transaction statements
     * are given, the data size of each statement, and the 'oplogEntryCountLimit' parameter
     * given to getApplyOpsInfo().
     *
     * This function expects that the size of 'oplogSlots' be at least as big as the size of
     * '_transactionOperations' in the worst case, where each operation requires an applyOps
     * entry of its own. If there are more oplog slots than applyOps operations are written, the
     * number of oplog slots corresponding to the number of applyOps written will be used.
     * It also expects that the vector of given statements is non-empty.
     *
     * The 'applyOpsOperationAssignment' contains BSON serialized transaction statements, their
     * assignment to "applyOps" oplog entries for a transaction.
     *
     * The 'oplogGroupingFormat' indicates whether these applyOps make up a multi-document
     * transaction (kDontGroup), a potentially multi-oplog-entry transactional batched wrote
     * (kGroupForTransaction), or a multi-oplog-entry potentially retryable write
     * (kGroupForPossiblyRetryableOperations)
     *
     *
     * In the case of writing entries for a prepared transaction, the last oplog entry
     * (i.e. the implicit prepare) will always be written using the last oplog slot given,
     * even if this means skipping over some reserved slots.
     *
     * The number of oplog entries written is returned.
     *
     * Throws TransactionTooLarge if the size of any resulting applyOps oplog entry exceeds the
     * BSON limit.
     * See packTransactionStatementsForApplyOps() and BSONObjMaxUserSize (currently set to 16 MB).
     */
    std::size_t logOplogEntries(const std::vector<OplogSlot>& oplogSlots,
                                const ApplyOpsInfo& applyOpsOperationAssignment,
                                Date_t wallClockTime,
                                WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                                LogApplyOpsFn logApplyOpsFn,
                                boost::optional<TransactionOperation::ImageBundle>*
                                    prePostImageToWriteToImageCollection) const;

    /**
     * Returns const reference to vector of operations for integrating with
     * BatchedWriteContext, TransactionParticipant, and OpObserver interfaces
     * for multi-doc transactions.
     *
     * This function can be removed when we have migrated callers of BatchedWriteContext
     * and TransactionParticipant to use the methods on this class directly.
     */
    const std::vector<TransactionOperation>& getOperationsForOpObserver() const;

    /**
     * Returns copy of operations for TransactionParticipant testing.
     */
    std::vector<TransactionOperation> getOperationsForTest() const;

private:
    std::vector<TransactionOperation> _transactionOperations;

    // Holds stmtIds for operations which have been applied in the current multi-document
    // transaction.
    IntegerIntervalSet<StmtId> _transactionStmtIds;

    // Size of operations in _transactionOperations as calculated by
    // DurableOplogEntry::getDurableReplOperationSize().
    std::size_t _totalOperationBytes{0};

    // Number of operations that have pre-images or post-images to be written to noop oplog
    // entries or the image collection.
    std::size_t _numberOfPrePostImagesToWrite{0};
};

}  // namespace mongo
