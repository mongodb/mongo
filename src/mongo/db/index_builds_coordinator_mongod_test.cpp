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
    void createCollection(const NamespaceString& nss);

    const NamespaceString _testFooNss = NamespaceString("test.foo");
    const NamespaceString _testBarNss = NamespaceString("test.bar");
    const NamespaceString _othertestFooNss = NamespaceString("othertest.foo");
    std::unique_ptr<IndexBuildsCoordinator> _indexBuildsCoord;
};

void IndexBuildsCoordinatorMongodTest::setUp() {
    CatalogTestFixture::setUp();
    createCollection(_testFooNss);
    createCollection(_testBarNss);
    createCollection(_othertestFooNss);
    _indexBuildsCoord = std::make_unique<IndexBuildsCoordinatorMongod>();
}

void IndexBuildsCoordinatorMongodTest::tearDown() {
    _indexBuildsCoord->verifyNoIndexBuilds_forTestOnly();
    _indexBuildsCoord->shutdown();
    _indexBuildsCoord.reset();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsCoordinatorMongodTest::createCollection(const NamespaceString& nss) {
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
}

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
    Collection* coll = autoColl.getCollection();
    ASSERT(coll);
    return coll->uuid().get();
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
    SharedSemiFuture<void> testFoo1Future = assertGet(_indexBuildsCoord->buildIndex(
        operationContext(), _testFooNss, makeSpecs(_testFooNss, {"a", "b"}), UUID::gen()));

    // Attempt and fail to register an index build on _testFooNss with the same index name, while
    // the prior build is still running.
    ASSERT_EQ(ErrorCodes::IndexKeySpecsConflict,
              _indexBuildsCoord
                  ->buildIndex(
                      operationContext(), _testFooNss, makeSpecs(_testFooNss, {"b"}), UUID::gen())
                  .getStatus());

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);
    ASSERT_OK(testFoo1Future.getNoThrow());
}

// Incrementally registering index builds and checking both that the registration was successful and
// that the access functions convey the expected state of the manager.
TEST_F(IndexBuildsCoordinatorMongodTest, Registration) {
    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    // Register an index build on _testFooNss.
    UUID testFooUUID = getCollectionUUID(operationContext(), _testFooNss);
    SharedSemiFuture<void> testFoo1Future = assertGet(_indexBuildsCoord->buildIndex(
        operationContext(), _testFooNss, makeSpecs(_testFooNss, {"a", "b"}), UUID::gen()));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.db()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(testFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register a second index build on _testFooNss.
    SharedSemiFuture<void> testFoo2Future = assertGet(_indexBuildsCoord->buildIndex(
        operationContext(), _testFooNss, makeSpecs(_testFooNss, {"c", "d"}), UUID::gen()));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testFooNss.db()), 2);
    ASSERT(_indexBuildsCoord->inProgForCollection(testFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(testFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testFooNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a different collection _testBarNss.
    UUID testBarUUID = getCollectionUUID(operationContext(), _testBarNss);
    SharedSemiFuture<void> testBarFuture = assertGet(_indexBuildsCoord->buildIndex(
        operationContext(), _testBarNss, makeSpecs(_testBarNss, {"x", "y"}), UUID::gen()));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_testBarNss.db()), 3);
    ASSERT(_indexBuildsCoord->inProgForCollection(testBarUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_testBarNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(testBarUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_testBarNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    // Register an index build on a collection in a different database _othertestFoo.
    UUID othertestFooUUID = getCollectionUUID(operationContext(), _othertestFooNss);
    SharedSemiFuture<void> othertestFooFuture =
        assertGet(_indexBuildsCoord->buildIndex(operationContext(),
                                                _othertestFooNss,
                                                makeSpecs(_othertestFooNss, {"r", "s"}),
                                                UUID::gen()));

    ASSERT_EQ(_indexBuildsCoord->numInProgForDb(_othertestFooNss.db()), 1);
    ASSERT(_indexBuildsCoord->inProgForCollection(othertestFooUUID));
    ASSERT(_indexBuildsCoord->inProgForDb(_othertestFooNss.db()));
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoIndexBuildInProgForCollection(othertestFooUUID),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForNamespace);
    ASSERT_THROWS_CODE(_indexBuildsCoord->assertNoBgOpInProgForDb(_othertestFooNss.db()),
                       AssertionException,
                       ErrorCodes::BackgroundOperationInProgressForDatabase);

    _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

    ASSERT_OK(testFoo1Future.getNoThrow());
    ASSERT_OK(testFoo2Future.getNoThrow());
    ASSERT_OK(testBarFuture.getNoThrow());
    ASSERT_OK(othertestFooFuture.getNoThrow());

    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(testFooUUID);
    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(testBarUUID);
    _indexBuildsCoord->assertNoIndexBuildInProgForCollection(othertestFooUUID);

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
    UUID testFooUUID = getCollectionUUID(operationContext(), _testFooNss);

    {
        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

        // Create a scoped object to block new index builds ONLY on _testFooNss.
        ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), testFooUUID);

        // Registering an index build on _testFooNss should fail.
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->buildIndex(operationContext(),
                                   _testFooNss,
                                   makeSpecs(_testFooNss, {"a", "b"}),
                                   UUID::gen())
                      .getStatus());

        // Registering index builds on other collections and databases should still succeed.
        SharedSemiFuture<void> testBarFuture = assertGet(_indexBuildsCoord->buildIndex(
            operationContext(), _testBarNss, makeSpecs(_testBarNss, {"c", "d"}), UUID::gen()));
        SharedSemiFuture<void> othertestFooFuture =
            assertGet(_indexBuildsCoord->buildIndex(operationContext(),
                                                    _othertestFooNss,
                                                    makeSpecs(_othertestFooNss, {"e", "f"}),
                                                    UUID::gen()));

        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

        ASSERT_OK(testBarFuture.getNoThrow());
        ASSERT_OK(othertestFooFuture.getNoThrow());
    }

    {
        // Check that the scoped object correctly cleared.
        SharedSemiFuture<void> testFooFuture = assertGet(_indexBuildsCoord->buildIndex(
            operationContext(), _testFooNss, makeSpecs(_testFooNss, {"a", "b"}), UUID::gen()));
        ASSERT_OK(testFooFuture.getNoThrow());
    }

    {
        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

        // Create a scoped object to block new index builds on the 'test' database.
        ScopedStopNewDatabaseIndexBuilds scopedStop(_indexBuildsCoord.get(), _testFooNss.db());

        // Registering an index build on any collection in the 'test' database should fail.
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->buildIndex(operationContext(),
                                   _testFooNss,
                                   makeSpecs(_testFooNss, {"a", "b"}),
                                   UUID::gen())
                      .getStatus());
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->buildIndex(operationContext(),
                                   _testBarNss,
                                   makeSpecs(_testBarNss, {"c", "d"}),
                                   UUID::gen())
                      .getStatus());

        // Registering index builds on another database should still succeed.
        SharedSemiFuture<void> othertestFooFuture =
            assertGet(_indexBuildsCoord->buildIndex(operationContext(),
                                                    _othertestFooNss,
                                                    makeSpecs(_othertestFooNss, {"e", "f"}),
                                                    UUID::gen()));

        _indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

        ASSERT_OK(othertestFooFuture.getNoThrow());
    }

    {
        // Check that the scoped object correctly cleared.
        SharedSemiFuture<void> testFooFuture = assertGet(_indexBuildsCoord->buildIndex(
            operationContext(), _testFooNss, makeSpecs(_testFooNss, {"a", "b"}), UUID::gen()));
        ASSERT_OK(testFooFuture.getNoThrow());
    }

    {
        // Test concurrency of multiple scoped objects to block an index builds.

        ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), testFooUUID);
        {
            ScopedStopNewCollectionIndexBuilds scopedStop(_indexBuildsCoord.get(), testFooUUID);

            ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                      _indexBuildsCoord
                          ->buildIndex(operationContext(),
                                       _testFooNss,
                                       makeSpecs(_testFooNss, {"a", "b"}),
                                       UUID::gen())
                          .getStatus());
        }
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  _indexBuildsCoord
                      ->buildIndex(operationContext(),
                                   _testFooNss,
                                   makeSpecs(_testFooNss, {"a", "b"}),
                                   UUID::gen())
                      .getStatus());
    }

    {
        // Check that the scoped object correctly cleared.
        SharedSemiFuture<void> testFooFuture = assertGet(_indexBuildsCoord->buildIndex(
            operationContext(), _testFooNss, makeSpecs(_testFooNss, {"a", "b"}), UUID::gen()));
        ASSERT_OK(testFooFuture.getNoThrow());
    }
}

}  // namespace

}  // namespace mongo
