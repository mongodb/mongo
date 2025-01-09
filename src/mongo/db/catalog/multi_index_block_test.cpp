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

#include <fmt/format.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * Unit test for MultiIndexBlock to verify basic functionality.
 */
class MultiIndexBlockTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

protected:
    NamespaceString getNSS() const {
        return _nss;
    }

    MultiIndexBlock* getIndexer() const {
        return _indexer.get();
    }

private:
    NamespaceString _nss;
    std::unique_ptr<MultiIndexBlock> _indexer;
};

void MultiIndexBlockTest::setUp() {
    CatalogTestFixture::setUp();

    auto service = getServiceContext();
    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    _nss = NamespaceString::createNamespaceString_forTest("db.coll");

    CollectionOptions options;
    options.uuid = UUID::gen();

    ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, options));
    _indexer = std::make_unique<MultiIndexBlock>();
}

void MultiIndexBlockTest::tearDown() {
    _indexer = {};

    CatalogTestFixture::tearDown();
}

TEST_F(MultiIndexBlockTest, CommitWithoutInsertingDocuments) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   std::vector<BSONObj>(),
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState));
    ASSERT_EQUALS(0U, specs.size());

    ASSERT_OK(indexer->dumpInsertsFromBulk(operationContext(), coll.get()));
    ASSERT_OK(indexer->checkConstraints(operationContext(), coll.get()));

    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
    }
}

TEST_F(MultiIndexBlockTest, CommitAfterInsertingSingleDocument) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   std::vector<BSONObj>(),
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::InitialSync));
    ASSERT_EQUALS(0U, specs.size());

    ASSERT_OK(indexer->insertSingleDocumentForInitialSyncOrRecovery(
        operationContext(),
        coll.get(),
        {},
        {},
        /*saveCursorBeforeWrite*/ []() {},
        /*restoreCursorAfterWrite*/ []() {}));
    ASSERT_OK(indexer->dumpInsertsFromBulk(operationContext(), coll.get()));
    ASSERT_OK(indexer->checkConstraints(operationContext(), coll.get()));

    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
    }

    // abort() should have no effect after the index build is committed.
    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, AbortWithoutCleanupAfterInsertingSingleDocument) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   std::vector<BSONObj>(),
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::InitialSync));
    ASSERT_EQUALS(0U, specs.size());
    ASSERT_OK(indexer->insertSingleDocumentForInitialSyncOrRecovery(
        operationContext(),
        coll.get(),
        {},
        {},
        /*saveCursorBeforeWrite*/ []() {},
        /*restoreCursorAfterWrite*/ []() {}));
    auto isResumable = false;
    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
}

TEST_F(MultiIndexBlockTest, InitWriteConflictException) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    BSONObj spec = BSON("key" << BSON("a" << 1) << "name"
                              << "a_1"
                              << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_THROWS_CODE(indexer->init(operationContext(),
                                         coll,
                                         {spec},
                                         [](std::vector<BSONObj>& specs) -> Status {
                                             throwWriteConflictException(
                                                 "Throw WriteConflictException in 'OnInitFn'.");
                                         }),
                           DBException,
                           ErrorCodes::WriteConflict);
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->init(operationContext(), coll, {spec}, MultiIndexBlock::kNoopOnInitFn)
                      .getStatus());
        wuow.commit();
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, InitMultipleSpecs) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    BSONObj spec = BSON("key" << BSON("a" << 1) << "name"
                              << "a_1"
                              << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion));

    // Starting multiple index builds that conflicts with each other fails, but not with
    // IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        auto status = indexer
                          ->init(operationContext(),
                                 coll,
                                 {spec, spec},
                                 [](std::vector<BSONObj>& specs) -> Status { return Status::OK(); })
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_NE(status, ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Start one index build is OK
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->init(operationContext(), coll, {spec}, MultiIndexBlock::kNoopOnInitFn)
                      .getStatus());
        wuow.commit();
    }

    auto secondaryIndexer = std::make_unique<MultiIndexBlock>();

    // Trying to start the index build again fails with IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(
            secondaryIndexer->init(operationContext(), coll, {spec}, MultiIndexBlock::kNoopOnInitFn)
                .getStatus(),
            ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Trying to start multiple index builds with the same spec fails with
    // IndexBuildAlreadyInProgress if there is an existing index build matching any spec
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(), coll, {spec, spec}, MultiIndexBlock::kNoopOnInitFn)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    BSONObj specB = BSON("key" << BSON("b" << 1) << "name"
                               << "b_1"
                               << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion));

    // If one of the requested specs are already in progress we fail with
    // IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(
            secondaryIndexer
                ->init(operationContext(), coll, {specB, spec}, MultiIndexBlock::kNoopOnInitFn)
                .getStatus(),
            ErrorCodes::IndexBuildAlreadyInProgress);
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

}  // namespace
}  // namespace mongo
