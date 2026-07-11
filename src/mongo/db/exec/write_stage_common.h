// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
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
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

class CanonicalQuery;
class Collection;
class CollectionPtr;
class OperationContext;
class BSONObj;
class Document;

namespace write_stage_common {

class [[MONGO_MOD_PUBLIC]] PreWriteFilter {
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
                                           std::string_view opKind,
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
        const Document& doc,
        std::string_view opKind,
        const NamespaceString& collNs,
        F&& yieldHandler) {
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
                                    std::string_view opKind,
                                    const NamespaceString& collNs);
    static void logFromMigrate(const Document& doc,
                               std::string_view opKind,
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
 *
 * If the fetch is successful, increments docsFetchedCounter by 1.
 */
bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq,
                        size_t& docsFetchedCounter);

/**
 * Returns true if we are running retryable write or retryable internal multi-document transaction.
 */
bool isRetryableWrite(OperationContext* opCtx);

}  // namespace write_stage_common
}  // namespace mongo
