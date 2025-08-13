/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/classic/working_set_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CanonicalQuery;
class Collection;
class CollectionPtr;
class OperationContext;
class BSONObj;
class Document;

namespace write_stage_common {

class PreWriteFilter {
public:
    /**
     * This class represents the different kind of actions we can take when handling a write
     * operation:
     *   - kWrite: perform the current write operation.
     *   - kWriteAsFromMigrate: perform the current write operation but marking it with the
     *     fromMigrate flag.
     *   - kSkip: skip the current write operation.
     */
    enum class Action { kWrite, kWriteAsFromMigrate, kSkip };

    PreWriteFilter(OperationContext* opCtx, NamespaceString nss);

    void saveState() {}

    void restoreState();

    /**
     * Returns which PreWriteFilterAction we should take for the current write operation over doc.
     */
    Action computeAction(const Document& doc);

    /**
     * Computes the required action for the current write operation over the 'doc' and logs cases
     * of 'kSkip' or 'kWriteAsFromMigrate'.
     *
     * - Returns 'kWRite' if the 'doc' is writeable
     * - Returns 'kSkip' if the 'doc' is not writeable and should be skipped.
     * - Returns 'kWriteAsFromMigrate' meaning that the 'doc' should be written to orphan chunk.
     */
    Action computeActionAndLogSpecialCases(const Document& doc,
                                           StringData opKind,
                                           const NamespaceString& collNs) {
        const auto action = computeAction(doc);
        if (action == Action::kSkip) {
            logSkippingDocument(doc, opKind, collNs);
        } else if (action == Action::kWriteAsFromMigrate) {
            logFromMigrate(doc, opKind, collNs);
        }

        return action;
    }

    /**
     * Checks if the 'doc' is NOT writable and additionally handles the StaleConfig error. This
     * method should be called in a context of single update / delete.
     *
     * Returns a pair of [optional immediate StageState return code, bool fromMigrate].
     * - Returns {{}, false} if the 'doc' is simply writable.
     * - Returns PlanStage::StageState if the 'doc' is not writable and the caller should return
     *   immediately with the state.
     * - Returns bool for 'fromMigrate' flag meaning that the 'doc' should be written to orphan
     *   chunk.
     */
    template <typename F>
    std::pair<boost::optional<PlanStage::StageState>, bool> checkIfNotWritable(
        const Document& doc, StringData opKind, const NamespaceString& collNs, F&& yieldHandler) {
        try {
            auto action = computeActionAndLogSpecialCases(doc, opKind, collNs);
            // If the 'doc' should be skipped in a context of single update / delete, the caller
            // should return immediately with NEED_TIME state. When action is 'kSkip', 'fromMigrate'
            // is a 'don't care' condition but we just fill it with false.
            if (action == Action::kSkip) {
                return {PlanStage::NEED_TIME, false};
            }
            return {{}, action == Action::kWriteAsFromMigrate};
        } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
            // If the placement version is IGNORED and we encountered a critical section, then
            // yield, wait for the critical section to finish and then we'll resume the write from
            // the point we had left. We do this to prevent large multi-writes from repeatedly
            // failing due to StaleConfig and exhausting the mongos retry attempts.
            if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
                ex->getCriticalSectionSignal()) {
                yieldHandler(ex);
                return {PlanStage::NEED_YIELD, false};
            }
            throw;
        }
    }

private:
    /**
     * Returns true if the operation is not versioned or if the doc is owned by the shard.
     *
     * May throw a ShardKeyNotFound if the document has an invalid shard key.
     */
    bool _documentBelongsToMe(const BSONObj& doc);

    static void logSkippingDocument(const Document& doc,
                                    StringData opKind,
                                    const NamespaceString& collNs);
    static void logFromMigrate(const Document& doc,
                               StringData opKind,
                               const NamespaceString& collNs);

    OperationContext* _opCtx;
    NamespaceString _nss;
    const bool _skipFiltering;
    std::unique_ptr<ShardFilterer> _shardFilterer;
};

/**
 * Returns true if the document referred to by 'id' still exists and matches the query predicate
 * given by 'cq'. Returns true if the document still exists and 'cq' is null. Returns false
 * otherwise, in which case the WorkingSetMember referred to by 'id' will no longer contain a valid
 * document, and the only operation that should be performed on the WSM is to free it.
 *
 * May throw a WriteConflictException if there was a conflict while searching to see if the document
 * still exists.
 */
bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq);

/**
 * Returns true if we are running retryable write or retryable internal multi-document transaction.
 */
bool isRetryableWrite(OperationContext* opCtx);

}  // namespace write_stage_common
}  // namespace mongo
