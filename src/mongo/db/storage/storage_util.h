// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/util/modules.h"

#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo::storage_helpers {
/**
 * Inserts the batch of documents 'docs' using the provided callable object 'insertFn'.
 *
 * 'insertFnType' type should be Callable and have the following call signature:
 *     Status insertFn(OperationContext* opCtx,
 *                     std::vector<InsertStatement>::const_iterator begin,
 *                     std::vector<InsertStatement>::const_iterator end);
 *
 *     where 'begin' (inclusive) and 'end' (exclusive) are the iterators for the range of documents
 * 'docs'.
 *
 * The function first attempts to insert documents as one batch. If the insertion fails, then it
 * falls back to inserting documents one at a time. The insertion is retried in case of write
 * conflicts.
 */
template <typename insertFnType>
Status insertBatchAndHandleRetry(OperationContext* opCtx,
                                 const NamespaceStringOrUUID& nsOrUUID,
                                 const std::vector<InsertStatement>& docs,
                                 insertFnType&& insertFn) {
    if (docs.size() > 1U) {
        try {
            if (insertFn(opCtx, docs.cbegin(), docs.cend()).isOK()) {
                return Status::OK();
            }
        } catch (...) {
            // Ignore this failure and behave as-if we never tried to do the combined batch insert.
            // The loop below will handle reporting any non-transient errors.
        }
    }

    // Try to insert the batch one-at-a-time because the batch failed all-at-once inserting.
    for (auto it = docs.cbegin(); it != docs.cend(); ++it) {
        auto status = writeConflictRetry(opCtx, "batchInsertDocuments", nsOrUUID, [&] {
            auto status = insertFn(opCtx, it, it + 1);
            if (!status.isOK()) {
                return status;
            }

            return Status::OK();
        });

        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}
}  // namespace mongo::storage_helpers
