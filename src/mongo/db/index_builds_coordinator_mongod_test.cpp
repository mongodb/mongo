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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/uuid.h"

namespace mongo {

using unittest::assertGet;
using unittest::log;

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
    std::unique_ptr<IndexBuildsCoordinator> _indexBuildsCoord;
};

void IndexBuildsCoordinatorMongodTest::setUp() {
    CatalogTestFixture::setUp();
    createCollection(_testFooNss, _testFooUUID);
    createCollection(_testBarNss, _testBarUUID);
    createCollection(_othertestFooNss, _othertestFooUUID);
    _indexBuildsCoord = std::make_unique<IndexBuildsCoordinatorMongod>();
}

void IndexBuildsCoordinatorMongodTest::tearDown() {
    _indexBuildsCoord->verifyNoIndexBuilds_forTestOnly();
    _indexBuildsCoord->shutdown();
    _indexBuildsCoord.reset();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsCoordinatorMongodTest::createCollection(const NamespaceString& nss,
                                                        CollectionUUID uuid) {
    CollectionOptions options;
    options.uuid = uuid;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

std::vector<BSONObj> makeSpecs(const NamespaceString& nss, std::vector<std::string> keys) {
    invariant(keys.size());
    std::vector<BSONObj> indexSpecs;
    for (auto keyName : keys) {
        indexSpecs.push_back(BSON("ns" << nss.toString() << "v" << 2 << "key" << BSON(keyName << 1)
                                       << "name"
                                       << (keyName + "_1")));
    }
    return indexSpecs;
}

TEST_F(IndexBuildsCoordinatorMongodTest, CannotBuildIndexWithSameIndexName) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    auto testFoo1Future =
        assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"a", "b"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase));

    // Attempt and fail to register an index build on _testFooNss with the same index name, while
    // the prior build is still running.
    ASSERT_EQ(ErrorCodes::IndexKeySpecsConflict,
              _indexBuildsCoord
                  ->startIndexBuild(operationContext(),
                                    _testFooUUID,
                                    makeSpecs(_testFooNss, {"b"}),
                                    UUID::gen(),
                                    IndexBuildProtocol::kTwoPhase)
                  .getStatus());

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
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"a", "b"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase));

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
                                                     _testFooUUID,
                                                     makeSpecs(_testFooNss, {"c", "d"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase));

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
                                                     _testBarUUID,
                                                     makeSpecs(_testBarNss, {"x", "y"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase));

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
                                                     _othertestFooUUID,
                                                     makeSpecs(_othertestFooNss, {"r", "s"}),
                                                     UUID::gen(),
                                                     IndexBuildProtocol::kTwoPhase));

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

// Exercises the stopIndexBuildsOnCollection/Database() and allowIndexBuildsOnCollection/Database()
// functions, checking that they correctly disallow and allow index builds when
// ScopedStopNewCollectionIndexBuilds and ScopedStopNewDatabaseIndexBuilds are present on a
// collection or database name.
TEST_F(IndexBuildsCoordinatorMongodTest, DisallowNewBuildsOnNamespace) {
    {
        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

        // Create a scoped object to block new index builds ONLY on _testFooNss.
        ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), _testFooUUID);

        // Registering an index build on _testFooNss should fail.
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->startIndexBuild(operationContext(),
                                        _testFooUUID,
                                        makeSpecs(_testFooNss, {"a", "b"}),
                                        UUID::gen(),
                                        IndexBuildProtocol::kTwoPhase)
                      .getStatus());

        // Registering index builds on other collections and databases should still succeed.
        auto testBarFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _testBarUUID,
                                                         makeSpecs(_testBarNss, {"c", "d"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));
        auto othertestFooFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _othertestFooUUID,
                                                         makeSpecs(_othertestFooNss, {"e", "f"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));

        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

        auto indexCatalogStats = unittest::assertGet(testBarFuture.getNoThrow());
        ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
        indexCatalogStats = unittest::assertGet(othertestFooFuture.getNoThrow());
        ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
    }

    {
        // Check that the scoped object correctly cleared.
        auto testFooFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _testFooUUID,
                                                         makeSpecs(_testFooNss, {"a", "b"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));
        auto indexCatalogStats = unittest::assertGet(testFooFuture.getNoThrow());
        ASSERT_EQ(1, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(3, indexCatalogStats.numIndexesAfter);
    }

    {
        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

        // Create a scoped object to block new index builds on the 'test' database.
        ScopedStopNewDatabaseIndexBuilds scopedStop(_indexBuildsCoord.get(), _testFooNss.db());

        // Registering an index build on any collection in the 'test' database should fail.
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->startIndexBuild(operationContext(),
                                        _testFooUUID,
                                        makeSpecs(_testFooNss, {"a", "b"}),
                                        UUID::gen(),
                                        IndexBuildProtocol::kTwoPhase)
                      .getStatus());
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->startIndexBuild(operationContext(),
                                        _testBarUUID,
                                        makeSpecs(_testBarNss, {"c", "d"}),
                                        UUID::gen(),
                                        IndexBuildProtocol::kTwoPhase)
                      .getStatus());

        // Registering index builds on another database should still succeed.
        auto othertestFooFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _othertestFooUUID,
                                                         makeSpecs(_othertestFooNss, {"g", "h"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));

        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

        auto indexCatalogStats = unittest::assertGet(othertestFooFuture.getNoThrow());
        ASSERT_EQ(3, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(5, indexCatalogStats.numIndexesAfter);
    }

    {
        // Check that the scoped object correctly cleared.
        auto testFooFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _testFooUUID,
                                                         makeSpecs(_testFooNss, {"c", "d"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));
        auto indexCatalogStats = unittest::assertGet(testFooFuture.getNoThrow());
        ASSERT_EQ(3, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(5, indexCatalogStats.numIndexesAfter);
    }

    {
        // Test concurrency of multiple scoped objects to block an index builds.

        ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), _testFooUUID);
        {
            ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), _testFooUUID);

            ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                      _indexBuildsCoord
                          ->startIndexBuild(operationContext(),
                                            _testFooUUID,
                                            makeSpecs(_testFooNss, {"a", "b"}),
                                            UUID::gen(),
                                            IndexBuildProtocol::kTwoPhase)
                          .getStatus());
        }
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->startIndexBuild(operationContext(),
                                        _testFooUUID,
                                        makeSpecs(_testFooNss, {"a", "b"}),
                                        UUID::gen(),
                                        IndexBuildProtocol::kTwoPhase)
                      .getStatus());
    }

    {
        // Check that the scoped object correctly cleared.
        auto testFooFuture =
            assertGet(_indexBuildsCoord->startIndexBuild(operationContext(),
                                                         _testFooUUID,
                                                         makeSpecs(_testFooNss, {"e", "f"}),
                                                         UUID::gen(),
                                                         IndexBuildProtocol::kTwoPhase));
        auto indexCatalogStats = unittest::assertGet(testFooFuture.getNoThrow());
        ASSERT_EQ(5, indexCatalogStats.numIndexesBefore);
        ASSERT_EQ(7, indexCatalogStats.numIndexesAfter);
    }
}

}  // namespace

}  // namespace mongo
