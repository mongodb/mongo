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

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/index_catalog_mock.h"
#include "mongo/db/query/canonical_query_test_util.h"
#include "mongo/db/query/cqf_get_executor.h"
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
          _featureFlagController("featureFlagOptimizerPlanCache", true) {}

    /**
     * Optimize the CanonicalQuery and return the executor parameters.
     */
    boost::optional<ExecParams> optimize(const CanonicalQuery& query) {
        return getSBEExecutorViaCascadesOptimizer(
            MultipleCollectionAccessor{_collectionPtr}, _queryHints, &query);
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
            opCtx(), [](OperationContext* opCtx, UUID uuid) -> const Collection* {
                return CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid);
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
    sbe::PlanCacheKey makeSbeKey(const CanonicalQuery& query) {
        return plan_cache_key_factory::make(
            query, MultipleCollectionAccessor{_collectionPtr}, false);
    }

private:
    UUID _uuid;
    CollectionPtr _collectionPtr;
    QueryHints _queryHints;
    RAIIServerParameterControllerForTest _featureFlagController;
};

TEST_F(CqfPlanCacheTest, CqfPlanCacheInsertTest) {
    auto query = canonicalize("{jenny: 8675309}", "{}", "{}", "{}");
    const auto key = makeSbeKey(*query);

    auto& planCache = sbe::getPlanCache(opCtx());

    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_NE(optimize(*query), boost::none);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
}

}  // namespace
}  // namespace mongo
