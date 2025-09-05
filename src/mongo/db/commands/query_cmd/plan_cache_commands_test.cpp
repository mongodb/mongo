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

#include "mongo/db/commands/query_cmd/plan_cache_commands.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <utility>

namespace mongo {
namespace {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("test.collection"_sd);

class PlanCacheCommandsTest : public ServiceContextTest {
public:
    void setUp() override {
        auto catalog = CollectionCatalog::get(_opCtx.get());
        std::shared_ptr<Collection> coll = std::make_shared<CollectionMock>(nss);
        catalog->onCreateCollection(_opCtx.get(), coll);
        auto collection = catalog->establishConsistentCollection(
            _opCtx.get(), nss, boost::none /* readTimestamp */);
        _collectionAcq = std::make_unique<CollectionAcquisition>(
            shard_role_mock::acquireCollectionMocked(_opCtx.get(), nss, collection));
    }

    void tearDown() override {
        _collectionAcq.reset();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    PlanCacheKey makeClassicKey(CanonicalQuery& cq) {
        return plan_cache_key_factory::make<PlanCacheKey>(cq, _collectionAcq->getCollectionPtr());
    }


private:
    ServiceContext::UniqueOperationContext _opCtx{makeOperationContext()};
    std::unique_ptr<CollectionAcquisition> _collectionAcq;
};

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWithMissingQueryField) {
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{}")).getStatus());
}

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWhenQueryFieldIsNotObject) {
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: 1}")).getStatus());
}

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWhenSortFieldIsNotObject) {
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {}, sort: 1}"))
                      .getStatus());
}

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWhenProjectionFieldIsNotObject) {
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {}, projection: 1}"))
            .getStatus());
}

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWhenCollationFieldIsNotObject) {
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {}, collation: 1}"))
            .getStatus());
}

TEST_F(PlanCacheCommandsTest, CannotCanonicalizeWhenSortObjectIsMalformed) {
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {}, sort: {a: 0}}"))
            .getStatus());
}

TEST_F(PlanCacheCommandsTest, CanCanonicalizeWithValidQuery) {
    PlanCache planCache(5000);

    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    // Equivalent query should generate same key.
    statusWithCQ =
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {b: 3, a: 4}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> equivQuery = std::move(statusWithCQ.getValue());
    ASSERT_EQUALS(makeClassicKey(*query), makeClassicKey(*equivQuery));
}

TEST_F(PlanCacheCommandsTest, SortQueryResultsInDifferentPlanCacheKeyFromUnsorted) {
    PlanCache planCache(5000);

    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    // Sort query should generate different key from unsorted query.
    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx(), nss, fromjson("{query: {a: 1, b: 1}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(makeClassicKey(*query), makeClassicKey(*sortQuery));
}

// Regression test for SERVER-17158.
TEST_F(PlanCacheCommandsTest, SortsAreProperlyDelimitedInPlanCacheKey) {
    PlanCache planCache(5000);

    auto statusWithCQ = plan_cache_commands::canonicalize(
        opCtx(), nss, fromjson("{query: {a: 1, b: 1}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery1 = std::move(statusWithCQ.getValue());

    // Confirm sort arguments are properly delimited (SERVER-17158)
    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx(), nss, fromjson("{query: {a: 1, b: 1}, sort: {aab: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery2 = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(makeClassicKey(*sortQuery1), makeClassicKey(*sortQuery2));
}

TEST_F(PlanCacheCommandsTest, ProjectQueryResultsInDifferentPlanCacheKeyFromUnprojected) {
    PlanCache planCache(5000);

    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx(), nss, fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx(), nss, fromjson("{query: {a: 1, b: 1}, projection: {_id: 0, a: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> projectionQuery = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(makeClassicKey(*query), makeClassicKey(*projectionQuery));
}

}  // namespace
}  // namespace mongo
