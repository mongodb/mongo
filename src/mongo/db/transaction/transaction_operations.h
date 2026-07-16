// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

extern FailPoint hangAfterLoggingApplyOpsForTransaction;

/**
 * Container for ReplOperation used in multi-doc transactions and batched writer context.
 * Includes statistics on operations held in this container.
 * Provides methods for exporting ReplOperations in one or more applyOps oplog entries.
 * Concurrency control for this class is maintained by the TransactionParticipant.
 */
class [[MONGO_MOD_PUBLIC]] TransactionOperations {
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
     * BSON serialized operations, and the relative position in the oplog. The operations in field
     * 'ApplyOpsEntry::operations' should be considered opaque outside the OpObserver.
     */
    struct ApplyOpsInfo {
        // Conservative BSON array element overhead assuming maximum 6 digit array index.
        static constexpr std::size_t kBSONArrayElementOverhead = 8U;

        struct ApplyOpsEntry {
            std::vector<BSONObj> operations;
            size_t oplogSlotIndex;
        };

        ApplyOpsInfo(std::vector<ApplyOpsEntry> applyOpsEntries,
                     std::size_t numberOfOplogSlotsRequired,
                     std::size_t numOperationsWithNeedsRetryImage,
                     bool prepare)
            : applyOpsEntries(std::move(applyOpsEntries)),
              numberOfOplogSlotsRequired(numberOfOplogSlotsRequired),
              numOperationsWithNeedsRetryImage(numOperationsWithNeedsRetryImage),
              prepare(prepare) {}

        explicit ApplyOpsInfo(bool prepare)
            : applyOpsEntries(),
              numberOfOplogSlotsRequired(0),
              numOperationsWithNeedsRetryImage(0),
              prepare(prepare) {}

        // Representation of "applyOps" oplog entries.
        std::vector<ApplyOpsEntry> applyOpsEntries;

        // Number of oplog slots required for these oplog entries including pre/post image slots.
        std::size_t numberOfOplogSlotsRequired;

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
     * Returns the number of collected operations that carry statement ids.
     */
    std::size_t getNumberOfOperationsWithStatementIds() const;

    /**
     * Clears the operations stored in this container along with corresponding statistics.
     */
    void clear();

    /**
     * Reorders the stored operations so all operations for the same record are contiguous, which
     * getApplyOpsInfo(..., respectAtomicGroups) requires to fit a record's operations in one
     * applyOps entry. A record is its group record id if set, else its own record id; records keep
     * first-seen order and operations keep their staged order within a record.
     */
    void groupByRecordId();

    /**
     * Adds an operation to this container and updates relevant statistics.
     *
     * Ensures that statement ids in operation do not conflict with the operations
     * already added.
     *
     * Ensures that total size of collected operations after adding operation does not
     * exceed 'transactionSizeLimitBytes' (if provided).
     */
    Status addOperation(TransactionOperation operation,
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
     * Returns the number of oplog slots to be used for "applyOps" oplog entries, BSON serialized
     * operations, their assignments to "applyOps" entries, and the number of oplog slots to be used
     * for writing pre- and post- image oplog entries for the transaction consisting of
     * 'operations'. The 'prepare' indicates if the function is called when preparing a transaction.
     *
     * When 'respectAtomicGroups' is true, a group's operations are never split across "applyOps"
     * entries: a group that would straddle a boundary is packed whole into the next entry, and a
     * group too large for one entry throws TransactionTooLarge. The operations must already be
     * grouped (see groupByRecordId). Used for kGroupForPossiblyRetryableOperations, whose entries
     * apply independently on secondaries.
     */
    ApplyOpsInfo getApplyOpsInfo(std::size_t oplogEntryCountLimit,
                                 std::size_t oplogEntrySizeLimitBytes,
                                 bool prepare,
                                 bool respectAtomicGroups = false) const;

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
     * This function expects that the size of 'oplogSlots' be the exact size needed to
     * assign slots to the '_transactionOperations' vector (including any pre-/post- image no-ops),
     * which size is returned in ApplyOpsInfo by getApplyOpsInfo above.
     *
     * The 'applyOpsOperationAssignment' contains BSON serialized transaction statements, their
     * assignment to "applyOps" oplog entries for a transaction.
     *
     * The 'oplogGroupingFormat' indicates whether these applyOps make up a multi-document
     * transaction (kDontGroup), a potentially multi-oplog-entry transactional batched wrote
     * (kGroupForTransaction), or a multi-oplog-entry potentially retryable write
     * (kGroupForPossiblyRetryableOperations)
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

    /**
     * Returns number of operations.
     */
    size_t getOperationsCount() const;

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
