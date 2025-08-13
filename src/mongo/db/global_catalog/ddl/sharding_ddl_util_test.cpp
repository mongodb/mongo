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

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/idl/error_status_idl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

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
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        // Initialize a shard
        shard0.setName("shard0");
        shard0.setHost("shard0:12");
        setupShards({shard0});
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interrupt();
        ConfigServerTestFixture::tearDown();
    }

public:
    CollectionType setupShardedCollection(const NamespaceString& nss) {

        // Initialize a chunk
        ChunkVersion chunkVersion({OID::gen(), Timestamp(2, 1)}, {1, 1});
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());
        chunk.setVersion(chunkVersion);
        chunk.setShard(shard0.getName());
        chunk.setOnCurrentShardSince(Timestamp(1, 1));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shard0.getName())});
        chunk.setRange({kMinBSONKey, kMaxBSONKey});

        // Initialize the sharded collection
        return setupCollection(nss, KeyPattern(BSON("x" << 1)), {chunk});
    }
};

const NamespaceString kFromNss = NamespaceString::createNamespaceString_forTest("test.from");
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
    idl::serializeErrorStatus(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized = idl::deserializeErrorStatus(serialized.firstElement());

    ASSERT_EQ(sample.code(), deserialized.code());
    ASSERT_EQ(sample.reason(), deserialized.reason());
    ASSERT(!deserialized.extraInfo());
}

TEST_F(ShardingDDLUtilTest, SerializeDeserializeErrorStatusWithExtraInfo) {
    OptionalErrorExtraInfoExample::EnableParserForTest whenInScope;

    const Status sample{
        ErrorCodes::ForTestingOptionalErrorExtraInfo, "Dummy reason", fromjson("{data: 123}")};

    BSONObjBuilder bsonBuilder;
    idl::serializeErrorStatus(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized = idl::deserializeErrorStatus(serialized.firstElement());

    ASSERT_EQ(sample.code(), deserialized.code());
    ASSERT_EQ(sample.reason(), deserialized.reason());
    ASSERT(deserialized.extraInfo());
    ASSERT(deserialized.extraInfo<OptionalErrorExtraInfoExample>());
    ASSERT_EQ(deserialized.extraInfo<OptionalErrorExtraInfoExample>()->data, 123);
}

TEST_F(ShardingDDLUtilTest, SerializeDeserializeErrorStatusInvalid) {
    BSONObjBuilder bsonBuilder;
    ASSERT_THROWS_CODE(
        idl::serializeErrorStatus(Status::OK(), "status", &bsonBuilder), DBException, 7418500);

    const auto okStatusBSON =
        BSON("status" << BSON("code" << ErrorCodes::OK << "codeName"
                                     << ErrorCodes::errorString(ErrorCodes::OK)));
    ASSERT_THROWS_CODE(
        idl::deserializeErrorStatus(okStatusBSON.firstElement()), DBException, 7418501);
}

TEST_F(ShardingDDLUtilTest, TruncateBigErrorStatus) {
    const std::string longReason(1024 * 3, 'x');
    const Status sample{ErrorCodes::ForTestingOptionalErrorExtraInfo, longReason};

    BSONObjBuilder bsonBuilder;
    idl::serializeErrorStatus(sample, "status", &bsonBuilder);
    const auto serialized = bsonBuilder.done();

    const auto deserialized = idl::deserializeErrorStatus(serialized.firstElement());
    const auto truncated = sharding_ddl_util::possiblyTruncateErrorStatus(deserialized);

    ASSERT_EQ(ErrorCodes::TruncatedSerialization, truncated.code());
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
        truncated.reason());
    ASSERT(!truncated.extraInfo());
}

// Test all combinations of rename acceptable preconditions:
// (1) Namespace of target collection is not too long
// (2) Target collection doesn't exist and doesn't have no associated tags
// (3) Target collection exists and doesn't have associated tags
TEST_F(ShardingDDLUtilTest, RenamePreconditionsAreMet) {
    auto opCtx = operationContext();

    // Initialize the sharded FROM collection
    const auto fromColl = setupShardedCollection(kFromNss);

    // No exception is thrown if the TO collection does not exist and has no associated tags
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, kToNss, boost::none /*toColl*/, false /*isSourceUnsharded*/, false /* dropTarget */);

    // Initialize the sharded TO collection
    const auto toColl = setupShardedCollection(kToNss);

    // No exception is thrown if the TO collection exists and dropTarget is `true`
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, kToNss, toColl, false /*isSourceUnsharded*/, true /* dropTarget */);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionsTargetNamespaceIsTooLong) {
    auto opCtx{operationContext()};

    const std::string dbName{"test"};

    // Initialize the sharded FROM collection
    const auto fromColl = setupShardedCollection(kFromNss);

    // Check that no exception is thrown if the namespace of the target collection is long enough
    const NamespaceString longEnoughNss = NamespaceString::createNamespaceString_forTest(
        dbName + "." +
        std::string(NamespaceString::MaxUserNsShardedCollectionLen - dbName.length() - 1, 'x'));
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, longEnoughNss, boost::none, false /*isSourceUnsharded*/, false /* dropTarget */);

    // Check that an exception is thrown if the namespace of the target collection is too long
    const NamespaceString tooLongNss =
        NamespaceString::createNamespaceString_forTest(longEnoughNss.toString_forTest() + 'x');
    ASSERT_THROWS_CODE(
        sharding_ddl_util::checkRenamePreconditions(
            opCtx, tooLongNss, boost::none, false /*isSourceUnsharded*/, false /* dropTarget */),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionsTargetNamespaceIsTooLongUnsplittable) {
    auto opCtx{operationContext()};

    const std::string dbName{"test"};

    // Initialize the sharded FROM collection
    const auto fromColl = setupShardedCollection(kFromNss);

    // Check that no exception is thrown if the namespace of the target collection is long enough
    const NamespaceString longEnoughNss = NamespaceString::createNamespaceString_forTest(
        dbName + "." +
        std::string(NamespaceString::MaxUserNsCollectionLen - dbName.length() - 1, 'x'));
    sharding_ddl_util::checkRenamePreconditions(
        opCtx, longEnoughNss, boost::none, true /*isSourceUnsharded*/, false /* dropTarget */);

    // Check that an exception is thrown if the namespace of the target collection is too long
    const NamespaceString tooLongNss =
        NamespaceString::createNamespaceString_forTest(longEnoughNss.toString_forTest() + 'x');
    ASSERT_THROWS_CODE(
        sharding_ddl_util::checkRenamePreconditions(
            opCtx, tooLongNss, boost::none, true /*isSourceUnsharded*/, false /* dropTarget */),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionsTargetCollectionExists) {
    auto opCtx = operationContext();

    // Initialize the sharded FROM collection
    const auto fromColl = setupShardedCollection(kFromNss);

    // Initialize the sharded TO collection
    const auto toColl = setupShardedCollection(kToNss);

    // Check that an exception is thrown if the target collection exists and dropTarget is not set
    ASSERT_THROWS_CODE(
        sharding_ddl_util::checkRenamePreconditions(
            opCtx, kToNss, toColl, false /*isSourceUnsharded*/, false /* dropTarget */),
        AssertionException,
        ErrorCodes::NamespaceExists);
}

TEST_F(ShardingDDLUtilTest, RenamePreconditionTargetCollectionHasTags) {
    auto opCtx = operationContext();

    // Initialize the sharded FROM collection
    const auto fromColl = setupShardedCollection(kFromNss);

    // Initialize the sharded TO collection
    const auto toColl = setupShardedCollection(kToNss);

    // Associate a tag to the target collection
    TagsType tagDoc;
    tagDoc.setNS(kToNss);
    tagDoc.setRange({BSON("x" << 0), BSON("x" << 1)});
    tagDoc.setTag("z");
    ASSERT_OK(insertToConfigCollection(operationContext(), TagsType::ConfigNS, tagDoc.toBSON()));

    // Check that an exception is thrown if some tag is associated to the target collection
    ASSERT_THROWS_CODE(
        sharding_ddl_util::checkRenamePreconditions(
            opCtx, kToNss, toColl, false /*isSourceUnsharded*/, true /* dropTarget */),
        AssertionException,
        ErrorCodes::CommandFailed);
}

TEST_F(ShardingDDLUtilTest, NamespaceTooLong) {
    auto generateNss = [](int len) {
        std::string dbName = "test";
        auto collName = std::string(len - dbName.size() - 1, 'x');
        return NamespaceString::createNamespaceString_forTest(dbName, collName);
    };

    NamespaceString validNss = generateNss(NamespaceString::MaxUserNsShardedCollectionLen);
    NamespaceString invalidShardedNss =
        generateNss(NamespaceString::MaxUserNsShardedCollectionLen + 1);
    NamespaceString invalidNss = generateNss(NamespaceString::MaxUserNsCollectionLen + 1);

    // Always valid.
    ASSERT_DOES_NOT_THROW(
        sharding_ddl_util::assertNamespaceLengthLimit(validNss, true /*isUnsharded*/));
    ASSERT_DOES_NOT_THROW(
        sharding_ddl_util::assertNamespaceLengthLimit(validNss, false /*isUnsharded*/));

    // Only valid if the collection is unsharded.
    ASSERT_THROWS_CODE(
        sharding_ddl_util::assertNamespaceLengthLimit(invalidShardedNss, false /*isUnsharded*/),
        DBException,
        ErrorCodes::InvalidNamespace);
    ASSERT_DOES_NOT_THROW(
        sharding_ddl_util::assertNamespaceLengthLimit(invalidShardedNss, true /*isUnsharded*/));

    // Never valid.
    ASSERT_THROWS_CODE(
        sharding_ddl_util::assertNamespaceLengthLimit(invalidNss, true /*isUnsharded*/),
        DBException,
        ErrorCodes::InvalidNamespace);
    ASSERT_THROWS_CODE(
        sharding_ddl_util::assertNamespaceLengthLimit(invalidNss, false /*isUnsharded*/),
        DBException,
        ErrorCodes::InvalidNamespace);
}

}  // namespace
}  // namespace mongo
