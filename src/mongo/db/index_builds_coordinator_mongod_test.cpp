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

#include "mongo/platform/basic.h"

#include "mongo/db/index_builds_coordinator_mongod.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/uuid.h"

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
    void createCollection(const NamespaceString& nss, CollectionUUID uuid);

    const CollectionUUID _testFooUUID = UUID::gen();
    const NamespaceString _testFooNss = NamespaceString("test.foo");
    const CollectionUUID _testBarUUID = UUID::gen();
    const NamespaceString _testBarNss = NamespaceString("test.bar");
    const CollectionUUID _othertestFooUUID = UUID::gen();
    const NamespaceString _othertestFooNss = NamespaceString("othertest.foo");
    const IndexBuildsCoordinator::IndexBuildOptions _indexBuildOptions = {
        CommitQuorumOptions(CommitQuorumOptions::kDisabled)};
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

    _indexBuildsCoord = std::make_unique<IndexBuildsCoordinatorMongod>();

    // Disable index build commit quorum as we don't have support of replication subsystem for
    // voting.
    ASSERT_OK(ServerParameterSet::getGlobal()
                  ->getMap()
                  .find("enableIndexBuildCommitQuorum")
                  ->second->setFromString("false"));
}

void IndexBuildsCoordinatorMongodTest::tearDown() {
    _indexBuildsCoord->shutdown(operationContext());
    _indexBuildsCoord.reset();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsCoordinatorMongodTest::createCollection(const NamespaceString& nss,
                                                        CollectionUUID uuid) {
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

std::vector<BSONObj> makeSpecs(const NamespaceString& nss, std::vector<std::string> keys) {
    invariant(keys.size());
    std::vector<BSONObj> indexSpecs;
    for (auto keyName : keys) {
        indexSpecs.push_back(
            BSON("v" << 2 << "key" << BSON(keyName << 1) << "name" << (keyName + "_1")));
    }
    return indexSpecs;
}

TEST_F(IndexBuildsCoordinatorMongodTest, AttemptBuildSameIndexReturnsImmediateSuccess) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.db().toString(),
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"a", "b"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    // Attempt and fail to register an index build on _testFooNss with the same index name, while
    // the prior build is still running.
    auto readyFuture = assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                                    _testFooNss.db().toString(),
                                                                    _testFooUUID,
                                                                    makeSpecs(_testFooNss, {"b"}),
                                                                    UUID::gen(),
                                                                    IndexBuildProtocol::kTwoPhase,
                                                                    _indexBuildOptions));

    auto readyStats = assertGet(readyFuture.getNoThrow());
    ASSERT_EQ(3, readyStats.numIndexesBefore);
    ASSERT_EQ(3, readyStats.numIndexesAfter);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);
    auto indexCatalogStats = unittest::assertGet(testFoo1Future.getNoThrow());
    ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
    ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
}

// Incrementally registering index builds and checking both that the registration was successful and
// that the access functions convey the expected state of the manager.
TEST_F(IndexBuildsCoordinatorMongodTest, Registration) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.db().toString(),
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"a", "b"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.db()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register a second index build on _testFooNss.
    auto testFoo2Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooNss.db().toString(),
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"c", "d"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.db()), 2);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a different collection _testBarNss.
    auto testBarFuture =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testBarNss.db().toString(),
                                                     _testBarUUID,
                                                     makeSpecs(_testBarNss, {"x", "y"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testBarNss.db()), 3);
    ASSERT(_indexBuildsCoord->inProgForCollection(_testBarUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testBarNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_testBarUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testBarNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a collection in a different database _othertestFoo.
    auto othertestFooFuture =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _othertestFooNss.db().toString(),
                                                     _othertestFooUUID,
                                                     makeSpecs(_othertestFooNss, {"r", "s"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase,
                                                     _indexBuildOptions));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_othertestFooNss.db()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(_othertestFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_othertestFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(_othertestFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_othertestFooNss.db()),
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

    _indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.db());
    _indexBuildsCoord->assertNoBgOpInProgForDb(_othertestFooNss.db());

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
    NamespaceString nss("bad.collection");
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
                                                     _testFooNss.db().toString(),
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"a", "b"}),
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

}  // namespace

}  // namespace mongo
