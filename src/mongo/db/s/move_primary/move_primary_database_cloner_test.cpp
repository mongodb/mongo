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

#include "mongo/db/s/move_primary/move_primary_database_cloner.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/move_primary/move_primary_cloner_test_fixture.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class MovePrimaryDatabaseClonerTest : public MovePrimaryClonerTestFixture {
public:
    MovePrimaryDatabaseClonerTest() {}

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<NamespaceString> shardedColls)
            : _shardedColls(std::move(shardedColls)) {}

        std::vector<NamespaceString> getAllShardedCollectionsForDb(
            OperationContext* opCtx,
            StringData dbName,
            repl::ReadConcernLevel readConcern,
            const BSONObj& sort = BSONObj()) override {
            return _shardedColls;
        }

    private:
        std::vector<NamespaceString> _shardedColls;
    };

protected:
    void setUp() override {
        MovePrimaryClonerTestFixture::setUp();
    }

    void tearDown() override {
        ClonerTestFixture::tearDown();
    }

    std::unique_ptr<MovePrimaryDatabaseCloner> makeDatabaseCloner(
        const stdx::unordered_set<NamespaceString>& donorShardedColls,
        const std::vector<NamespaceString>& recipientShardedColls,
        MovePrimarySharedData* sharedData = nullptr) {
        Timestamp ts = Timestamp();

        _catalogClient = std::make_unique<StaticCatalogClient>(recipientShardedColls);

        auto cloner =
            std::make_unique<MovePrimaryDatabaseCloner>(DatabaseName(boost::none, _dbName),
                                                        donorShardedColls,
                                                        ts,
                                                        sharedData ? sharedData : getSharedData(),
                                                        _source,
                                                        _mockClient.get(),
                                                        &_storageInterface,
                                                        _dbWorkThreadPool.get(),
                                                        _catalogClient.get());
        return cloner;
    }

    std::vector<BSONObj> createSourceCollections(
        const std::vector<std::pair<NamespaceString, UUID>>& sourceCollectionParams) {
        std::vector<BSONObj> result;
        for (auto& c : sourceCollectionParams) {
            auto ns = c.first;
            auto uuid = c.second;
            BSONObj b =
                BSON("db" << _dbName << "name" << ns.coll() << "type"
                          << "collection"
                          << "shard"
                          << "shard1"
                          << "md" << BSON("ns" << ns.ns() << "options" << BSON("uuid" << uuid))
                          << "idxIdent"
                          << BSON("_id_"
                                  << "index1")
                          << "ns" << ns.ns() << "ident"
                          << "xyz");

            result.emplace_back(b);
        }
        return result;
    }

    BSONObj createListExistingCollectionsOnDonorResponse(const std::vector<BSONObj>& collections) {
        auto ns =
            NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "admin"))
                .toString();
        BSONObjBuilder bob;
        {
            BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
            cursorBob.append("id", CursorId(0));
            cursorBob.append("ns", ns);
            {
                BSONArrayBuilder batchBob(cursorBob.subarrayStart("firstBatch"));
                for (const auto& coll : collections) {
                    batchBob.append(coll);
                }
            }
        }
        bob.append("ok", 1);
        bob.append("operationTime", Timestamp());
        return bob.obj();
    }

    size_t getDonorCollectionSize(MovePrimaryDatabaseCloner* cloner) const {
        return cloner->getDonorCollectionSize_ForTest();
    }
    size_t getRecipientCollectionSize(MovePrimaryDatabaseCloner* cloner) const {
        return cloner->getRecipientCollectionSize_ForTest();
    }
    size_t getCollectionsToCloneSize(MovePrimaryDatabaseCloner* cloner) const {
        return cloner->getCollectionsToCloneSize_ForTest();
    }

    const std::string _dbName = "_testDb";
    std::unique_ptr<ShardingCatalogClient> _catalogClient;
};

TEST_F(MovePrimaryDatabaseClonerTest, ListCollectionsOnSource) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, {}, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnDonor");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ListCollectionsFailsOnDuplicateNames) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto bUUID = UUID::gen();
    NamespaceString aNss = NamespaceString(_dbName, "a");

    auto cloner = makeDatabaseCloner({}, {}, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnDonor");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {std::make_pair(aNss, aUUID),
                                                                      std::make_pair(aNss, bUUID)};
    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(2U, getDonorCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ListCollectionsOnSourceFailsOnMissingUUID) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto cloner = makeDatabaseCloner({}, {}, &dummySharedData);
    NamespaceString aNss = NamespaceString(_dbName, "a");
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnDonor");
    const std::vector<BSONObj> sourceInfos = {
        BSON("db" << _dbName << "name" << aNss.coll() << "type"
                  << "collection"
                  << "shard"
                  << "shard1"
                  << "md" << BSON("ns" << aNss.ns() << "options" << BSONObj()) << "idxIdent"
                  << BSON("_id_"
                          << "index2")
                  << "ns" << aNss.ns() << "ident"
                  << "xyz")};

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
}

TEST_F(MovePrimaryDatabaseClonerTest, ListCollectionsOnSourceAndDestinationNoOverlap) {
    auto resumePhases = {ResumePhase::kNone, ResumePhase::kDataSync};
    for (auto& resumePhase : resumePhases) {
        MovePrimarySharedData dummySharedData(&_clock, _migrationId, resumePhase);
        LOGV2(5551109,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "resumePhase"_attr = dummySharedData.resumePhaseToString(resumePhase));
        auto aUUID = UUID::gen();
        auto shardedCollUUID = UUID::gen();
        auto bUUID = UUID::gen();

        NamespaceString aNss = NamespaceString(_dbName, "a");
        NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
        NamespaceString bNss = NamespaceString(_dbName, "b");
        auto shardedColls = {shardedCollNss};
        auto cloner = makeDatabaseCloner(shardedColls, {}, &dummySharedData);
        cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
        std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
            std::make_pair(aNss, aUUID),
            std::make_pair(shardedCollNss, shardedCollUUID),
            std::make_pair(bNss, bUUID)};

        const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
        _mockServer->setCommandReply("aggregate",
                                     createListExistingCollectionsOnDonorResponse(sourceInfos));
        ASSERT_OK(cloner->run());
        ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
        ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
        ASSERT_EQUALS(0U, getRecipientCollectionSize(cloner.get()));
        ASSERT_EQUALS(3U, getCollectionsToCloneSize(cloner.get()));
    }
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionOverlap) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};

    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    CollectionOptions options;
    options.uuid = shardedCollUUID;

    ASSERT_OK(createCollection(shardedCollNss, options));
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
    ASSERT_EQUALS(2U, getCollectionsToCloneSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionOverlapDataSyncPhase) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};

    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    CollectionOptions options;
    options.uuid = shardedCollUUID;

    ASSERT_OK(createCollection(shardedCollNss, options));
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
    ASSERT_EQUALS(2U, getCollectionsToCloneSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionButDifferentNamespace) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString recipientShardedCollNss = NamespaceString(_dbName, "shardedCollNssNotSame");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto recipientShardedColls = {recipientShardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, recipientShardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    CollectionOptions options;
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(recipientShardedCollNss, options));

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionButDifferentNamespaceDataSyncPhase) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString recipientShardedCollNss = NamespaceString(_dbName, "shardedCollNssNotSame");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto recipientShardedColls = {recipientShardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, recipientShardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    CollectionOptions options;
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(recipientShardedCollNss, options));

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, UnshardedCollectionButDifferentNamespace) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString recipientUnshardedCollNss = NamespaceString(_dbName, "colllNssNotSame");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    CollectionOptions options;
    options.uuid = aUUID;
    ASSERT_OK(createCollection(recipientUnshardedCollNss, options));

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionOverlapsButDifferentUUID) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(NamespaceString(_dbName, "shardedColl"), options));

    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ShardedCollectionOverlapsButDifferentUUIDDataSyncPhase) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(NamespaceString(_dbName, "shardedColl"), options));

    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, UnshardedCollectionButDifferentUUID) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString recipientUnshardedCollNss = NamespaceString(_dbName, "colllNssNotSame");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);

    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(recipientUnshardedCollNss, options));

    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, UnshardedCollectionOverlaps) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = aUUID;
    ASSERT_OK(createCollection(aNss, options));
    ASSERT_NOT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, UnshardedAndShardedCollectionsOverlapsDataSync) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = aUUID;
    ASSERT_OK(createCollection(aNss, options));
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(shardedCollNss, options));

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(2U, getRecipientCollectionSize(cloner.get()));
    ASSERT_EQUALS(2U, getCollectionsToCloneSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ExtraShardedCollectionsOnRecipient) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kNone);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    NamespaceString extraShardedCollNss = NamespaceString(_dbName, "extraShardedColl");
    auto shardedColls = {shardedCollNss};
    auto recipientShardedColls = {shardedCollNss, extraShardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, recipientShardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(shardedCollNss, options));
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(extraShardedCollNss, options));

    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(2U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, ExtraCollectionsOnRecipientDataSyncPhase) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    NamespaceString extraShardedCollNss = NamespaceString(_dbName, "extraShardedColl");
    NamespaceString extraUnshardedCollNss = NamespaceString(_dbName, "extraUnshardedColl");
    auto shardedColls = {shardedCollNss};
    auto recipientShardedColls = {shardedCollNss, extraShardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, recipientShardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(shardedCollNss, options));
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(extraShardedCollNss, options));
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(extraUnshardedCollNss, options));

    ASSERT_NOT_OK(cloner->run());
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(3U, getRecipientCollectionSize(cloner.get()));
}

TEST_F(MovePrimaryDatabaseClonerTest, AllCollectionsOverlapsDataSync) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto aUUID = UUID::gen();
    auto shardedCollUUID = UUID::gen();
    auto bUUID = UUID::gen();

    NamespaceString aNss = NamespaceString(_dbName, "a");
    NamespaceString shardedCollNss = NamespaceString(_dbName, "shardedColl");
    NamespaceString bNss = NamespaceString(_dbName, "b");
    auto shardedColls = {shardedCollNss};
    auto cloner = makeDatabaseCloner(shardedColls, shardedColls, &dummySharedData);
    cloner->setStopAfterStage_forTest("listExistingCollectionsOnRecipient");
    std::vector<std::pair<NamespaceString, UUID>> sourceCollParams = {
        std::make_pair(aNss, aUUID),
        std::make_pair(shardedCollNss, shardedCollUUID),
        std::make_pair(bNss, bUUID)};

    const std::vector<BSONObj> sourceInfos = createSourceCollections(sourceCollParams);
    _mockServer->setCommandReply("aggregate",
                                 createListExistingCollectionsOnDonorResponse(sourceInfos));

    CollectionOptions options;
    options.uuid = aUUID;
    ASSERT_OK(createCollection(aNss, options));
    options.uuid = shardedCollUUID;
    ASSERT_OK(createCollection(shardedCollNss, options));
    options.uuid = bUUID;
    ASSERT_OK(createCollection(bNss, options));

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    ASSERT_EQUALS(3U, getDonorCollectionSize(cloner.get()));
    ASSERT_EQUALS(3U, getRecipientCollectionSize(cloner.get()));
    ASSERT_EQUALS(1U, getCollectionsToCloneSize(cloner.get()));
}
}  // namespace mongo
