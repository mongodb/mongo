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

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace catalog {

/**
 * Indicates whether the data drop (the data table) should occur immediately or be two-phased, which
 * delays data removal to support older PIT reads or rollback.
 */
enum class DataRemoval {
    kImmediate,
    kTwoPhase,
};

/**
 * Performs two-phase index drop.
 *
 * Passthrough to DurableCatalog::removeIndex to execute the first phase of drop by removing the
 * index catalog entry, then registers an onCommit hook to schedule the second phase of drop to
 * delete the index data. The 'dataRemoval' field can be used to specify whether the second phase of
 * drop, table data deletion, should run immediately or delayed: immediate deletion should only be
 * used for incomplete indexes, where the index build is the only accessor and the data will not be
 * needed for earlier points in time.
 *
 * Uses 'ident' shared_ptr to ensure that the second phase of drop (data table drop) will not
 * execute until no users of the index (shared owners) remain. 'ident' is allowed to be a nullptr,
 * in which case the caller guarantees that there are no remaining users of the index. This handles
 * situations wherein there is no in-memory state available for an index, such as during repair.
 */
void removeIndex(OperationContext* opCtx,
                 StringData indexName,
                 Collection* collection,
                 std::shared_ptr<Ident> ident,
                 DataRemoval dataRemoval = DataRemoval::kTwoPhase);

/**
 * Performs two-phase collection drop.
 *
 * Passthrough to DurableCatalog::dropCollection to execute the first phase of drop by removing the
 * collection entry, then registers and onCommit hook to schedule the second phase of drop to delete
 * the collection data.
 *
 * Uses 'ident' shared_ptr to ensure that the second phase of drop (data table drop) will not
 * execute until no users of the collection record store (shared owners) remain. 'ident' is not
 * allowed to be nullptr.
 */
Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      std::shared_ptr<Ident> ident);


}  // namespace catalog

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
        auto status = writeConflictRetry(opCtx, "batchInsertDocuments", nsOrUUID.toString(), [&] {
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
