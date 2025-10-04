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

#include "mongo/db/transaction/transaction_operations.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

/**
 * Returns operations that can fit into an "applyOps" entry. The returned operations are
 * serialized to BSON. The operations are given by range ['operationsBegin', 'operationsEnd').
 * - Multi-document transactions follow the following constraints for fitting the operations:
 *    (1) the resulting "applyOps" entry shouldn't exceed the 16MB limit, unless only one operation
 *          is allocated to it;
 *    (2) the number of operations is not larger than the maximum number of transaction statements
 *          allowed in one entry as defined by
 *          'gMaxNumberOfTransactionOperationsInSingleOplogEntry'.
 * - Batched writes (WUOWs that pack writes into a single applyOps outside of a multi-doc
 *    transaction) are exempt from the constraints above, but instead are subject to one:
 *    (1) If the operations cannot be packed into a single applyOps that's within the BSON size
 *         limit (16MB), the batched write will fail with TransactionTooLarge.
 */
std::vector<BSONObj> packOperationsIntoApplyOps(
    std::vector<repl::ReplOperation>::const_iterator operationsBegin,
    std::vector<repl::ReplOperation>::const_iterator operationsEnd,
    std::size_t oplogEntryCountLimit,
    std::size_t oplogEntrySizeLimitBytes) {
    std::vector<BSONObj> operations;
    std::size_t totalOperationsSize{0};
    for (auto operationIter = operationsBegin; operationIter != operationsEnd; ++operationIter) {
        const auto& operation = *operationIter;

        if (operations.size() == oplogEntryCountLimit) {
            break;
        }
        if ((operations.size() > 0 &&
             (totalOperationsSize +
                  repl::DurableOplogEntry::getDurableReplOperationSize(operation) >
              oplogEntrySizeLimitBytes))) {
            break;
        }

        auto serializedOperation = operation.toBSON();
        totalOperationsSize += static_cast<std::size_t>(serializedOperation.objsize());

        // Add BSON array element overhead since operations will ultimately be packed into BSON
        // array.
        totalOperationsSize += TransactionOperations::ApplyOpsInfo::kBSONArrayElementOverhead;

        operations.emplace_back(std::move(serializedOperation));
    }
    return operations;
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(hangAfterLoggingApplyOpsForTransaction);
MONGO_FAIL_POINT_DEFINE(skipLoggingLastOpForTransaction);

// static
void TransactionOperations::packTransactionStatementsForApplyOps(
    std::vector<TransactionOperation>::const_iterator stmtBegin,
    std::vector<TransactionOperation>::const_iterator stmtEnd,
    const std::vector<BSONObj>& operations,
    BSONObjBuilder* applyOpsBuilder,
    std::vector<StmtId>* stmtIdsWritten,
    boost::optional<repl::ReplOperation::ImageBundle>* imageToWrite) {
    tassert(6278508,
            "Number of operations does not match the number of transaction statements",
            operations.size() == static_cast<size_t>(stmtEnd - stmtBegin));

    auto operationsIter = operations.begin();
    BSONArrayBuilder opsArray(applyOpsBuilder->subarrayStart("applyOps"_sd));
    for (auto stmtIter = stmtBegin; stmtIter != stmtEnd; stmtIter++) {
        const auto& stmt = *stmtIter;
        opsArray.append(*operationsIter++);
        const auto stmtIds = stmt.getStatementIds();
        stmtIdsWritten->insert(stmtIdsWritten->end(), stmtIds.begin(), stmtIds.end());
        stmt.extractPrePostImageForTransaction(imageToWrite);
    }
    try {
        // If the BSONArrayBuilder exceeds the max BSON size, throw TransactionTooLarge.
        uassertStatusOK(opsArray.done().validateBSONObjSize().addContext(
            "Packing transaction statements failed."));
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>& e) {
        // Change BSONObjectTooLarge => TransactionTooLarge.
        uasserted(ErrorCodes::TransactionTooLarge, e.reason());
    }
}

bool TransactionOperations::isEmpty() const {
    return _transactionOperations.empty();
}

std::size_t TransactionOperations::numOperations() const {
    return _transactionOperations.size();
}

std::size_t TransactionOperations::getTotalOperationBytes() const {
    return _totalOperationBytes;
}

std::size_t TransactionOperations::getNumberOfPrePostImagesToWrite() const {
    return _numberOfPrePostImagesToWrite;
}

void TransactionOperations::clear() {
    _transactionOperations.clear();
    _transactionStmtIds.clear();
    _totalOperationBytes = 0;
    _numberOfPrePostImagesToWrite = 0;
}

Status TransactionOperations::addOperation(const TransactionOperation& operation,
                                           boost::optional<std::size_t> transactionSizeLimitBytes) {
    const auto& stmtIdsToInsert = operation.getStatementIds();
    auto nextStmtIdToInsert = stmtIdsToInsert.begin();

    // If the addOperation fails, we need to remove any statement IDs inserted.
    ScopeGuard stmtIdRemover([&] {
        for (auto iter = stmtIdsToInsert.begin(); iter != nextStmtIdToInsert; iter++)
            _transactionStmtIds.erase(*iter);
    });

    for (; nextStmtIdToInsert != stmtIdsToInsert.end(); ++nextStmtIdToInsert) {
        auto stmtId = *nextStmtIdToInsert;
        auto [_, inserted] = _transactionStmtIds.insert(stmtId);
        if (!inserted) {
            return Status(ErrorCodes::Error(5875600),
                          fmt::format("Found two operations using the same stmtId of {}", stmtId));
        }
    }

    auto opSize = repl::DurableOplogEntry::getDurableReplOperationSize(operation);

    // The pre-image size is always added to the collection transaction size, but
    // there are additional conditions for adding the pre-image to
    // '_numberOfPrePostImagesToWrite'. See SERVER-59694.
    std::size_t numberOfPrePostImagesToWrite = 0;
    if (const auto& preImage = operation.getPreImage(); !preImage.isEmpty()) {
        opSize += static_cast<std::size_t>(preImage.objsize());
        if (operation.isPreImageRecordedForRetryableInternalTransaction()) {
            numberOfPrePostImagesToWrite++;
        }
    }

    // The post-image, if present, is always included in the size and pre/post image counters.
    if (const auto& postImage = operation.getPostImage(); !postImage.isEmpty()) {
        opSize += operation.getPostImage().objsize();
        numberOfPrePostImagesToWrite++;
    }

    if (transactionSizeLimitBytes && (_totalOperationBytes + opSize) > *transactionSizeLimitBytes) {
        return Status(ErrorCodes::TransactionTooLarge,
                      fmt::format("Total size of all transaction operations must be less than "
                                  "server parameter 'transactionSizeLimitBytes' = {}",
                                  *transactionSizeLimitBytes));
    }

    _transactionOperations.push_back(operation);
    _totalOperationBytes += opSize;
    _numberOfPrePostImagesToWrite += numberOfPrePostImagesToWrite;
    stmtIdRemover.dismiss();

    return Status::OK();
}

TransactionOperations::CollectionUUIDs TransactionOperations::getCollectionUUIDs() const {
    CollectionUUIDs uuids;
    for (const auto& op : _transactionOperations) {
        if (op.getOpType() == repl::OpTypeEnum::kNoop) {
            // No-ops can't modify data, so there's no need to check if they involved a temporary
            // collection.
            continue;
        }

        // Ignore operations without collection UUIDs. No need for invariant.
        auto uuid = op.getUuid();
        if (!uuid) {
            continue;
        }

        uuids.insert(*uuid);
    }

    return uuids;
}

TransactionOperations::ApplyOpsInfo TransactionOperations::getApplyOpsInfo(
    std::size_t oplogEntryCountLimit, std::size_t oplogEntrySizeLimitBytes, bool prepare) const {
    const auto& operations = _transactionOperations;
    if (operations.empty()) {
        return {/*applyOpsEntries=*/{},
                /*numberOfOplogSlotsRequired=*/(prepare ? 1U : 0),
                /*numOperationsWithNeedsRetryImage=*/0,
                prepare};
    }

    std::vector<ApplyOpsInfo::ApplyOpsEntry> applyOpsEntries;
    std::size_t numOperationsWithNeedsRetryImage = 0;
    std::size_t numOplogSlotsRequired = 0;
    auto hasNeedsRetryImage = [](const repl::ReplOperation& operation) {
        return static_cast<bool>(operation.getNeedsRetryImage());
    };

    // Assign operations to "applyOps" entries.
    for (auto operationIt = operations.begin(); operationIt != operations.end();) {
        auto applyOpsOperations = packOperationsIntoApplyOps(
            operationIt, operations.end(), oplogEntryCountLimit, oplogEntrySizeLimitBytes);
        const auto opCountWithNeedsRetryImage =
            std::count_if(operationIt, operationIt + applyOpsOperations.size(), hasNeedsRetryImage);
        if (opCountWithNeedsRetryImage > 0) {
            // Reserve a slot for a forged no-op entry.
            numOplogSlotsRequired++;

            numOperationsWithNeedsRetryImage += opCountWithNeedsRetryImage;
        }
        operationIt += applyOpsOperations.size();
        applyOpsEntries.emplace_back(
            ApplyOpsInfo::ApplyOpsEntry{std::move(applyOpsOperations), numOplogSlotsRequired++});
    }

    return {std::move(applyOpsEntries),
            numOplogSlotsRequired,
            /*numOperationsWithNeedsRetryImage=*/numOperationsWithNeedsRetryImage,
            prepare};
}

std::size_t TransactionOperations::logOplogEntries(
    const std::vector<OplogSlot>& oplogSlots,
    const ApplyOpsInfo& applyOpsOperationAssignment,
    Date_t wallClockTime,
    WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
    LogApplyOpsFn logApplyOpsFn,
    boost::optional<TransactionOperation::ImageBundle>* prePostImageToWriteToImageCollection)
    const {
    invariant(oplogSlots.size() == applyOpsOperationAssignment.numberOfOplogSlotsRequired,
              "Wrong number of oplogSlots reserved");

    // Each entry in a chain of applyOps contains a 'prevOpTime' field that serves as a back
    // pointer to the previous entry.
    // The first entry will always have a null op time with term -1 and a timestamp containing
    // 0 seconds and 0 for the increment component.
    // For the atomic applyOps format used for batched writes, any value in this field will be
    // rejected during secondary oplog application in the form of an invariant, so it is important
    // for 'logApplyOpsFn' to remove this field before writing to the actual oplog collection.
    repl::OpTime prevWriteOpTime;

    // Stores the statement ids of all write statements in the transaction.
    std::vector<StmtId> stmtIdsWritten;

    // At the beginning of each loop iteration below, 'stmtsIter' will always point to the
    // first statement of the sequence of remaining, unpacked transaction statements. If all
    // statements have been packed, it should point to stmts.end(), which is the loop's
    // termination condition.
    auto stmtsIter = _transactionOperations.begin();
    auto applyOpsIter = applyOpsOperationAssignment.applyOpsEntries.begin();
    const bool prepare = applyOpsOperationAssignment.prepare;
    const bool applyOpsAppliedSeparately =
        oplogGroupingFormat == WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    while (stmtsIter != _transactionOperations.end()) {
        tassert(6278509,
                "Not enough \"applyOps\" entries",
                applyOpsIter != applyOpsOperationAssignment.applyOpsEntries.end());
        const auto& applyOpsEntry = *applyOpsIter++;
        BSONObjBuilder applyOpsBuilder;
        boost::optional<repl::ReplOperation::ImageBundle> imageToWrite;

        const auto nextStmt = stmtsIter + applyOpsEntry.operations.size();
        TransactionOperations::packTransactionStatementsForApplyOps(stmtsIter,
                                                                    nextStmt,
                                                                    applyOpsEntry.operations,
                                                                    &applyOpsBuilder,
                                                                    &stmtIdsWritten,
                                                                    &imageToWrite);

        // If we packed the last op, then the next oplog entry we log should be the implicit
        // commit or implicit prepare, i.e. we omit the 'partialTxn' field.
        auto firstOp = stmtsIter == _transactionOperations.begin();
        auto lastOp = nextStmt == _transactionOperations.end();

        // Skip logging the last oplog entry if this failpoint is enabled.
        if (MONGO_unlikely(skipLoggingLastOpForTransaction.shouldFail()) && lastOp) {
            // This will cause the loop to exit.
            stmtsIter = nextStmt;
            continue;
        }

        auto implicitPrepare = lastOp && prepare;
        auto isPartialTxn = !lastOp && !applyOpsAppliedSeparately;

        if (imageToWrite) {
            uassert(6054002,
                    str::stream() << NamespaceString::kConfigImagesNamespace.toStringForErrorMsg()
                                  << " can only store the pre or post image of one "
                                     "findAndModify operation for each "
                                     "transaction",
                    !(*prePostImageToWriteToImageCollection));
        }

        // A 'prepare' oplog entry should never include a 'partialTxn' field.
        invariant(!(isPartialTxn && implicitPrepare),
                  str::stream() << "isPartialTxn: " << isPartialTxn
                                << ", implicitPrepare: " << implicitPrepare);
        if (implicitPrepare) {
            applyOpsBuilder.append("prepare", true);
        }
        if (isPartialTxn) {
            applyOpsBuilder.append("partialTxn", true);
        }

        // The 'count' field gives the total number of individual operations in the
        // transaction, and is included on a non-initial implicit commit or prepare entry.
        // This field is used by the oplog application logic on secondary nodes to determine
        // how many operations to apply in the same batch without having to step through
        // all the oplog entries in an applyOps chain.
        // The 'count' field is redundant for single entry applyOps operations because
        // the number of operations can be derived from the length of the array in the
        // 'applyOps' field.
        // See SERVER-40676 and SERVER-40678.
        if (lastOp && !firstOp && !applyOpsAppliedSeparately) {
            applyOpsBuilder.append("count", static_cast<long long>(_transactionOperations.size()));
        }

        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
        oplogEntry.setOpTime(oplogSlots[applyOpsEntry.oplogSlotIndex]);
        oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTime);
        oplogEntry.setWallClockTime(wallClockTime);
        oplogEntry.setObject(applyOpsBuilder.done());
        oplogEntry.setTid(stmtsIter->getTid());

        prevWriteOpTime =
            logApplyOpsFn(&oplogEntry,
                          firstOp,
                          lastOp,
                          (lastOp || applyOpsAppliedSeparately ? std::move(stmtIdsWritten)
                                                               : std::vector<StmtId>{}),
                          oplogGroupingFormat);

        hangAfterLoggingApplyOpsForTransaction.pauseWhileSet();

        if (imageToWrite) {
            invariant(!(*prePostImageToWriteToImageCollection));
            imageToWrite->timestamp = prevWriteOpTime.getTimestamp();
            *prePostImageToWriteToImageCollection = *imageToWrite;
        }

        // Advance the iterator to the beginning of the remaining unpacked statements.
        stmtsIter = nextStmt;
    }

    return applyOpsOperationAssignment.numberOfOplogSlotsRequired;
}

const std::vector<TransactionOperations::TransactionOperation>&
TransactionOperations::getOperationsForOpObserver() const {
    return _transactionOperations;
}

std::vector<TransactionOperations::TransactionOperation>
TransactionOperations::getOperationsForTest() const {
    return _transactionOperations;
}

size_t TransactionOperations::getOperationsCount() const {
    return _transactionOperations.size();
}

}  // namespace mongo
