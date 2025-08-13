/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

#include <vector>

namespace mongo {
namespace storage_helpers {

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
}  // namespace storage_helpers

}  // namespace mongo
