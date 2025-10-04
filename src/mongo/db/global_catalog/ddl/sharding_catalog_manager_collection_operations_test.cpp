/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cassert>
#include <cstdint>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardingCatalogManagerCollectionOperationTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    std::string _shardHostName = "host01";

    const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest(kDbName, "coll");
    const NamespaceString kBucketNss = kNss.makeTimeseriesBucketsNamespace();
    const std::string kShardKeyField = "meta";
    const ShardKeyPattern kTimestampShardKeyPattern{BSON(kShardKeyField << 1)};

    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std ::make_unique<LogicalSessionCacheNoop>());

        setupShards({ShardType(_shardName, _shardHostName)});
        setupDatabase(kDbName, _shardName);
    }

    void setupTimeseriesCollection(const NamespaceString& nss) {
        const auto epoch = OID::gen();
        const auto collectionTimestamp = Timestamp{100, 0};
        const auto collectionUuid = UUID::gen();

        CollectionType collType(nss,
                                epoch,
                                collectionTimestamp,
                                Date_t::now() /* updatedAt */,
                                collectionUuid,
                                kTimestampShardKeyPattern.toBSON());

        ChunkVersion latestVersion({epoch, collectionTimestamp}, {1, 0});

        ChunkType chunk(collectionUuid,
                        {kTimestampShardKeyPattern.getKeyPattern().globalMin(),
                         kTimestampShardKeyPattern.getKeyPattern().globalMax()},
                        latestVersion,
                        _shardName);
        chunk.setName(OID::gen());
        chunk.setOnCurrentShardSince(Timestamp(200, 0));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), _shardName)});

        auto setTsFieldsLambda = [&](CollectionType& c) {
            TypeCollectionTimeseriesFields tsFields;
            tsFields.setTimeField("time");
            tsFields.setMetaField(kShardKeyField);
            tsFields.setGranularity(BucketGranularityEnum::Seconds);
            c.setTimeseriesFields(tsFields);
        };

        setupCollection(nss, kTimestampShardKeyPattern.getKeyPattern(), {chunk}, setTsFieldsLambda);
    }

    CollectionType getConfigCollectionEntry(NamespaceString nss) {
        return CollectionType(
            findOneOnConfigCollection(operationContext(),
                                      CollectionType::ConfigNS,
                                      BSON(CollectionType::kNssFieldName << nss.ns_forTest()))
                .getValue());
    }
};

TEST_F(ShardingCatalogManagerCollectionOperationTest, UpdateTimeseriesBucketingParamsTest) {
    auto opCtx = operationContext();

    const auto getOptionalGranularity = [&]() {
        return getConfigCollectionEntry(kNss).getTimeseriesFields()->getGranularity();
    };

    const auto getOptionalRoundingAndMaxSpan =
        [&]() -> std::pair<boost::optional<int>, boost::optional<int>> {
        const auto tsFields = getConfigCollectionEntry(kNss).getTimeseriesFields();
        return {tsFields->getBucketRoundingSeconds(), tsFields->getBucketMaxSpanSeconds()};
    };

    const auto getMixedSchemaFlag = [&]() {
        return getConfigCollectionEntry(kNss)
            .getTimeseriesFields()
            ->getTimeseriesBucketsMayHaveMixedSchemaData()
            .get_value_or(false);
    };

    const auto getChunk = [&]() {
        const auto collEntry = getConfigCollectionEntry(kNss);
        return uassertStatusOK(getChunkDoc(opCtx,
                                           collEntry.getUuid(),
                                           kTimestampShardKeyPattern.getKeyPattern().globalMin(),
                                           collEntry.getEpoch(),
                                           collEntry.getTimestamp()));
    };

    setupTimeseriesCollection(kNss);

    const auto originalChunkVersion = getChunk().getVersion();
    int expectedMajorVersion = originalChunkVersion.majorVersion();

    const auto updateTimeseriesBucketingParameters = [&](CollModRequest req) {
        ShardingCatalogManager::get(opCtx)->updateTimeSeriesBucketingParameters(opCtx, kNss, req);
        expectedMajorVersion++;
    };

    /* Set the mixed schema flag works */
    {
        ASSERT_FALSE(getMixedSchemaFlag());
        CollModRequest collModReq;
        collModReq.setTimeseriesBucketsMayHaveMixedSchemaData(true);

        updateTimeseriesBucketingParameters(collModReq);

        ASSERT_TRUE(getMixedSchemaFlag());
    }

    /* Unset the mixed schema flag works */
    {
        CollModRequest collModReq;
        collModReq.setTimeseriesBucketsMayHaveMixedSchemaData(false);

        updateTimeseriesBucketingParameters(collModReq);

        ASSERT_FALSE(getMixedSchemaFlag());
    }

    /* Set rounding and max span works */
    {
        const auto rounding = 12345678, maxSpan = 87654;

        ASSERT_TRUE(getOptionalGranularity());
        CollModTimeseries collModTs;
        collModTs.setBucketRoundingSeconds(rounding);
        collModTs.setBucketMaxSpanSeconds(maxSpan);
        CollModRequest collModReq;
        collModReq.setTimeseries(collModTs);

        updateTimeseriesBucketingParameters(collModReq);

        ASSERT_FALSE(getOptionalGranularity());
        const auto roundingAndMaxSpan = getOptionalRoundingAndMaxSpan();
        ASSERT_EQ(roundingAndMaxSpan.first, rounding);
        ASSERT_EQ(roundingAndMaxSpan.second, maxSpan);
    }

    /* Set only granularity works */
    {
        CollModTimeseries collModTs;
        collModTs.setGranularity(BucketGranularityEnum::Seconds);
        CollModRequest collModReq;
        collModReq.setTimeseries(collModTs);

        updateTimeseriesBucketingParameters(collModReq);

        const auto optGranularity = getOptionalGranularity();
        ASSERT_TRUE(optGranularity.has_value());
        ASSERT_EQ(*optGranularity, BucketGranularityEnum::Seconds);
        const auto roundingAndMaxSpan = getOptionalRoundingAndMaxSpan();
        ASSERT_FALSE(roundingAndMaxSpan.first.has_value());
        ASSERT_TRUE(roundingAndMaxSpan.second.has_value());
    }

    /* Set multiple flags works: rounding, max span and mixed schema */
    {
        const auto rounding = 12345678, maxSpan = 87654;
        ASSERT_FALSE(getMixedSchemaFlag());
        ASSERT_TRUE(getOptionalGranularity());

        CollModTimeseries collModTs;
        collModTs.setBucketRoundingSeconds(rounding);
        collModTs.setBucketMaxSpanSeconds(maxSpan);
        CollModRequest collModReq;
        collModReq.setTimeseries(collModTs);
        collModReq.setTimeseriesBucketsMayHaveMixedSchemaData(true);

        updateTimeseriesBucketingParameters(collModReq);

        ASSERT_TRUE(getMixedSchemaFlag());
        ASSERT_FALSE(getOptionalGranularity());
        const auto roundingAndMaxSpan = getOptionalRoundingAndMaxSpan();
        ASSERT_EQ(roundingAndMaxSpan.first, rounding);
        ASSERT_EQ(roundingAndMaxSpan.second, maxSpan);
    }

    // Major version should have increased as many times as `updateTimeSeriesBucketingParameters`
    // was invoked
    const auto latestChunkVersion = getChunk().getVersion();
    ASSERT_EQ(latestChunkVersion.majorVersion(), expectedMajorVersion);
}

}  // namespace
}  // namespace mongo
