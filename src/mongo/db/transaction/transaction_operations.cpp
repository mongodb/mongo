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

#include <fmt/format.h>

namespace mongo {

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
    auto stmtIdsToInsert = operation.getStatementIds();
    auto newTransactionStmtIds = _transactionStmtIds;
    for (auto stmtId : stmtIdsToInsert) {
        auto [_, inserted] = newTransactionStmtIds.insert(stmtId);
        if (inserted) {
            continue;
        }
        return Status(static_cast<ErrorCodes::Error>(5875600),
                      fmt::format("Found two operations using the same stmtId of {}", stmtId));
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
    _transactionStmtIds = std::move(newTransactionStmtIds);
    _totalOperationBytes += opSize;
    _numberOfPrePostImagesToWrite += numberOfPrePostImagesToWrite;

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

std::vector<TransactionOperations::TransactionOperation>*
TransactionOperations::getMutableOperationsForOpObserver() {
    return &_transactionOperations;
}

std::vector<TransactionOperations::TransactionOperation>
TransactionOperations::getOperationsForTest() const {
    return _transactionOperations;
}

}  // namespace mongo
