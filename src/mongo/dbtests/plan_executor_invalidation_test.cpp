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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::unique_ptr;

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.PlanExecutorInvalidationTest");

/**
 * Test fixture for verifying that plan executors correctly raise errors when invalidating events
 * such as collection or index drops happen during yield.
 */
class PlanExecutorInvalidationTest : public unittest::Test {
public:
    PlanExecutorInvalidationTest()
        : _expCtx(ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss).build()) {
        DBDirectClient client(&_opCtx);
        client.dropCollection(nss);

        for (int i = 0; i < N(); ++i) {
            client.insert(nss, BSON("foo" << i));
        }

        _coll = acquireColl(&_opCtx, MODE_IX);
    }

    /**
     * Return a plan executor that is going over the collection in nss.ns_forTest().
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollscan() {
        unique_ptr<WorkingSet> ws(new WorkingSet());
        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;
        unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection(), params, ws.get(), nullptr));

        // Create a plan executor to hold it
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        auto cq = std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = _expCtx.get(),
                                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

        // Takes ownership of 'ws', 'scan', and 'cq'.
        auto statusWithPlanExecutor =
            plan_executor_factory::make(std::move(cq),
                                        std::move(ws),
                                        std::move(scan),
                                        collection(),
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT);

        ASSERT_OK(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeIxscanPlan(BSONObj keyPattern,
                                                                        BSONObj startKey,
                                                                        BSONObj endKey) {
        auto indexDescriptor =
            collection().getCollectionPtr()->getIndexCatalog()->findIndexByKeyPatternAndOptions(
                &_opCtx, keyPattern, _makeMinimalIndexSpec(keyPattern));
        ASSERT(indexDescriptor);
        return InternalPlanner::indexScan(&_opCtx,
                                          collection(),
                                          indexDescriptor,
                                          startKey,
                                          endKey,
                                          BoundInclusion::kIncludeBothStartAndEndKeys,
                                          PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    }

    int N() {
        return 50;
    }

    bool dropDatabase(const std::string& dbname) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        bool res =
            client.dropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, dbname));
        return res;
    }

    bool dropCollection(StringData ns) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        bool res = client.dropCollection(NamespaceString::createNamespaceString_forTest(ns));
        return res;
    }

    void dropIndexes(const NamespaceString& nss) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        client.dropIndexes(nss);
    }

    void dropIndex(const NamespaceString& nss, BSONObj keys) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        client.dropIndex(nss, keys);
    }

    void renameCollection(const std::string& to) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        BSONObj info;

        ASSERT_TRUE(client.runCommand(
            DatabaseName::kAdmin,
            BSON("renameCollection" << nss.ns_forTest() << "to" << to << "dropTarget" << true),
            info));
    }

    Status createIndex(StringData ns, const BSONObj& keys, bool unique = false) {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        auto coll = acquireColl(opCtx2.get(), MODE_X);
        Status res = dbtests::createIndex(opCtx2.get(), ns, keys, unique);
        return res;
    }

    CollectionAcquisition acquireColl(OperationContext* opCtx, LockMode mode) {
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            mode);
    }
    const CollectionAcquisition& collection() const {
        invariant(_coll);
        return *_coll;
    }

    void truncateCollection() {
        // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
        // query.
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        auto coll = acquireColl(opCtx2.get(), MODE_X);
        WriteUnitOfWork wunit(opCtx2.get());
        CollectionWriter writer{opCtx2.get(), &coll};
        auto collection = writer.getWritableCollection(opCtx2.get());
        ASSERT_OK(collection->truncate(opCtx2.get()));
        wunit.commit();
    }

    // Order of these is important for initialization
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;

    boost::optional<CollectionAcquisition> _coll;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

private:
    BSONObj _makeMinimalIndexSpec(BSONObj keyPattern) {
        return BSON(IndexDescriptor::kKeyPatternFieldName
                    << keyPattern << IndexDescriptor::kIndexVersionFieldName
                    << IndexDescriptor::getDefaultIndexVersion());
    }
};

TEST_F(PlanExecutorInvalidationTest, ExecutorToleratesDeletedDocumentsDuringYield) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);

    // Delete some data, namely the next 2 things we'd expect.
    // Do it from a different opCtx to avoid polluting the yielded transaction resources for the
    // query.
    {
        auto newClient = _opCtx.getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtx2 = cc().makeOperationContext();

        DBDirectClient client(opCtx2.get());
        client.remove(nss, BSON("foo" << 10));
        client.remove(nss, BSON("foo" << 11));
    }

    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    exec->restoreState(nullptr);

    // Make sure that the PlanExecutor moved forward over the deleted data.  We don't see foo==10 or
    // foo==11.
    for (int i = 12; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

TEST_F(PlanExecutorInvalidationTest, PlanExecutorThrowsOnRestoreWhenCollectionIsDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);

    // Drop a collection that's not ours.
    dropCollection("unittests.someboguscollection");

    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    exec->restoreState(nullptr);

    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_EQUALS(10, obj["foo"].numberInt());

    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);

    dropCollection(nss.ns_forTest());

    // Try to restore
    ASSERT_THROWS_CODE(
        restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2)),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, CollScanExecutorDoesNotDieWhenAllIndicesDropped) {

    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), BSON("foo" << 1)));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));

    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    dropIndexes(nss);
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));
    exec->restoreState(nullptr);

    // Read the rest of the collection.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, CollScanExecutorDoesNotDieWhenOneIndexDropped) {
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), BSON("foo" << 1)));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));

    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    dropIndex(nss, BSON("foo" << 1));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));
    exec->restoreState(nullptr);

    // Read the rest of the collection.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorDiesWhenAllIndexesDropped) {
    BSONObj keyPattern = BSON("foo" << 1);

    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPattern));
    // Create a second index which is not used by the plan executor.
    ASSERT_OK(createIndex(nss.ns_forTest(), BSON("bar" << 1)));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop the index which the plan executor is scanning while the executor is in a saved state.
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    dropIndexes(nss);

    // Restore the ShardRole Transaction resources
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));

    // Restoring the executor should throw.
    ASSERT_THROWS_CODE(exec->restoreState(nullptr), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorDiesWhenIndexBeingScannedIsDropped) {
    BSONObj keyPattern = BSON("foo" << 1);
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPattern));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop all indexes while the executor is saved.
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    dropIndex(nss, keyPattern);

    // Restore the ShardRole Transaction resources
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));

    // Restoring the executor should throw.
    ASSERT_THROWS_CODE(exec->restoreState(nullptr), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorSurvivesWhenUnrelatedIndexIsDropped) {
    BSONObj keyPatternFoo = BSON("foo" << 1);
    BSONObj keyPatternBar = BSON("bar" << 1);
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPatternFoo));
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPatternBar));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    auto exec = makeIxscanPlan(keyPatternFoo, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop an index which the plan executor is *not* scanning while the executor is in a saved
    // state.
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    dropIndex(nss, keyPatternBar);
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));
    exec->restoreState(nullptr);

    // Scan the rest of the index.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, ExecutorThrowsOnRestoreWhenDatabaseIsDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);

    // Drop a DB that's not ours (yield and restore resources in between, this will release and
    // re-acquire the locks)
    dropDatabase("somesillydb");

    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    exec->restoreState(nullptr);

    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_EQUALS(10, obj["foo"].numberInt());

    // Drop our DB (yield and restore resources in between, this will release and re-acquire the
    // locks)
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);

    dropDatabase("unittests");

    ASSERT_THROWS_CODE(
        restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2)),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

// TODO SERVER-31695: Allow PlanExecutors to remain valid after collection rename.
TEST_F(PlanExecutorInvalidationTest, CollScanDiesOnCollectionRenameWithinDatabase) {
    auto exec = getCollscan();

    // Partially scan the collection.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    // Rename the collection.
    exec->saveState();
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    renameCollection("unittests.new_collection_name");

    ASSERT_THROWS_CODE(
        restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources)),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

// TODO SERVER-31695: Allow PlanExecutors to remain valid after collection rename.
TEST_F(PlanExecutorInvalidationTest, IxscanDiesOnCollectionRenameWithinDatabase) {
    BSONObj keyPattern = BSON("foo" << 1);

    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPattern));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Partially scan the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Rename the collection.
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    renameCollection("unittests.new_collection_name");

    ASSERT_THROWS_CODE(
        restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2)),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorSurvivesCollectionTruncate) {
    BSONObj keyPattern = BSON("foo" << 1);
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    ASSERT_OK(createIndex(nss.ns_forTest(), keyPattern));
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Partially scan the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Call truncate() on the Collection during yield. The PlanExecutor should be restored
    // successfully.
    exec->saveState();
    auto yieldedResources2 = yieldTransactionResourcesFromOperationContext(&_opCtx);
    truncateCollection();
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources2));
    exec->restoreState(nullptr);

    // Since all documents in the collection have been deleted, the PlanExecutor should issue EOF.
    ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

TEST_F(PlanExecutorInvalidationTest, CollScanExecutorSurvivesCollectionTruncate) {
    auto exec = getCollscan();

    // Partially scan the collection.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    // Call truncate() on the Collection during yield. The PlanExecutor should be restored
    // successfully.
    exec->saveState();
    auto yieldedResources = yieldTransactionResourcesFromOperationContext(&_opCtx);
    truncateCollection();
    restoreTransactionResourcesToOperationContext(&_opCtx, std::move(yieldedResources));
    exec->restoreState(nullptr);

    // Since all documents in the collection have been deleted, the PlanExecutor should issue EOF.
    ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

}  // namespace mongo
