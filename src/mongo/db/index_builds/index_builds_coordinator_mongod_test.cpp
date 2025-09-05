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

#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

using unittest::assertGet;

namespace {

class IndexBuildsCoordinatorMongodTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

public:
    /**
     * Creates a collection with a default CollectionsOptions and the given UUID.
     */
    void createCollection(const NamespaceString& nss, UUID uuid);

    CollectionAcquisition getCollectionExclusive(OperationContext* opCtx,
                                                 const NamespaceString& nss);

    std::vector<IndexBuildInfo> makeSpecs(std::vector<std::string> keys, std::vector<int32_t> ids);

    const UUID _testFooUUID = UUID::gen();
    const NamespaceString _testFooNss = NamespaceString::createNamespaceString_forTest("test.foo");
    const UUID _testBarUUID = UUID::gen();
    const NamespaceString _testBarNss = NamespaceString::createNamespaceString_forTest("test.bar");
    const UUID _othertestFooUUID = UUID::gen();
    const NamespaceString _othertestFooNss =
        NamespaceString::createNamespaceString_forTest("othertest.foo");
    const TenantId _tenantId{OID::gen()};
    const NamespaceString _testTenantFooNss =
        NamespaceString::createNamespaceString_forTest(_tenantId, "test.test");
    const UUID _testFooTenantUUID = UUID::gen();
    const IndexBuildsCoordinator::IndexBuildOptions _indexBuildOptions = {
        .commitQuorum = CommitQuorumOptions(CommitQuorumOptions::kDisabled)};
    std::unique_ptr<IndexBuildsCoordinator> _indexBuildsCoord;
};

void IndexBuildsCoordinatorMongodTest::setUp() {
    CatalogTestFixture::setUp();
    // Create config.system.indexBuilds collection to store commit quorum value during index
    // building.
    createCollection(NamespaceString::kIndexBuildEntryNamespace, UUID::gen());

    createCollection(_testFooNss, _testFooUUID);
    createCollection(_testBarNss, _testBarUUID);
    createCollection(_othertestFooNss, _othertestFooUUID);
    createCollection(_testTenantFooNss, _testFooTenantUUID);

    _indexBuildsCoord = std::make_unique<IndexBuildsCoordinatorMongod>();
}

void IndexBuildsCoordinatorMongodTest::tearDown() {
    // Resume index builds left running by test failures so that shutdown() will not block.
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

    _indexBuildsCoord->shutdown(operationContext());
    _indexBuildsCoord.reset();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsCoordinatorMongodTest::createCollection(const NamespaceString& nss, UUID uuid) {
    CollectionOptions options;
    options.uuid = uuid;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    // Insert document into collection to avoid optimization for index creation on an empty
    // collection. This allows us to pause index builds on the collection using the test function
    // IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly().
    ASSERT_OK(storageInterface()->insertDocument(operationContext(),
                                                 nss,
                                                 {BSON("_id" << 0), Timestamp()},
                                                 repl::OpTime::kUninitializedTerm));
}

CollectionAcquisition IndexBuildsCoordinatorMongodTest::getCollectionExclusive(
    OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
}

std::vector<IndexBuildInfo> IndexBuildsCoordinatorMongodTest::makeSpecs(
    std::vector<std::string> keys, std::vector<int32_t> ids) {
    invariant(keys.size());
    invariant(keys.size() == ids.size());
    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& keyName = keys[i];
        IndexBuildInfo indexBuildInfo(
            BSON("v" << 2 << "key" << BSON(keyName << 1) << "name" << (keyName + "_1")),
            fmt::format("index-{}", ids[i]));
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexes.push_back(std::move(indexBuildInfo));
    }
    return indexes;
}

TEST_F(IndexBuildsCoordinatorMongodTest, AttemptBuildSameIndexFails) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a", "b"}, {1, 2}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    // Attempt and fail to register an index build on _testFooNss with the same index name, while
    // the prior build is still running.
    ASSERT_EQ(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                 _testFooNss.dbName(),
                                                 _testFooUUID,
                                                 makeSpecs({"b"}, {3}),
                                                 UUID::gen(),
                                                 IndexBuildProtocol::kTwoPhase,
                                                 _indexBuildOptions),
              ErrorCodes::IndexBuildAlreadyInProgress);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);
    auto indexCatalogStats = unittest::assertGet(testFoo1Future.getNoThrow());
    ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
    ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
}

TEST_F(IndexBuildsCoordinatorMongodTest, RestartIndexBuild_HandlesPendingIndexBuild) {
    repl::ReplSettings settings;
    settings.setReplSetString("mySet/node1:12345");
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(),
                                                                        std::move(settings));
    ASSERT_OK(replCoord->setFollowerModeRollback(operationContext()));
    replCoord->alwaysAllowWrites(true);
    repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a", "b"}, {1, 2}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));
    ASSERT_TRUE(
        _indexBuildsCoord->inProgForCollection(_testFooUUID, IndexBuildProtocol::kTwoPhase));

    auto failPoint = globalFailPointRegistry().find("hangBeforeCompletingAbort");
    failPoint->setMode(FailPoint::Mode::alwaysOn);

    auto thread = stdx::thread([&] {
        // We would not be able to restart the index build.
        auto indexBuilds = _indexBuildsCoord->restartAllTwoPhaseIndexBuilds(operationContext());
        ASSERT_EQ(1, indexBuilds.size());
    });

    // Wait until the index build is aborted.
    failPoint->waitForTimesEntered(1);
    failPoint->setMode(FailPoint::Mode::off);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

    thread.join();

    // Verify that the startIndexBuild() call above fails.
    ASSERT_THROWS(testFoo1Future.get(), ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>);

    _indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(
        operationContext(), _testFooUUID, IndexBuildProtocol::kTwoPhase);
    ASSERT_FALSE(
        _indexBuildsCoord->inProgForCollection(_testFooUUID, IndexBuildProtocol::kTwoPhase));
    auto collection = getCollectionExclusive(operationContext(), _testFooNss);
    ASSERT_EQ(
        3,
        _indexBuildsCoord->getNumIndexesTotal(operationContext(), collection.getCollectionPtr()));
}

TEST_F(IndexBuildsCoordinatorMongodTest, RestartIndexBuild_HandlesCommittedIndexBuild) {
    repl::ReplSettings settings;
    settings.setReplSetString("mySet/node1:12345");
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(),
                                                                        std::move(settings));
    ASSERT_OK(replCoord->setFollowerModeRollback(operationContext()));
    replCoord->alwaysAllowWrites(true);
    repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

    auto failPoint = globalFailPointRegistry().find("hangIndexBuildBeforeCommit");
    failPoint->setMode(FailPoint::Mode::alwaysOn);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a", "b"}, {1, 2}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));
    ASSERT_TRUE(
        _indexBuildsCoord->inProgForCollection(_testFooUUID, IndexBuildProtocol::kTwoPhase));
    // Wait until the index build is ready to commit.
    failPoint->waitForTimesEntered(1);

    auto thread = stdx::thread([&] {
        // We would not be able to restart the index build now as it's about to commit, but we
        // should gracefully handle it.
        auto indexBuilds = _indexBuildsCoord->restartAllTwoPhaseIndexBuilds(operationContext());
        ASSERT_TRUE(indexBuilds.empty());
    });

    failPoint->setMode(FailPoint::Mode::off);

    thread.join();

    // Verify that the startIndexBuild() call above succeeds.
    auto indexCatalogStats = unittest::assertGet(testFoo1Future.getNoThrow());
    ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
    ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
}

// Incrementally registering index builds and checking both that the registration was successful and
// that the access functions convey the expected state of the manager.
TEST_F(IndexBuildsCoordinatorMongodTest, Registration) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1BuildUUID = UUID::gen();
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a", "b"}, {1, 2}),
                                                     testFoo1BuildUUID,
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.dbName()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.dbName()));
    ASSERT_THROWS_WITH_CHECK(
        _indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testFooUUID),
        ExceptionFor<ErrorCodes::BackgroundOperationInProgressForNamespace>,
        [&](const auto& ex) { ASSERT_STRING_CONTAINS(ex.reason(), testFoo1BuildUUID.toString()); });
    ASSERT_THROWS_WITH_CHECK(
        _indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.dbName()),
        ExceptionFor<ErrorCodes::BackgroundOperationInProgressForDatabase>,
        [&](const auto& ex) { ASSERT_STRING_CONTAINS(ex.reason(), testFoo1BuildUUID.toString()); });

    // Register a second index build on _testFooNss.
    auto testFoo2Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"c", "d"}, {3, 4}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.dbName()), 2);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.dbName()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.dbName()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a different collection _testBarNss.
    auto testBarFuture = assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                                      _testBarNss.dbName(),
                                                                      _testBarUUID,
                                                                      makeSpecs({"x", "y"}, {5, 6}),
                                                                      UUID::gen(),
                                                                      IndexBuildProtocol::kTwoPhase,
                                                                      _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testBarNss.dbName()), 3);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testBarUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testBarNss.dbName()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testBarUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testBarNss.dbName()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a collection in a different database _othertestFoo.
    auto othertestFooFuture =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _othertestFooNss.dbName(),
                                                     _othertestFooUUID,
                                                     makeSpecs({"r", "s"}, {7, 8}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_othertestFooNss.dbName()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(_othertestFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_othertestFooNss.dbName()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_othertestFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_othertestFooNss.dbName()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

    auto indexCatalogStats = unittest::assertGet(testFoo1Future.getNoThrow());
    ASSERT_GTE(indexCatalogStats.numIndexesBefore, 1);
    ASSERT_GT(indexCatalogStats.numIndexesAfter, 1);
    ASSERT_LTE(indexCatalogStats.numIndexesAfter, 5);

    indexCatalogStats = unittest::assertGet(testFoo2Future.getNoThrow());
    ASSERT_GTE(indexCatalogStats.numIndexesBefore, 1);
    ASSERT_GT(indexCatalogStats.numIndexesAfter, 1);
    ASSERT_LTE(indexCatalogStats.numIndexesAfter, 5);

    indexCatalogStats = unittest::assertGet(testBarFuture.getNoThrow());
    ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
    ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);

    indexCatalogStats = unittest::assertGet(othertestFooFuture.getNoThrow());
    ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
    ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);

    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testFooUUID);
    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testBarUUID);
    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(_othertestFooUUID);

    _indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.dbName());
    _indexBuildsCoord->assertNoBgOpInProgForDb(_othertestFooNss.dbName());

    ASSERT_NOT_EQUALS(_testFooNss, _testBarNss);
    ASSERT_NOT_EQUALS(_testFooNss, _othertestFooNss);
}

TEST_F(IndexBuildsCoordinatorMongodTest, SetCommitQuorumWithBadArguments) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    CommitQuorumOptions newCommitQuorum("majority");

    // Pass in an empty index list.
    Status status =
        _indexBuildsCoord->setCommitQuorum(operationContext(), _testFooNss, {}, newCommitQuorum);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound, status);

    // Use an invalid collection namespace.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("bad.collection");
    status = _indexBuildsCoord->setCommitQuorum(
        operationContext(), nss, {"a_1", "b_1"}, newCommitQuorum);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);

    // No index builds are happening on the collection.
    status = _indexBuildsCoord->setCommitQuorum(
        operationContext(), _testFooNss, {"a_1", "b_1"}, newCommitQuorum);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound, status);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a", "b"}, {1, 2}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    // No index with the name "c" is being built.
    status =
        _indexBuildsCoord->setCommitQuorum(operationContext(), _testFooNss, {"c"}, newCommitQuorum);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound, status);

    // Pass in extra indexes not being built by the same index builder.
    status = _indexBuildsCoord->setCommitQuorum(
        operationContext(), _testFooNss, {"a_1", "b_1", "c_1"}, newCommitQuorum);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound, status);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);
    unittest::assertGet(testFoo1Future.getNoThrow());
}

TEST_F(IndexBuildsCoordinatorMongodTest, SetCommitQuorumFailsToTurnCommitQuorumFromOffToOn) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Start an index build on _testFooNss with commit quorum disabled.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a"}, {1}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    // Update the commit quorum value such that it enables commit quorum for the index
    // build 'a_1'.
    auto status = _indexBuildsCoord->setCommitQuorum(
        operationContext(), _testFooNss, {"a_1"}, CommitQuorumOptions(1));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);
    assertGet(testFoo1Future.getNoThrow());
}

TEST_F(IndexBuildsCoordinatorMongodTest, SetCommitQuorumFailsToTurnCommitQuorumFromOnToOff) {

    const IndexBuildsCoordinator::IndexBuildOptions indexBuildOptionsWithCQOn = {
        .commitQuorum = CommitQuorumOptions(1)};
    const auto buildUUID = UUID::gen();

    // Start an index build on _testFooNss with commit quorum enabled.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.dbName(),
                                                     _testFooUUID,
                                                     makeSpecs({"a"}, {1}),
                                                     buildUUID,
                                                     IndexBuildProtocol::kTwoPhase,
                                                     indexBuildOptionsWithCQOn));

    // Update the commit quorum value such that it disables commit quorum for the index
    // build 'a_1'.
    auto status =
        _indexBuildsCoord->setCommitQuorum(operationContext(),
                                           _testFooNss,
                                           {"a_1"},
                                           CommitQuorumOptions(CommitQuorumOptions::kDisabled));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);

    ASSERT_OK(_indexBuildsCoord->voteCommitIndexBuild(
        operationContext(), buildUUID, HostAndPort("test1", 1234)));

    assertGet(testFoo1Future.getNoThrow());
}

}  // namespace

}  // namespace mongo
