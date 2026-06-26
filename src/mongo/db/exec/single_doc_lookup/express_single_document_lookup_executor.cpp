/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"

#include "mongo/db/exec/express/plan_executor_express.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kIdField = "_id"sv;

/**
 * Build the Express PlanExecutor for an _id-equality lookup. Returns nullptr if the documentKey
 * carries no _id, the caller should treat this as kNotHandled.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutor(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionAcquirer::Handle& coll,
    const Document& documentKey) {
    const Value idVal = documentKey[kIdField];
    if (idVal.missing()) {
        LOGV2_DEBUG(12841302,
                    1,
                    "ExpressSingleDocumentLookupExecutor: documentKey has no _id, falling back",
                    "documentKey"_attr = redact(documentKey.toBson()));
        return nullptr;
    }

    BSONObjBuilder filterBuilder;
    idVal.addToBsonObj(&filterBuilder, kIdField);
    const BSONObj filter = filterBuilder.obj();

    auto findCmd = std::make_unique<FindCommandRequest>(nss);
    findCmd->setFilter(filter);
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCmd).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCmd)},
    });

    // No shard filter needed. Unlike a scatter-gathered find({_id}), the eligibility check already
    // shard-key-targeted this documentKey to this shard as the unique owner, and the versioned
    // acquisition confirms that ownership is current. With per-shard _id uniqueness, the doc found
    // by _id here is the owned one, not an orphan.
    boost::optional<ScopedCollectionFilter> collectionFilter;

    // The find command carries no collation, so adopt the collection's default collation.
    const auto& collPtr = coll.getCollectionPtr();
    if (cq->getCollator() == nullptr && collPtr && collPtr->getDefaultCollator()) {
        cq->setCollator(collPtr->getDefaultCollator()->clone());
    }

    const bool isClusteredOnId = collPtr && collPtr->isClustered() &&
        clustered_util::isClusteredOnId(collPtr->getClusteredInfo());
    return isClusteredOnId ? makeExpressExecutorForFindByClusteredId(opCtx,
                                                                     std::move(cq),
                                                                     coll.collection(),
                                                                     std::move(collectionFilter),
                                                                     /*returnOwnedBson=*/true)
                           : makeExpressExecutorForFindById(opCtx,
                                                            std::move(cq),
                                                            coll.collection(),
                                                            std::move(collectionFilter),
                                                            /*returnOwnedBson=*/true);
}

/**
 * Record index usage so $indexStats reflects the lookup..
 */
void recordIndexUsage(const CollectionAcquirer::Handle& coll, PlanExecutor& exec) {
    PlanSummaryStats summary;
    exec.getPlanExplainer().getSummaryStats(&summary);
    CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
        coll.getCollectionPtr().get(),
        summary.collectionScans,
        summary.collectionScansNonTailable,
        summary.indexesUsed);
}
}  // namespace

SingleDocumentLookupExecutor::LookupResult ExpressSingleDocumentLookupExecutor::performLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<Timestamp> afterClusterTime) {
    OperationContext* opCtx = expCtx->getOperationContext();

    // Express executor does not cache the acquisition and eligibility check is done before
    // acquiring the collection and therefore we always pass NoHeldAcquisition.
    return _localEligibility->run(
        expCtx,
        nss,
        documentKey,
        LocalLookupEligibility::NoHeldAcquisition{},
        [&](const LocalLookupEligibility::Decision& decision) -> LookupResult {
            // The document does not live on local shard. Early exit.
            if (!LocalLookupEligibility::isLocal(decision)) {
                return {LookupResult::HandledStatus::kNotHandled, boost::none};
            }

            // Create ScopedSetShardRole given the local routing decision.
            const auto& local = std::get<LocalLookupEligibility::Local>(decision);
            auto shardRoleScope = createScopedShardRole(opCtx, nss, local);
            try {
                auto coll = _collectionAcquirer->acquireCollection(opCtx, nss, collectionUUID);
                if (!coll.exists()) {
                    return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
                }
                assertLocalLookupReadAtOrAfter(opCtx, afterClusterTime);

                auto exec = makeExpressExecutor(opCtx, nss, coll, documentKey);
                if (!exec) {
                    return {LookupResult::HandledStatus::kNotHandled, boost::none};
                }

                Document out;
                const auto execState = exec->getNextDocument(out);
                recordIndexUsage(coll, *exec);

                switch (execState) {
                    case PlanExecutor::ExecState::ADVANCED:
                        return {LookupResult::HandledStatus::kDocumentFound, out.getOwned()};
                    case PlanExecutor::ExecState::IS_EOF:
                        return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
                    default:
                        uasserted(12841303,
                                  str::stream()
                                      << "Unexpected executor state in "
                                         "ExpressSingleDocumentLookupExecutor::performLookup");
                }
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                LOGV2_DEBUG(12841307,
                            1,
                            "Namespace not found while looking up document via Express path",
                            "error"_attr = redact(ex));
                return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
            } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
                LOGV2_DEBUG(12841308,
                            1,
                            "Collection UUID mismatch while looking up document via Express path",
                            "error"_attr = redact(ex));
                return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
            }
        });
}

}  // namespace mongo::exec::agg
