/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common_test.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

auto reshardingTempNss(const UUID& existingUUID) {
    return NamespaceString(fmt::format("db.system.resharding.{}", existingUUID.toString()));
}

class ReshardingDonorServiceTest : public ReshardingDonorRecipientCommonTest {
protected:
    class ThreeRecipientsCatalogClient final : public ShardingCatalogClientMock {
    public:
        ThreeRecipientsCatalogClient(UUID existingUUID, std::vector<ShardId> recipients)
            : _existingUUID(std::move(existingUUID)), _recipients(std::move(recipients)) {}

        // Makes one chunk object per shard; the actual key ranges not relevant for the test.
        // The output is deterministic since this function is also used to provide data to the
        // catalog cache loader.
        static std::vector<ChunkType> makeChunks(const NamespaceString& nss,
                                                 const std::vector<ShardId> shards,
                                                 const ChunkVersion& chunkVersion) {
            std::vector<ChunkType> chunks;
            constexpr auto kKeyDelta = 1000;

            int curKey = 0;
            for (size_t i = 0; i < shards.size(); ++i, curKey += kKeyDelta) {
                const auto min = (i == 0) ? BSON("a" << MINKEY) : BSON("a" << curKey);
                const auto max = (i == shards.size() - 1) ? BSON("a" << MAXKEY)
                                                          : BSON("a" << curKey + kKeyDelta);
                chunks.push_back(ChunkType(nss, ChunkRange(min, max), chunkVersion, shards[i]));
            }

            return chunks;
        }

        StatusWith<std::vector<ChunkType>> getChunks(
            OperationContext* opCtx,
            const BSONObj& filter,
            const BSONObj& sort,
            boost::optional<int> limit,
            repl::OpTime* opTime,
            repl::ReadConcernLevel readConcern,
            const boost::optional<BSONObj>& hint) override {
            auto version = ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);
            return makeChunks(reshardingTempNss(_existingUUID), _recipients, version);
        }

        StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
            OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
            auto shardTypes = [this]() {
                std::vector<ShardType> result;
                std::transform(
                    kRecipientShards.begin(),
                    kRecipientShards.end(),
                    std::back_inserter(result),
                    [&](const ShardId& id) { return ShardType(id.toString(), id.toString()); });
                return result;
            };
            return repl::OpTimeWith<std::vector<ShardType>>(shardTypes());
        }

        UUID _existingUUID;
        ChunkVersion maxCollVersion = ChunkVersion(0, 0, OID::gen(), boost::none);
        std::vector<ShardId> _recipients;
    };

    void setUp() override {
        _reshardingUUID = UUID::gen();

        auto mockLoader = std::make_unique<CatalogCacheLoaderMock>();
        _mockCatalogCacheLoader = mockLoader.get();

        // Must be called prior to setUp so that the catalog cache is wired properly.
        setCatalogCacheLoader(std::move(mockLoader));
        ReshardingDonorRecipientCommonTest::setUp();

        mockCatalogCacheLoader(
            NamespaceString(kReshardNs), reshardingTempNss(kExistingUUID), kRecipientShards);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<ThreeRecipientsCatalogClient>(
            uassertStatusOK(UUID::parse(kExistingUUID.toString())), kRecipientShards);
    }

    std::shared_ptr<ReshardingDonorService::DonorStateMachine> getStateMachineInstace(
        OperationContext* opCtx, ReshardingDonorDocument initialState) {
        auto instanceId = BSON(ReshardingDonorDocument::k_idFieldName << initialState.get_id());
        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        auto service = registry->lookupServiceByName(ReshardingDonorService::kServiceName);
        return ReshardingDonorService::DonorStateMachine::getOrCreate(
            opCtx, service, initialState.toBSON());
    }

    std::vector<BSONObj> getOplogWritesForDonorDocument(const ReshardingDonorDocument& doc) {
        auto reshardNs = doc.getNss().toString();
        DBDirectClient client(operationContext());
        auto result = client.query(NamespaceString(NamespaceString::kRsOplogNamespace.ns()),
                                   BSON("ns" << reshardNs));
        std::vector<BSONObj> results;
        while (result->more()) {
            results.push_back(result->next());
        }
        return results;
    }

    void mockCatalogCacheLoader(const NamespaceString& nss,
                                const NamespaceString tempNss,
                                const std::vector<ShardId>& recipients) {
        auto version = ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);
        CollectionType coll(nss, version.epoch(), Date_t::now(), UUID::gen());
        coll.setKeyPattern(BSON("y" << 1));
        coll.setUnique(false);
        coll.setAllowMigrations(false);

        _mockCatalogCacheLoader->setDatabaseRefreshReturnValue(
            DatabaseType(nss.toString(), recipients.front(), true, DatabaseVersion(UUID::gen())));
        _mockCatalogCacheLoader->setCollectionRefreshValues(
            kTemporaryReshardingNss,
            coll,
            ThreeRecipientsCatalogClient::makeChunks(tempNss, recipients, version),
            boost::none);
    }

    boost::optional<UUID> _reshardingUUID;
    boost::optional<UUID> _existingUUID;
    CatalogCacheLoaderMock* _mockCatalogCacheLoader;

    static const inline auto kRecipientShards =
        std::vector<ShardId>{ShardId("shard1"), ShardId("shard2"), ShardId("shard3")};
    static constexpr auto kExpectedO2Type = "reshardFinalOp"_sd;
    static constexpr auto kReshardNs = "db.foo"_sd;
};

TEST_F(ReshardingDonorServiceTest, ShouldWriteFinalOpLogEntryAfterTransitionToPreparingToMirror) {
    ReshardingDonorDocument doc(DonorStateEnum::kPreparingToMirror);
    CommonReshardingMetadata metadata(kReshardingUUID,
                                      mongo::NamespaceString(kReshardNs),
                                      kExistingUUID,
                                      KeyPattern(kReshardingKeyPattern));
    doc.setCommonReshardingMetadata(metadata);
    doc.getMinFetchTimestampStruct().setMinFetchTimestamp(Timestamp{0xf00});

    auto donorStateMachine = getStateMachineInstace(operationContext(), doc);
    ASSERT(donorStateMachine);

    const auto expectedRecipients =
        std::set<ShardId>(kRecipientShards.begin(), kRecipientShards.end());
    ASSERT(expectedRecipients.size());

    donorStateMachine->awaitFinalOplogEntriesWritten().get();

    const auto oplogs = getOplogWritesForDonorDocument(doc);
    ASSERT(oplogs.size() == expectedRecipients.size());

    std::set<ShardId> actualRecipients;
    for (const auto& oplog : oplogs) {
        LOGV2_INFO(5279502, "verify retrieved oplog document", "document"_attr = oplog);

        ASSERT(oplog.hasField("ns"));
        auto actualNs = oplog.getStringField("ns");
        ASSERT_EQUALS(kReshardNs, actualNs);

        ASSERT(oplog.hasField("o2"));
        auto o2 = oplog.getObjectField("o2");
        ASSERT(o2.hasField("type"));
        auto actualType = StringData(o2.getStringField("type"));
        ASSERT_EQUALS(kExpectedO2Type, actualType);
        ASSERT(o2.hasField("reshardingUUID"));
        auto actualReshardingUUIDBson = o2.getField("reshardingUUID");
        auto actualReshardingUUID = UUID::parse(actualReshardingUUIDBson);
        ASSERT_EQUALS(doc.get_id(), actualReshardingUUID);

        ASSERT(oplog.hasField("ui"));
        auto actualUiBson = oplog.getField("ui");
        auto actualUi = UUID::parse(actualUiBson);
        ASSERT_EQUALS(kExistingUUID, actualUi);

        ASSERT(oplog.hasField("destinedRecipient"));
        auto actualRecipient = oplog.getStringField("destinedRecipient");
        actualRecipients.insert(ShardId(actualRecipient));
    }

    ASSERT(expectedRecipients == actualRecipients);
}

}  // namespace
}  // namespace mongo
