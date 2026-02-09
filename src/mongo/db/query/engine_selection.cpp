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

#include "mongo/db/query/engine_selection.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/query_utils.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
/**
 * Returns true iff 'descriptor' has fields A and B where all of the following hold
 *
 *   - A is a path prefix of B
 *   - A is a hashed field in the index
 *   - B is a non-hashed field in the index
 *
 * TODO SERVER-99889 this is a workaround for an SBE stage builder bug.
 */
bool indexHasHashedPathPrefixOfNonHashedPath(const IndexDescriptor* descriptor) {
    boost::optional<StringData> hashedPath;
    for (const auto& elt : descriptor->keyPattern()) {
        if (elt.valueStringDataSafe() == "hashed") {
            // Indexes may only contain one hashed field.
            hashedPath = elt.fieldNameStringData();
            break;
        }
    }
    if (hashedPath == boost::none) {
        // No hashed fields in the index.
        return false;
    }
    // Check if 'hashedPath' is a path prefix for any field in the index.
    for (const auto& elt : descriptor->keyPattern()) {
        if (expression::isPathPrefixOf(hashedPath.get(), elt.fieldNameStringData())) {
            return true;
        }
    }
    return false;
}

/**
 * Returns true if 'collection' has an index that contains two fields, one of which is a path prefix
 * of the other, where the prefix field is hashed. Indexes can only contain one hashed field.
 *
 * TODO SERVER-99889: At the time of writing, there is a bug in the SBE stage builders that
 * constructs ExpressionFieldPaths over hashed values. This leads to wrong query results.
 *
 * The bug arises for covered index scans where a path P is a non-hashed path in the index and a
 * strict prefix P' of P is a hashed path in the index.
 */
bool collectionHasIndexWithHashedPathPrefixOfNonHashedPath(const CollectionPtr& collection,
                                                           ExpressionContext* expCtx) {
    const IndexCatalog* indexCatalog = collection->getIndexCatalog();
    tassert(10230200, "'CollectionPtr' does not have an 'IndexCatalog'", indexCatalog);
    OperationContext* opCtx = expCtx->getOperationContext();
    tassert(10230201, "'ExpressionContext' does not have an 'OperationContext'", opCtx);
    std::unique_ptr<IndexCatalog::IndexIterator> indexIter =
        indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (indexIter->more()) {
        const IndexCatalogEntry* entry = indexIter->next();
        if (indexHasHashedPathPrefixOfNonHashedPath(entry->descriptor())) {
            return true;
        }
    }
    return false;
}

/**
 * Checks if the given query can be executed with the SBE engine based on the canonical query.
 *
 * This method determines whether the query may be compatible with SBE based only on high-level
 * information from the canonical query, before query planning has taken place (such as ineligible
 * expressions or collections).
 *
 * If this method returns true, query planning should be done, followed by another layer of
 * validation to make sure the query plan can be executed with SBE. If it returns false, SBE query
 * planning can be short-circuited as it is already known that the query is ineligible for SBE.
 */
bool isQuerySbeCompatible(const CollectionPtr& collection, const CanonicalQuery& cq) {
    auto expCtx = cq.getExpCtxRaw();

    // If we don't support all expressions used or the query is eligible for IDHack, don't use SBE.
    if (!expCtx || expCtx->getSbeCompatibility() == SbeCompatibility::notCompatible ||
        expCtx->getSbePipelineCompatibility() == SbeCompatibility::notCompatible ||
        (collection && isIdHackEligibleQuery(collection, cq))) {
        return false;
    }

    const auto* proj = cq.getProj();
    if (proj && (proj->requiresMatchDetails() || proj->containsElemMatch())) {
        return false;
    }

    // Tailable and resumed scans are not supported either.
    if (expCtx->isTailable() || cq.getFindCommandRequest().getRequestResumeToken()) {
        return false;
    }

    const auto& nss = cq.nss();

    const auto isTimeseriesColl = collection && collection->isTimeseriesCollection();

    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if ((!feature_flags::gFeatureFlagTimeSeriesInSbe.isEnabled() ||
         queryKnob.getSbeDisableTimeSeriesForOp()) &&
        isTimeseriesColl) {
        return false;
    }

    // Queries against the oplog are not supported. Also queries on the inner side of a $lookup are
    // not considered for SBE except search queries.
    if ((expCtx->getInLookup() && !cq.isSearchQuery()) || nss.isOplog() ||
        !cq.metadataDeps().none()) {
        return false;
    }


    // Queries against collections with a particular shape of compound hashed indexes are not
    // supported.
    if (collection && collectionHasIndexWithHashedPathPrefixOfNonHashedPath(collection, expCtx)) {
        return false;
    }

    // Find and aggregate queries with the $_startAt parameter are not supported in SBE.
    if (!cq.getFindCommandRequest().getStartAt().isEmpty()) {
        return false;
    }

    const auto& sortPattern = cq.getSortPattern();
    return !sortPattern || isSortSbeCompatible(*sortPattern);
}


bool hasNodeOfType(const QuerySolutionNode* node, StageType type) {
    if (node->getType() == type) {
        return true;
    }
    for (auto&& child : node->children) {
        if (hasNodeOfType(child.get(), type)) {
            return true;
        }
    }
    return false;
}

bool isPlanSbeEligible(const QuerySolution* solution) {
    // Distinct scan plans not supported in SBE.
    return !hasNodeOfType(solution->root(), StageType::STAGE_DISTINCT_SCAN);
}

EngineChoice engineSelectionForPlan(const QuerySolution* solution) {
    // TODO SERVER-117448 implement engine selection based on winning plan.
    return EngineChoice::kClassic;
}

/**
 * Function which returns true if 'cq' uses features that are currently supported in SBE without
 * 'featureFlagSbeFull' being set; false otherwise.
 */
EngineChoice shouldUseRegularSbe(OperationContext* opCtx,
                                 const CanonicalQuery& cq,
                                 const CollectionPtr& mainCollection,
                                 const bool sbeFull,
                                 const QuerySolution* solution) {
    // When featureFlagSbeFull is not enabled, we cannot use SBE unless 'trySbeEngine' is enabled or
    // if 'trySbeRestricted' is enabled, and we have eligible pushed down stages in the cq pipeline.
    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if (!queryKnob.canPushDownFullyCompatibleStages() && cq.cqPipeline().empty()) {
        return EngineChoice::kClassic;
    }

    if (mainCollection && mainCollection->isTimeseriesCollection() && cq.cqPipeline().empty()) {
        // TS queries only use SBE when there's a pipeline.
        return EngineChoice::kClassic;
    }

    // Return true if all the expressions in the CanonicalQuery's filter and projection are SBE
    // compatible.
    SbeCompatibility minRequiredCompatibility =
        getMinRequiredSbeCompatibility(queryKnob.getInternalQueryFrameworkControlForOp(), sbeFull);
    if (cq.getExpCtx()->getSbeCompatibility() >= minRequiredCompatibility) {
        return EngineChoice::kSbe;
    }

    // If we're given a QuerySolution, evaluate the plan to see if qualifies for SBE enablement.
    if (solution && engineSelectionForPlan(solution) == EngineChoice::kSbe) {
        return EngineChoice::kSbe;
    }

    return EngineChoice::kClassic;
}
}  // namespace

EngineChoice chooseEngine(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          CanonicalQuery* cq,
                          Pipeline* pipeline,
                          bool needsMerge,
                          std::unique_ptr<QueryPlannerParams> plannerParams,
                          const QuerySolution* solution) {
    if (solution) {
        tassert(11742301,
                "Expected to choose engine based on solution only if "
                "featureFlagGetExecutorDeferredEngineChoice is "
                "enabled.",
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled());
        if (!isPlanSbeEligible(solution)) {
            return EngineChoice::kClassic;
        }
    }
    const auto& mainColl = collections.getMainCollection();
    const bool forceClassic =
        cq->getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled();
    if (forceClassic || !isQuerySbeCompatible(mainColl, *cq)) {
        return EngineChoice::kClassic;
    }

    // Add the stages that are candidates for SBE lowering from the 'pipeline' into the
    // 'canonicalQuery'. This must be done _before_ checking shouldUseRegularSbe() or
    // creating the planner.
    attachPipelineStages(collections, pipeline, needsMerge, cq, std::move(plannerParams));

    const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabled();
    if (sbeFull ||
        shouldUseRegularSbe(opCtx, *cq, mainColl, sbeFull, solution) == EngineChoice::kSbe) {
        return EngineChoice::kSbe;
    }
    return EngineChoice::kClassic;
}

}  // namespace mongo
