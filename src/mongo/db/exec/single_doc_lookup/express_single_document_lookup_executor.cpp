// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"

#include "mongo/db/exec/express/plan_executor_express.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_util.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Build the Express PlanExecutor for an _id-equality lookup. Returns nullptr if the documentKey
 * carries no _id, the caller should treat this as kNotHandled.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutor(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionAcquirer::Handle& coll,
    const Document& documentKey,
    bool shouldApplyShardFilter) {
    const auto idFilter = makeIdEqualityFilter(documentKey);
    if (!idFilter) {
        LOGV2_DEBUG(12841302,
                    1,
                    "ExpressSingleDocumentLookupExecutor: documentKey has no _id, falling back",
                    "documentKey"_attr = redact(documentKey.toBson()));
        return nullptr;
    }
    const BSONObj& filter = *idFilter;

    auto findCmd = std::make_unique<FindCommandRequest>(nss);
    findCmd->setFilter(filter);
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCmd).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCmd)},
    });

    // Apply the acquisition's shard filter so orphans physically present on this node are dropped
    // post-read, unless the caller's eligibility has already checked that the shard key belongs to
    // the local shard ('shouldApplyShardFilter' false). For callers running on bare _ids that may
    // include orphans (e.g. $search/$vectorSearch idLookup) it is what drops them. The filter is
    // only defined on sharded collections as getShardingFilter() tasserts otherwise.
    boost::optional<ScopedCollectionFilter> collectionFilter;
    if (shouldApplyShardFilter && coll.collection().getShardingDescription().isSharded()) {
        collectionFilter = coll.collection().getShardingFilter();
    }

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

}  // namespace

SingleDocumentLookupExecutor::LookupResult ExpressSingleDocumentLookupExecutor::performLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<Timestamp> afterClusterTime) {
    OperationContext* opCtx = expCtx->getOperationContext();

    // Timed around the whole eligibility run (not just the terminal attempt) so the recorded
    // latency reflects what the caller actually waited, including any StaleConfig-triggered
    // routing refresh + retry.
    Timer timer;

    // Express executor does not cache the acquisition and eligibility check is done before
    // acquiring the collection and therefore we always pass NoHeldAcquisition.
    LookupResult result = _localEligibility->run(
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
            return withCollectionGoneMappedToNotFound([&]() -> LookupResult {
                auto coll = _collectionAcquirer->acquireCollection(opCtx, nss, collectionUUID);
                if (!coll.exists()) {
                    return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
                }
                assertLocalLookupReadAtOrAfter(opCtx, afterClusterTime);

                auto exec = makeExpressExecutor(
                    opCtx, nss, coll, documentKey, !_eligibilityChecksShardKeyOwnership);
                if (!exec) {
                    return {LookupResult::HandledStatus::kNotHandled, boost::none};
                }

                Document out;
                const auto execState = exec->getNextDocument(out);

                PlanSummaryStats summary;
                exec->getPlanExplainer().getSummaryStats(&summary);
                recordIndexUsage(coll.getCollectionPtr().get(), summary, _planSummaryStatsSink);

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
            });
        });

    if (_recorder) {
        switch (result.status) {
            case LookupResult::HandledStatus::kDocumentFound:
                _recorder->recordFound(timer.elapsed());
                break;
            case LookupResult::HandledStatus::kDocumentNotFound:
                _recorder->recordNotFound(timer.elapsed());
                break;
            case LookupResult::HandledStatus::kNotHandled:
                _recorder->recordNotHandled();
                break;
        }
    }
    return result;
}

}  // namespace mongo::exec::agg
