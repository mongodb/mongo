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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardingDDLUtilTest : public ConfigServerTestFixture {
protected:
    ShardType shard0;

private:
    ReadWriteConcernDefaultsLookupMock _lookupMock;

    void setUp() override {
        setUpAndInitializeConfigDb();

        // Manually instantiate the ReadWriteConcernDefaults decoration on the service
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace);
        client.createCollection(CollectionType::ConfigNS);

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);

        // Initialize a shard
        shard0.setName("shard0");
        shard0.setHost("shard0:12");
        setupShards({shard0});
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }
};

const NamespaceString kToNss = NamespaceString::createNamespaceString_forTest("test.to");

// Query 'limit' objects from the database into an array.
void findN(DBClientBase& client,
           FindCommandRequest findRequest,
           int limit,
           std::vector<BSONObj>& out) {
    out.reserve(limit);
    findRequest.setLimit(limit);
    std::unique_ptr<DBClientCursor> c = client.find(std::move(findRequest));
    ASSERT(c.get());

    while (c->more()) {
        out.push_back(c->nextSafe());
    }
}

TEST_F(ShardingDDLUtilTest, SerializeDeserializeErrorStatusWithoutExtraInfo) {
    const Status sample{ErrorCodes::ForTestingOptionalErrorExtraInfo, "Dummy reason"};

    BSONObjBuilder bsonBuilder;
    sharding_ddl_util_serializeErrorStatusToBSON(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized =
        sharding_ddl_util_deserializeErrorStatusFromBSON(serialized.firstElement());

    ASSERT_EQ(sample.code(), deserialized.code());
    ASSERT_EQ(sample.reason(), deserialized.reason());
    ASSERT(!deserialized.extraInfo());
}

TEST_F(ShardingDDLUtilTest, SerializeDeserializeErrorStatusWithExtraInfo) {
    OptionalErrorExtraInfoExample::EnableParserForTest whenInScope;

    const Status sample{
        ErrorCodes::ForTestingOptionalErrorExtraInfo, "Dummy reason", fromjson("{data: 123}")};

    BSONObjBuilder bsonBuilder;
    sharding_ddl_util_serializeErrorStatusToBSON(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized =
        sharding_ddl_util_deserializeErrorStatusFromBSON(serialized.firstElement());

    ASSERT_EQ(sample.code(), deserialized.code());
    ASSERT_EQ(sample.reason(), deserialized.reason());
    ASSERT(deserialized.extraInfo());
    ASSERT(deserialized.extraInfo<OptionalErrorExtraInfoExample>());
    ASSERT_EQ(deserialized.extraInfo<OptionalErrorExtraInfoExample>()->data, 123);
}

TEST_F(ShardingDDLUtilTest, SerializeDeserializeErrorStatusInvalid) {
    BSONObjBuilder bsonBuilder;
    ASSERT_THROWS_CODE(
        sharding_ddl_util_serializeErrorStatusToBSON(Status::OK(), "status", &bsonBuilder),
        DBException,
        7418500);

    const auto okStatusBSON =
        BSON("status" << BSON("code" << ErrorCodes::OK << "codeName"
                                     << ErrorCodes::errorString(ErrorCodes::OK)));
    ASSERT_THROWS_CODE(
        sharding_ddl_util_deserializeErrorStatusFromBSON(okStatusBSON.firstElement()),
        DBException,
        7418501);
}

TEST_F(ShardingDDLUtilTest, SerializeErrorStatusTooBig) {
    const std::string longReason(1024 * 3, 'x');
    const Status sample{ErrorCodes::ForTestingOptionalErrorExtraInfo, longReason};

    BSONObjBuilder bsonBuilder;
    sharding_ddl_util_serializeErrorStatusToBSON(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized =
        sharding_ddl_util_deserializeErrorStatusFromBSON(serialized.firstElement());

    ASSERT_EQ(ErrorCodes::TruncatedSerialization, deserialized.code());
    ASSERT_EQ(
        "ForTestingOptionalErrorExtraInfo: "
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        deserialized.reason());
    ASSERT(!deserialized.extraInfo());
}

// Test that config.collection document and config.chunks documents are properly updated from source
// to destination collection metadata
TEST_F(ShardingDDLUtilTest, ShardedRenameMetadata) {
    auto opCtx = operationContext();
    DBDirectClient client(opCtx);

    const NamespaceString fromNss = NamespaceString::createNamespaceString_forTest("test.from");
    const auto fromCollQuery = BSON(CollectionType::kNssFieldName << fromNss.ns());

    const auto toCollQuery = BSON(CollectionType::kNssFieldName << kToNss.ns());

    const Timestamp collTimestamp(1);
    const auto collUUID = UUID::gen();

    // Initialize FROM collection chunks
    const auto fromEpoch = OID::gen();
    const int nChunks = 10;
    std::vector<ChunkType> chunks;
    for (int i = 0; i < nChunks; i++) {
        ChunkVersion chunkVersion({fromEpoch, collTimestamp}, {1, uint32_t(i)});
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUUID);
        chunk.setVersion(chunkVersion);
        chunk.setShard(shard0.getName());
        chunk.setOnCurrentShardSince(Timestamp(1, i));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shard0.getName())});
        chunk.setMin(BSON("a" << i));
        chunk.setMax(BSON("a" << i + 1));
        chunks.push_back(chunk);
    }

    setupCollection(fromNss, KeyPattern(BSON("x" << 1)), chunks);

    // Initialize TO collection chunks
    std::vector<ChunkType> originalToChunks;
    const auto toEpoch = OID::gen();
    const auto toUUID = UUID::gen();
    for (int i = 0; i < nChunks; i++) {
        ChunkVersion chunkVersion({toEpoch, Timestamp(2)}, {1, uint32_t(i)});
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(toUUID);
        chunk.setVersion(chunkVersion);
        chunk.setShard(shard0.getName());
        chunk.setOnCurrentShardSince(Timestamp(1, i));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shard0.getName())});
        chunk.setMin(BSON("a" << i));
        chunk.setMax(BSON("a" << i + 1));
        originalToChunks.push_back(chunk);
    }
    setupCollection(kToNss, KeyPattern(BSON("x" << 1)), originalToChunks);

    // Get FROM collection document and chunks
    auto fromDoc = client.findOne(CollectionType::ConfigNS, fromCollQuery);
    CollectionType fromCollection(fromDoc);

    FindCommandRequest fromChunksRequest{ChunkType::ConfigNS};
    fromChunksRequest.setFilter(BSON(ChunkType::collectionUUID << collUUID));
    fromChunksRequest.setSort(BSON("_id" << 1));

    std::vector<BSONObj> fromChunks;
    findN(client, std::move(fromChunksRequest), nChunks, fromChunks);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto fromCollType = catalogClient->getCollection(opCtx, fromNss);
    // Perform the metadata rename
    sharding_ddl_util::shardedRenameMetadata(opCtx,
                                             configShard,
                                             catalogClient,
                                             fromCollType,
                                             kToNss,
                                             ShardingCatalogClient::kMajorityWriteConcern);

    // Check that the FROM config.collections entry has been deleted
    ASSERT(client.findOne(CollectionType::ConfigNS, fromCollQuery).isEmpty());

    // Get TO collection document and chunks
    auto toDoc = client.findOne(CollectionType::ConfigNS, toCollQuery);
    CollectionType toCollection(toDoc);

    FindCommandRequest toChunksRequest{ChunkType::ConfigNS};
    toChunksRequest.setFilter(BSON(ChunkType::collectionUUID << collUUID));
    toChunksRequest.setSort(BSON("_id" << 1));

    std::vector<BSONObj> toChunks;
    findN(client, std::move(toChunksRequest), nChunks, toChunks);

    // Check that original epoch/timestamp are changed in config.collections entry
    ASSERT(fromCollection.getEpoch() != toCollection.getEpoch());
    ASSERT(fromCollection.getTimestamp() != toCollection.getTimestamp());

    // Check that no other CollectionType field has been changed
    auto fromUnchangedFields = fromDoc.removeField(CollectionType::kNssFieldName)
                                   .removeField(CollectionType::kEpochFieldName)
                                   .removeField(CollectionType::kTimestampFieldName);
    auto toUnchangedFields = toDoc.removeField(CollectionType::kNssFieldName)
                                 .removeField(CollectionType::kEpochFieldName)
                                 .removeField(CollectionType::kTimestampFieldName);
    ASSERT_EQ(fromUnchangedFields.woCompare(toUnchangedFields), 0);

    // Check that chunk documents remain unchanged
    for (int i = 0; i < nChunks; i++) {
        auto fromChunkDoc = fromChunks[i];
        auto toChunkDoc = toChunks[i];

        ASSERT_EQ(fromChunkDoc.woCompare(toChunkDoc), 0);
    }
}

// Test all combinations of rename acceptable preconditions:
// (1) Namespace of target collection is not too long
// (2) Target collection doesn't exist and doesn't have no associated tags
// (3) Target collection exists and doesn't have associated tags
TEST_F(ShardingDDLUtilTest, RenamePreconditionsAreMet) {
    auto opCtx = operationContext();

    // No exception is thrown if the TO collection does not exist and has no associated tags
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, false /* sourceIsSharded */, kToNss, false /* dropTarget */);

    // Initialize a chunk
    ChunkVersion chunkVersion({OID::gen(), Timestamp(2, 1)}, {1, 1});
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(UUID::gen());
    chunk.setVersion(chunkVersion);
    chunk.setShard(shard0.getName());
    chunk.setOnCurrentShardSince(Timestamp(1, 1));
    chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shard0.getName())});
    chunk.setMin(kMinBSONKey);
    chunk.setMax(kMaxBSONKey);

    // Initialize the sharded TO collection
    setupCollection(kToNss, KeyPattern(BSON("x" << 1)), {chunk});

    sharding_ddl_util::checkRenamePreconditions(
        opCtx, false /* sourceIsSharded */, kToNss, true /* dropTarget */);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionsTargetNamespaceIsTooLong) {
    auto opCtx{operationContext()};

    const std::string dbName{"test"};

    // Check that no exception is thrown if the namespace of the target collection is long enough
    const NamespaceString longEnoughNss = NamespaceString::createNamespaceString_forTest(
        dbName + "." +
        std::string(NamespaceString::MaxNsShardedCollectionLen - dbName.length() - 1, 'x'));
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, true /* sourceIsSharded */, longEnoughNss, false /* dropTarget */);

    // Check that an exception is thrown if the namespace of the target collection is too long
    const NamespaceString tooLongNss =
        NamespaceString::createNamespaceString_forTest(longEnoughNss.ns().toString() + 'x');
    ASSERT_THROWS_CODE(sharding_ddl_util::checkRenamePreconditions(
                           opCtx, true /* sourceIsSharded */, tooLongNss, false /* dropTarget */),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionsTargetCollectionExists) {
    auto opCtx = operationContext();

    // Initialize a chunk
    ChunkVersion chunkVersion({OID::gen(), Timestamp(2, 1)}, {1, 1});
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(UUID::gen());
    chunk.setVersion(chunkVersion);
    chunk.setShard(shard0.getName());
    chunk.setOnCurrentShardSince(Timestamp(1, 1));
    chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shard0.getName())});
    chunk.setMin(kMinBSONKey);
    chunk.setMax(kMaxBSONKey);

    // Initialize the sharded collection
    setupCollection(kToNss, KeyPattern(BSON("x" << 1)), {chunk});

    // Check that an exception is thrown if the target collection exists and dropTarget is not set
    ASSERT_THROWS_CODE(sharding_ddl_util::checkRenamePreconditions(
                           opCtx, false /* sourceIsSharded */, kToNss, false /* dropTarget */),
                       AssertionException,
                       ErrorCodes::NamespaceExists);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionTargetCollectionHasTags) {
    auto opCtx = operationContext();

    // Associate a tag to the target collection
    TagsType tagDoc;
    tagDoc.setNS(kToNss);
    tagDoc.setMinKey(BSON("x" << 0));
    tagDoc.setMaxKey(BSON("x" << 1));
    tagDoc.setTag("z");
    ASSERT_OK(insertToConfigCollection(operationContext(), TagsType::ConfigNS, tagDoc.toBSON()));

    // Check that an exception is thrown if some tag is associated to the target collection
    ASSERT_THROWS_CODE(sharding_ddl_util::checkRenamePreconditions(
                           opCtx, false /* sourceIsSharded */, kToNss, false /* dropTarget */),
                       AssertionException,
                       ErrorCodes::CommandFailed);
}

}  // namespace
}  // namespace mongo
