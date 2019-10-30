/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class CollectionClonerTest : public ClonerTestFixture {
public:
    CollectionClonerTest() {}

protected:
    void setUp() final {
        ClonerTestFixture::setUp();
        _collectionStats = std::make_shared<CollectionMockStats>();
        _storageInterface.createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj idIndexSpec,
                   const std::vector<BSONObj>& nonIdIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(_collectionStats);
            Status result = localLoader->init(nonIdIndexSpecs);
            if (!result.isOK())
                return result;

            _loader = localLoader.get();

            return std::move(localLoader);
        };
        _mockServer->assignCollectionUuid(_nss.ns(), _collUuid);
    }
    std::unique_ptr<CollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions()) {
        options.uuid = _collUuid;
        return std::make_unique<CollectionCloner>(_nss,
                                                  options,
                                                  _sharedData.get(),
                                                  _source,
                                                  _mockClient.get(),
                                                  &_storageInterface,
                                                  _dbWorkThreadPool.get());
    }

    ProgressMeter& getProgressMeter(CollectionCloner* cloner) {
        return cloner->_progressMeter;
    }

    std::vector<BSONObj>& getIndexSpecs(CollectionCloner* cloner) {
        return cloner->_indexSpecs;
    }

    BSONObj& getIdIndexSpec(CollectionCloner* cloner) {
        return cloner->_idIndexSpec;
    }

    std::shared_ptr<CollectionMockStats> _collectionStats;  // Used by the _loader.
    CollectionBulkLoaderMock* _loader = nullptr;            // Owned by CollectionCloner.

    NamespaceString _nss = {"testDb", "testColl"};
    UUID _collUuid = UUID::gen();
};

TEST_F(CollectionClonerTest, CountStage) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(100, getProgressMeter(cloner.get()).total());
}

// On a negative count, the CollectionCloner should use a zero count.
TEST_F(CollectionClonerTest, CountStageNegativeCount) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(-100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(0, getProgressMeter(cloner.get()).total());
}

// On NamespaceNotFound, the CollectionCloner should exit without doing anything.
TEST_F(CollectionClonerTest, CountStageNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::NamespaceNotFound, "NoSuchUuid"));
    ASSERT_OK(cloner->run());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughCommandStatusErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    _mockServer->setCommandReply("count",
                                 BSON("ok" << 0 << "errmsg"
                                           << "TEST error"
                                           << "code" << int(ErrorCodes::OperationFailed)));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_STRING_CONTAINS(status.reason(), "TEST error");
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", BSON("ok" << 1));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
}

TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    ASSERT_OK(cloner->run());
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

// NamespaceNotFound is treated the same as no index.
TEST_F(CollectionClonerTest, ListIndexesReturnedNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 Status(ErrorCodes::NamespaceNotFound, "No indexes here."));
    ASSERT_OK(cloner->run());
    ASSERT(!_loader);  // We expect not to have run the create collection.
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTest, ListIndexesHasResults) {
    auto cloner = makeCollectionCloner();
    BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                   << "_id_");
    BSONObj nonIdIndexSpec1 = BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                       << "a_1");
    BSONObj nonIdIndexSpec2 = BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                       << "b_1");
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply(
        "listIndexes",
        createCursorResponse(_nss.ns(),
                             BSON_ARRAY(nonIdIndexSpec1 << idIndexSpec << nonIdIndexSpec2)));
    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(2, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(nonIdIndexSpec1, getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(nonIdIndexSpec2, getIndexSpecs(cloner.get())[1]);
    ASSERT_EQ(3, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    _storageInterface.createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    // Set up data for preliminary stages
    auto idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                << "_id_");
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(2, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    // Set up data for preliminary stages
    auto idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                << "_id_");
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(3, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    auto stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);
}

}  // namespace repl
}  // namespace mongo
