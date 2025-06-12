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

/**
 * This file contains tests for mongo/db/query/cqf_get_executor.h
 */

#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/index_catalog_mock.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_test_util.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using namespace optimizer;

/**
 * Tests specifically for CQF plan cache.
 */
class CqfPlanCacheTest : public CanonicalQueryTest {
public:
    CqfPlanCacheTest()
        : CanonicalQueryTest(),
          _uuid(UUID::gen()),
          _collectionPtr(),
          _queryHints(getHintsFromQueryKnobs()),
          _featureFlagController("featureFlagOptimizerPlanCache", true),
          _cardinalityEstimatorController("internalQueryCardinalityEstimatorMode", "sampling"),
          _parameterizationController("internalCascadesOptimizerEnableParameterization", true) {}

    /**
     * Optimize the CanonicalQuery and return the executor parameters.
     */
    boost::optional<ExecParams> optimize(const CanonicalQuery& query) {
        return getSBEExecutorViaCascadesOptimizer(MultipleCollectionAccessor{_collectionPtr},
                                                  _queryHints,
                                                  BonsaiEligibility::FullyEligible,
                                                  &query);
    }

    /**
     * Optimize the Pipeline and return the executor parameters.
     */
    boost::optional<ExecParams> optimize(Pipeline& query,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return getSBEExecutorViaCascadesOptimizer(opCtx(),
                                                  expCtx,
                                                  nss,
                                                  MultipleCollectionAccessor{_collectionPtr},
                                                  _queryHints,
                                                  boost::none,
                                                  BonsaiEligibility::FullyEligible,
                                                  &query);
    }

protected:
    void setUp() override {
        shard_role_details::getLocker(opCtx())->lockGlobal(opCtx(), LockMode::MODE_X);
        CollectionCatalog::write(opCtx(), [this](CollectionCatalog& catalog) {
            catalog.registerCollection(
                opCtx(),
                std::make_unique<CollectionMock>(_uuid, nss, std::make_unique<IndexCatalogMock>()),
                boost::none);
        });
        shard_role_details::getLocker(opCtx())->unlockGlobal();

        _collectionPtr =
            CollectionPtr{CollectionCatalog::get(opCtx())->lookupCollectionByUUID(opCtx(), _uuid)};
        _collectionPtr.makeYieldable(
            opCtx(), [](OperationContext* opCtx, boost::optional<UUID> uuid) -> const Collection* {
                ASSERT(uuid.has_value());
                return CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, *uuid);
            });
    }

    void tearDown() override {
        _collectionPtr.reset();

        shard_role_details::getLocker(opCtx())->lockGlobal(opCtx(), LockMode::MODE_X);
        CollectionCatalog::write(opCtx(), [this](CollectionCatalog& catalog) {
            catalog.deregisterCollection(opCtx(), _uuid, false, boost::none);
        });
        shard_role_details::getLocker(opCtx())->unlockGlobal();
    }

    /**
     * Build a plan cache key from a CanonicalQuery.
     */
    sbe::PlanCacheKey makePlanCacheKey(const CanonicalQuery& query) {
        return plan_cache_key_factory::make(query,
                                            MultipleCollectionAccessor{_collectionPtr},
                                            canonical_query_encoder::Optimizer::kBonsai);
    }

    /**
     * Build a plan cache key from a Pipeline.
     */
    sbe::PlanCacheKey makePlanCacheKey(const Pipeline& query) {
        return plan_cache_key_factory::make(query, MultipleCollectionAccessor{_collectionPtr});
    }

    PhaseManagerWithPlan makePhaseManager(const CanonicalQuery& query) {
        return getPhaseManager(opCtx(),
                               query.getExpCtx(),
                               query.nss(),
                               _collectionPtr,
                               stdx::unordered_set<NamespaceString>(),
                               _queryHints,
                               boost::none,
                               false,
                               internalCascadesOptimizerEnableParameterization.load(),
                               nullptr,
                               &query);
    }

    PhaseManagerWithPlan makePhaseManager(Pipeline& query) {
        return getPhaseManager(opCtx(),
                               query.getContext(),
                               query.getContext()->ns,
                               _collectionPtr,
                               stdx::unordered_set<NamespaceString>(),
                               _queryHints,
                               boost::none,
                               false,
                               internalCascadesOptimizerEnableParameterization.load(),
                               &query,
                               nullptr);
    }

    auto makePlan(OptPhaseManager& phaseManager,
                  PlanAndProps& planAndProps,
                  VariableEnvironment& variableEnv,
                  const CanonicalQuery& query) {
        return plan(phaseManager,
                    planAndProps,
                    opCtx(),
                    MultipleCollectionAccessor{_collectionPtr},
                    false,
                    nullptr,
                    boost::none,
                    makePlanCacheKey(query),
                    variableEnv);
    }

    auto makePlan(OptPhaseManager& phaseManager,
                  PlanAndProps& planAndProps,
                  VariableEnvironment& variableEnv,
                  const Pipeline& query) {
        return plan(phaseManager,
                    planAndProps,
                    opCtx(),
                    MultipleCollectionAccessor{_collectionPtr},
                    false,
                    nullptr,
                    boost::none,
                    makePlanCacheKey(query),
                    variableEnv);
    }

    auto getCollectionUUIDString() const {
        return _collectionPtr->uuid().toString();
    }

private:
    UUID _uuid;
    CollectionPtr _collectionPtr;
    optimizer::QueryHints _queryHints;

    RAIIServerParameterControllerForTest _featureFlagController;
    RAIIServerParameterControllerForTest _cardinalityEstimatorController;
    RAIIServerParameterControllerForTest _parameterizationController;
};

TEST_F(CqfPlanCacheTest, CqfPlanCacheCanonicalQueryInsertTest) {
    auto query = canonicalize("{jenny: 8675309}", "{}", "{}", "{}");
    const auto key = makePlanCacheKey(*query);

    auto& planCache = sbe::getPlanCache(opCtx());

    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_NE(optimize(*query), boost::none);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
}

TEST_F(CqfPlanCacheTest, CqfPlanCachePipelineInsertTest) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {jenny: 8675309}}")};
    const auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx(), nss);
    auto query = Pipeline::parse(rawPipeline, expCtx);
    // This test suite has parameterization on.
    query->parameterize();
    const auto key = makePlanCacheKey(*query);

    auto& planCache = sbe::getPlanCache(opCtx());

    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);

    ASSERT_NE(optimize(*query, expCtx), boost::none);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
}

TEST_F(CqfPlanCacheTest, CqfPlanCacheReadTest) {
    auto query = canonicalize("{jenny: 8675309}", "{}", "{}", "{}");
    auto [phaseManager, maybePlanAndProps, optCounterInfo, pipelineMatchExpr] =
        makePhaseManager(*query);
    const auto key = makePlanCacheKey(*query);
    auto& planCache = sbe::getPlanCache(opCtx());

    ASSERT_EQ(static_cast<bool>(maybePlanAndProps), true);
    auto& planAndProps = maybePlanAndProps.get();
    auto variableEnv = VariableEnvironment::build(planAndProps._node);


    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_NE(optimize(*query), boost::none);
    const auto fromCache = makePlan(phaseManager, planAndProps, variableEnv, *query).fromCache;
    ASSERT_EQ(fromCache, true);
}
TEST_F(CqfPlanCacheTest, CqfPlanCachePipelineReadTest) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {jenny: 8675309}}")};
    const auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx(), nss);
    auto query = Pipeline::parse(rawPipeline, expCtx);
    // This test suite has parameterization on.
    query->parameterize();
    const auto key = makePlanCacheKey(*query);

    auto& planCache = sbe::getPlanCache(opCtx());

    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_NE(optimize(*query, expCtx), boost::none);

    auto [phaseManager, maybePlanAndProps, optCounterInfo, pipelineMatchExpr] =
        makePhaseManager(*query);
    ASSERT_EQ(static_cast<bool>(maybePlanAndProps), true);
    auto& planAndProps = maybePlanAndProps.get();
    auto variableEnv = VariableEnvironment::build(planAndProps._node);

    const auto fromCache = makePlan(phaseManager, planAndProps, variableEnv, *query).fromCache;
    ASSERT_EQ(fromCache, true);
}

}  // namespace
}  // namespace mongo
