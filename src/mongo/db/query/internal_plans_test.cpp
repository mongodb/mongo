// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/internal_plans.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.coll");

class InternalPlannerTest : public CatalogTestFixture {
public:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions{}));
    }

    void insertDocument(const BSONObj& doc) {
        ASSERT_OK(storageInterface()->insertDocument(
            operationContext(), kNss, {doc, Timestamp{}}, repl::OpTime::kUninitializedTerm));
    }

    MultipleCollectionAccessor acquireTestCollection() {
        return MultipleCollectionAccessor(acquireCollectionMaybeLockFree(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::kRead)));
    }
};

TEST_F(InternalPlannerTest, SbeCollectionScanUsesSbeEngine) {
    auto collections = acquireTestCollection();
    auto exec = InternalPlanner::collectionScanSbe(
        operationContext(), collections, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    ASSERT_EQ(exec->getQueryFramework(), PlanExecutor::QueryFramework::kSBEOnly);
}

TEST_F(InternalPlannerTest, SbeCollectionScanPreservesDocumentFields) {
    const auto expected =
        BSON("_id" << 1 << "nested" << BSON("value" << BSONNULL) << "array" << BSON_ARRAY(1 << 2));
    insertDocument(expected);
    auto collections = acquireTestCollection();
    auto exec = InternalPlanner::collectionScanSbe(
        operationContext(), collections, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    BSONObj doc;
    ASSERT_EQ(exec->getNext(&doc, nullptr), PlanExecutor::ADVANCED);
    ASSERT_BSONOBJ_EQ(doc, expected);
}

TEST_F(InternalPlannerTest, SbeCollectionScanReturnsEveryDocument) {
    insertDocument(BSON("_id" << 1));
    insertDocument(BSON("_id" << 2));
    auto collections = acquireTestCollection();
    auto exec = InternalPlanner::collectionScanSbe(
        operationContext(), collections, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    BSONObj doc;
    for (int id : {1, 2}) {
        ASSERT_EQ(exec->getNext(&doc, nullptr), PlanExecutor::ADVANCED);
        ASSERT_EQ(doc["_id"].Int(), id);
    }
    ASSERT_EQ(exec->getNext(&doc, nullptr), PlanExecutor::IS_EOF);
}

TEST_F(InternalPlannerTest, SbeCollectionScanReturnsEofForEmptyCollection) {
    auto collections = acquireTestCollection();
    auto exec = InternalPlanner::collectionScanSbe(
        operationContext(), collections, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    BSONObj doc;
    ASSERT_EQ(exec->getNext(&doc, nullptr), PlanExecutor::IS_EOF);
}

TEST_F(InternalPlannerTest, SbeCollectionScanCanBeInterrupted) {
    unittest::ServerParameterGuard yieldIterations{"internalQueryExecYieldIterations", 1};
    insertDocument(BSON("_id" << 1));
    insertDocument(BSON("_id" << 2));
    auto collections = acquireTestCollection();
    auto exec = InternalPlanner::collectionScanSbe(
        operationContext(), collections, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    BSONObj doc;
    ASSERT_EQ(exec->getNext(&doc, nullptr), PlanExecutor::ADVANCED);
    operationContext()->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS_CODE(exec->getNext(&doc, nullptr), DBException, ErrorCodes::Interrupted);
}

}  // namespace
}  // namespace mongo
