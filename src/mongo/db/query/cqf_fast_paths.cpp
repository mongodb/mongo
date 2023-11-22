/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/cqf_fast_paths.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/cqf_fast_paths_utils.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::fast_path {
namespace {

/**
 * Holds all information required to construct SBE plans for queries that have a fast path
 * implementation.
 */
struct ExecTreeGeneratorParams {
    const UUID collectionUuid;
    PlanYieldPolicy* yieldPolicy;
    const BSONObj& filter;
};

using ExecTreeResult = std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>;

/**
 * Interface for implementing a fast path for a simple query of certain shape. Responsible for SBE
 * plan generation.
 */
class ExecTreeGenerator {
public:
    virtual ~ExecTreeGenerator() = default;

    virtual ExecTreeResult generateExecTree(const ExecTreeGeneratorParams& params) const = 0;

    virtual BSONObj generateExplain() const = 0;
};

/**
 * ABTPrinter implementation that passes on the given explain BSON. Used for implementing
 * explain for fast paths.
 */
class FastPathPrinter : public AbstractABTPrinter {
public:
    FastPathPrinter(BSONObj fastPathExplain) : _explainBSON(std::move(fastPathExplain)) {}

    BSONObj explainBSON() const override final {
        return _explainBSON;
    }

    std::string getPlanSummary() const override final {
        return "COLLSCAN";
    }

private:
    const BSONObj _explainBSON;
};

// We can use a BSON object representing the filter for fast and simple comparison.
using FastPathMap = FilterComparator::Map<std::unique_ptr<ExecTreeGenerator>>;

FastPathMap fastPathMap{FilterComparator::kInstance.makeLessThan()};

/**
 * Do not call this method directly. Instead, use the REGISTER_FAST_PATH macro defined in this
 * file.
 */
void registerExecTreeGenerator(BSONObj shape,
                               std::unique_ptr<ExecTreeGenerator> execTreeGenerator) {
    tassert(8321506,
            "Did not expect 'shape' to contain '_id' field or a dotted path",
            !containsSpecialField(shape));
    fastPathMap.insert({shape, std::move(execTreeGenerator)});
}

bool canUseFastPath(const bool hasIndexHint,
                    const MultipleCollectionAccessor& collections,
                    const CanonicalQuery* canonicalQuery,
                    const Pipeline* pipeline) {
    if (internalCascadesOptimizerDisableFastPath.load()) {
        return false;
    }
    if (internalQueryDefaultDOP.load() > 1) {
        // The current fast path implementations don't support parallel scan plans.
        return false;
    }
    if (hasIndexHint) {
        // The current fast path implementations only deal with collection scans.
        return false;
    }
    if (canonicalQuery) {
        const auto& findRequest = canonicalQuery->getFindCommandRequest();
        if (canonicalQuery->getProj() || canonicalQuery->getSortPattern() ||
            findRequest.getLimit() || findRequest.getSkip()) {
            // The current fast path implementations don't support
            // projections or sorting.
            return false;
        }
    }
    if (pipeline) {
        const auto& sources = pipeline->getSources();
        if (sources.size() > 1 ||
            (sources.size() == 1 &&
             dynamic_cast<DocumentSourceMatch*>(sources.front().get()) == nullptr)) {
            // The current fast path implementations only support queries containing a single filter
            // with a simple predicate.
            return false;
        }
    }
    if (!collections.getMainCollection()) {
        // TODO SERVER-83267: Enable once we have a fast path for non-existent collections.
        return false;
    }
    const bool isSharded = collections.isAcquisition()
        ? collections.getMainAcquisition().getShardingDescription().isSharded()
        : collections.getMainCollection().isSharded_DEPRECATED();
    if (isSharded) {
        // The current fast path implementations don't support shard filtering.
        return false;
    }
    return true;
}

BSONObj extractQueryFilter(const CanonicalQuery* canonicalQuery, const Pipeline* pipeline) {
    if (canonicalQuery) {
        return canonicalQuery->getQueryObj();
    }
    if (pipeline) {
        return pipeline->getInitialQuery();
    }
    tasserted(8217100, "Expected canonicalQuery or pipeline.");
}

const ExecTreeGenerator* getFastPathExecTreeGenerator(const BSONObj& filter) {
    auto generatorIt = fastPathMap.find(filter);
    if (generatorIt == fastPathMap.end()) {
        OPTIMIZER_DEBUG_LOG(
            8321501, 5, "Query not eligible for a fast path.", "query"_attr = filter.toString());
        return {};
    }

    OPTIMIZER_DEBUG_LOG(
        8321502, 5, "Using a fast path for query", "query"_attr = filter.toString());

    return generatorIt->second.get();
}

/**
 * Implements fast path SBE plan generation for an empty query.
 */
class EmptyQueryExecTreeGenerator : public ExecTreeGenerator {
public:
    ExecTreeResult generateExecTree(const ExecTreeGeneratorParams& params) const override {
        // TODO SERVER-80582
        MONGO_UNIMPLEMENTED_TASSERT(8321504);
    }

    virtual BSONObj generateExplain() const override {
        // TODO SERVER-80582
        MONGO_UNIMPLEMENTED_TASSERT(8321505);
    }
};

REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(Empty, {}, std::make_unique<EmptyQueryExecTreeGenerator>());

}  // namespace

boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    const bool hasExplain,
    const bool hasIndexHint,
    const Pipeline* pipeline,
    const CanonicalQuery* canonicalQuery) {
    if (!canUseFastPath(hasIndexHint, collections, canonicalQuery, pipeline)) {
        return {};
    }

    const auto filter = extractQueryFilter(canonicalQuery, pipeline);

    auto generator = getFastPathExecTreeGenerator(filter);
    if (!generator) {
        return {};
    }

    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, collections, nss);

    ExecTreeGeneratorParams params{
        collections.getMainCollection()->uuid(), sbeYieldPolicy.get(), filter};

    auto [sbePlan, data] = generator->generateExecTree(params);

    {
        sbe::DebugPrinter p;
        OPTIMIZER_DEBUG_LOG(6264802, 5, "Lowered SBE plan", "plan"_attr = p.print(*sbePlan.get()));
    }

    sbePlan->attachToOperationContext(opCtx);
    if (expCtx->mayDbProfile) {
        sbePlan->markShouldCollectTimingInfo();
    }

    auto explain = hasExplain ? generator->generateExplain() : BSONObj{};

    sbePlan->prepare(data.env.ctx);
    CurOp::get(opCtx)->stopQueryPlanningTimer();

    return {{opCtx,
             nullptr /*solution*/,
             {std::move(sbePlan), std::move(data)},
             std::make_unique<FastPathPrinter>(std::move(explain)),
             QueryPlannerParams::Options::DEFAULT,
             nss,
             std::move(sbeYieldPolicy),
             false /*isFromPlanCache*/,
             true /* generatedByBonsai */,
             nullptr /*pipelineMatchExpr*/}};
}

boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    const MultipleCollectionAccessor& collections, const CanonicalQuery* query) {
    auto hasIndexHint = !query->getFindCommandRequest().getHint().isEmpty();

    return tryGetSBEExecutorViaFastPath(query->getOpCtx(),
                                        query->getExpCtx(),
                                        query->nss(),
                                        collections,
                                        query->getExplain(),
                                        hasIndexHint,
                                        nullptr /*pipeline*/,
                                        query);
}

}  // namespace mongo::optimizer::fast_path
