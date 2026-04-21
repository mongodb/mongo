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


#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_test_util.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const ShardId kShard0{"shard0"};
const ShardId kShard1{"shard1"};
const std::vector<ChunkHistory> kShard0History{ChunkHistory(Timestamp(1, 0), kShard0)};
const std::vector<ChunkHistory> kShard1History{ChunkHistory(Timestamp(1, 0), kShard1)};

ChunkType generateChunk(const UUID& collUuid,
                        const ShardId& shardId,
                        const BSONObj& minKey,
                        const BSONObj& maxKey,
                        const std::vector<ChunkHistory>& history,
                        const OID& epoch = OID::gen()) {
    ChunkType chunkType;
    chunkType.setName(OID::gen());
    chunkType.setCollectionUUID(collUuid);
    chunkType.setVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}));
    chunkType.setShard(shardId);
    chunkType.setRange({minKey, maxKey});
    chunkType.setOnCurrentShardSince(Timestamp(1, 0));
    chunkType.setHistory(history);
    return chunkType;
}

TagsType generateZone(const NamespaceString& nss, const BSONObj& minKey, const BSONObj& maxKey) {
    TagsType tagType;
    tagType.setTag(OID::gen().toString());
    tagType.setNS(nss);
    tagType.setRange({minKey, maxKey});
    return tagType;
}

TimeseriesOptions generateTimeseriesOptions(
    const std::string& timeField,
    const boost::optional<StringData> metaField = boost::none,
    const boost::optional<BucketGranularityEnum> granularity = boost::none,
    const boost::optional<int32_t>& bucketMaxSpanSeconds = boost::none,
    const boost::optional<int32_t>& bucketRoundingSeconds = boost::none) {
    TimeseriesOptions options{timeField};
    options.setMetaField(metaField);
    options.setGranularity(granularity);
    options.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);
    options.setBucketRoundingSeconds(bucketRoundingSeconds);
    return options;
}

std::pair<const std::shared_ptr<const CollectionCatalog>, std::vector<CollectionPtr>>
getLocalCatalog(OperationContext* opCtx, const NamespaceString& nss) {
    std::vector<CollectionPtr> localCatalogCollections;
    auto collCatalogSnapshot = [&] {
        AutoGetCollection coll(opCtx,
                               nss,
                               MODE_IS,
                               auto_get_collection::Options{}.viewMode(
                                   auto_get_collection::ViewMode::kViewsPermitted));
        return CollectionCatalog::get(opCtx);
    }();

    if (auto coll = collCatalogSnapshot->lookupCollectionByNamespace(opCtx, nss)) {
        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        localCatalogCollections.emplace_back(CollectionPtr::CollectionPtr_UNSAFE(coll));
    }
    return {collCatalogSnapshot, std::move(localCatalogCollections)};
}

class MetadataConsistencyTest : public ShardServerTestFixture {
protected:
    const ShardId _shardId = kShard0;
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    const DatabaseName _dbName = _nss.dbName();
    const UUID _collUuid = UUID::gen();
    const UUID _dbUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};
    const CollectionType _coll{
        _nss, OID::gen(), Timestamp(1), Date_t::now(), _collUuid, _keyPattern};

    CollectionType generateCollectionType(const NamespaceString& nss,
                                          const UUID& uuid,
                                          const KeyPattern& keyPattern = KeyPattern(BSON("_id"
                                                                                         << 1))) {
        return CollectionType{nss, OID::gen(), Timestamp(1), Date_t::now(), uuid, keyPattern};
    }

    void updateConfigChunks(const std::vector<ChunkType>& chunks) {
        DBDirectClient client(operationContext());

        // Remove all chunks
        const auto resRemove = client.remove(write_ops::DeleteCommandRequest{
            NamespaceString::kConfigsvrChunksNamespace,
            {write_ops::DeleteOpEntry{BSONObj(), true /* multi */}}});
        write_ops::checkWriteErrors(resRemove);

        // Insert the given chunks
        if (chunks.size()) {
            std::vector<BSONObj> docs;
            for (auto& chunk : chunks) {
                docs.emplace_back(chunk.toConfigBSON());
            }
            const auto resInsert = client.insert(
                write_ops::InsertCommandRequest{NamespaceString::kConfigsvrChunksNamespace, docs});
            write_ops::checkWriteErrors(resInsert);
        }
    }

    void assertOneInconsistencyFound(
        const MetadataInconsistencyTypeEnum& type,
        const std::vector<MetadataInconsistencyItem>& inconsistencies) {
        ASSERT_EQ(1, inconsistencies.size());
        ASSERT_EQ(type, inconsistencies[0].getType());
    }

    void assertCollectionOptionsMismatchInconsistencyFound(
        const std::vector<MetadataInconsistencyItem>& inconsistencies,
        const BSONObj& localOptions,
        const BSONObj& configOptions) {
        ASSERT_GT(inconsistencies.size(), 0);
        ASSERT_TRUE(std::any_of(
            inconsistencies.begin(), inconsistencies.end(), [&](const auto& inconsistency) {
                if (inconsistency.getType() !=
                    MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch) {
                    return false;
                }

                const auto& allOptions = inconsistency.getDetails().getField("options").Array();
                if (std::none_of(allOptions.begin(), allOptions.end(), [&](const BSONElement& o) {
                        return localOptions.woCompare(o.Obj().getField("options").Obj()) == 0;
                    })) {
                    return false;
                }
                if (std::none_of(allOptions.begin(), allOptions.end(), [&](const BSONElement& o) {
                        return configOptions.woCompare(o.Obj().getField("options").Obj()) == 0;
                    })) {
                    return false;
                }
                return true;
            }));
    }

    void assertNoCollectionOptionsMismatchInconsistencyFound(
        const std::vector<MetadataInconsistencyItem>& inconsistencies) {
        ASSERT_TRUE(std::none_of(
            inconsistencies.begin(), inconsistencies.end(), [](const auto& inconsistency) {
                return inconsistency.getType() ==
                    MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch;
            }));
    }
};

class MetadataConsistencyConfigTest : public MetadataConsistencyTest {
protected:
    void setUp() override {
        // The ShardingState must be set to 'config' to be able to call
        // metadata_consistency_util::checkChunksConsistency()
        kMyShardName = ShardId::kConfigServerId;
        MetadataConsistencyTest::setUp();
    }
};

TEST_F(MetadataConsistencyConfigTest, FindRoutingTableRangeGapInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("x" << 1),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kRoutingTableRangeGap,
                                inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindMissingChunkWithMaxKeyInconsistency) {
    const auto chunk = generateChunk(_collUuid,
                                     _shardId,
                                     _keyPattern.globalMin(),
                                     BSON("x" << 0),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey,
                                inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindMissingChunkWithMinKeyInconsistency) {
    const auto chunk = generateChunk(_collUuid,
                                     _shardId,
                                     BSON("x" << 0),
                                     _keyPattern.globalMax(),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey,
                                inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindRoutingTableRangeOverlapInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("x" << -10),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap,
                                inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindCorruptedChunkShardKeyInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("y" << 0),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    ASSERT_EQ(2, inconsistencies.size());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey, inconsistencies[0].getType());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kRoutingTableRangeGap, inconsistencies[1].getType());
}

TEST_F(MetadataConsistencyConfigTest, FindCorruptedZoneShardKeyInconsistency) {
    const auto zone1 = generateZone(_nss, _keyPattern.globalMin(), BSON("x" << 0));

    const auto zone2 = generateZone(_nss, BSON("y" << 0), _keyPattern.globalMax());

    const auto inconsistencies =
        metadata_consistency_util::checkZonesConsistency(operationContext(), _coll, {zone1, zone2});

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kCorruptedZoneShardKey,
                                inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindZoneRangeOverlapInconsistency) {
    const auto zone1 = generateZone(_nss, _keyPattern.globalMin(), BSON("x" << 0));

    const auto zone2 = generateZone(_nss, BSON("x" << -10), _keyPattern.globalMax());

    const auto inconsistencies =
        metadata_consistency_util::checkZonesConsistency(operationContext(), _coll, {zone1, zone2});

    assertOneInconsistencyFound(MetadataInconsistencyTypeEnum::kZonesRangeOverlap, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindCorruptedShardKeyInconsistencyForUnsplittableCollection) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestCollUnsplittable");
    const UUID collUuid = UUID::gen();
    const KeyPattern keyPattern{BSON("x" << 1)};
    CollectionType coll{nss, OID::gen(), Timestamp(1), Date_t::now(), collUuid, keyPattern};
    coll.setUnsplittable(true);
    const auto inconsistencies =
        metadata_consistency_util::checkCollectionShardingMetadataConsistency(operationContext(),
                                                                              coll);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasInvalidKey, inconsistencies);
}

TEST_F(MetadataConsistencyTest, CappedAndShardedCollection) {
    OperationContext* opCtx = operationContext();

    // Create a capped local collection.
    CreateCommand cmd(_nss);
    cmd.getCreateCollectionRequest().setCapped(true);
    cmd.getCreateCollectionRequest().setSize(100);
    createTestCollection(opCtx, _nss, cmd.toBSON());

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    // Create a CollectionType for a non-unsplittable collection to mock the collection info
    // fetched from the config server.
    auto configColl = generateCollectionType(_nss, localCatalogCollections[0]->uuid());

    // Catch the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);
    assertCollectionOptionsMismatchInconsistencyFound(
        inconsistencies,
        BSON("capped" << true),
        BSON("capped" << false << "unsplittable" << false));
}

TEST_F(MetadataConsistencyTest, DefaultCollationMismatchBetweenLocalAndShardingCatalog) {
    OperationContext* opCtx = operationContext();

    auto convertToCatalogCollationBSON = [&opCtx](const Collation& collation) {
        // The collation sent to the create cmd is slightly different to the one stored in the
        // Catalog.
        const auto collator =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(collation.toBSON()));
        if (!collator) {
            // Case when collation is {locale: 'simple'}
            return BSONObj();
        }
        return collator->getSpec().toBSON();
    };

    const auto testCollationMismatch = [&](const NamespaceString& nss,
                                           const boost::optional<Collation>& localCollation,
                                           const boost::optional<Collation>& configCollation,
                                           bool expectInconsistencies) {
        // Create a local collection with the given collation.
        CreateCommand cmd(nss);
        if (localCollation) {
            cmd.getCreateCollectionRequest().setCollation(*localCollation);
        }
        createTestCollection(opCtx, nss, cmd.toBSON());

        const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, nss);
        ASSERT_EQ(1, localCatalogCollections.size());

        // Create a CollectionType to mock the collection metadata fetched from the config server.
        auto configColl = generateCollectionType(nss, localCatalogCollections[0]->uuid());
        if (configCollation) {
            configColl.setDefaultCollation(convertToCatalogCollationBSON(*configCollation));
        }

        // Check the inconsistencies.
        const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
            opCtx,
            _shardId,
            _shardId,
            {configColl},
            localCatalogSnapshot,
            localCatalogCollections,
            false /*checkRangeDeletionIndexes*/,
            false /*optionalCheckIndexes*/);

        if (expectInconsistencies) {
            BSONObj collationLocalCatalog =
                localCollation ? convertToCatalogCollationBSON(*localCollation) : BSONObj();
            assertCollectionOptionsMismatchInconsistencyFound(
                inconsistencies,
                BSON(CollectionType::kDefaultCollationFieldName << collationLocalCatalog),
                BSON(CollectionType::kDefaultCollationFieldName
                     << configColl.getDefaultCollation()));
        } else {
            assertNoCollectionOptionsMismatchInconsistencyFound(inconsistencies);
        }
    };

    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl1"),
                          Collation("es"),
                          Collation("ar"),
                          true);
    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl2"),
                          boost::none,
                          Collation("ar"),
                          true);
    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl3"),
                          Collation("ca"),
                          boost::none,
                          true);
    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl4"),
                          Collation("simple"),
                          Collation("simple"),
                          false);
    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl5"),
                          Collation("az"),
                          Collation("az"),
                          false);
    testCollationMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl6"),
                          boost::none,
                          boost::none,
                          false);
}

TEST_F(MetadataConsistencyTest, TimeseriesOptionsMismatchBetweenLocalAndShardingCatalog) {
    OperationContext* opCtx = operationContext();

    const auto convertToCatalogTimeseriesOptions = [](TimeseriesOptions timeseriesOptions) {
        // The TimeseriesOptions sent to the create cmd are slightly different to the ones stored in
        // the Catalog.
        uassertStatusOK(timeseries::validateAndSetBucketingParameters(timeseriesOptions));
        return timeseriesOptions;
    };

    const auto testTimeseriesMismatch =
        [&](const NamespaceString& nss,
            const boost::optional<TimeseriesOptions>& localTimeseries,
            const boost::optional<TimeseriesOptions>& configTimeseries,
            bool expectInconsistencies) {
            // Create a local collection with the given collation.
            CreateCommand cmd(nss);
            if (localTimeseries) {
                cmd.getCreateCollectionRequest().setTimeseries(localTimeseries);
            }
            createTestCollection(opCtx, nss, cmd.toBSON());

            auto actualNss =
                localTimeseries ? timeseries::test_util::resolveTimeseriesNss(nss) : nss;

            const auto [localCatalogSnapshot, localCatalogCollections] =
                getLocalCatalog(opCtx, actualNss);
            ASSERT_EQ(1, localCatalogCollections.size());

            // Create a CollectionType to mock the collection metadata fetched from the config
            // server.
            auto configColl = generateCollectionType(actualNss, localCatalogCollections[0]->uuid());
            if (configTimeseries) {
                TypeCollectionTimeseriesFields timeseriesFields;
                timeseriesFields.setTimeseriesOptions(
                    convertToCatalogTimeseriesOptions(*configTimeseries));
                configColl.setTimeseriesFields(std::move(timeseriesFields));
            }

            // Check the inconsistencies.
            const auto inconsistencies =
                metadata_consistency_util::checkCollectionMetadataConsistency(
                    opCtx,
                    _shardId,
                    _shardId,
                    {configColl},
                    localCatalogSnapshot,
                    localCatalogCollections,
                    false /*checkRangeDeletionIndexes*/,
                    false /*optionalCheckIndexes*/);

            if (expectInconsistencies) {
                const BSONObj& localCatalogBSON =
                    (localTimeseries ? convertToCatalogTimeseriesOptions(*localTimeseries).toBSON()
                                     : BSONObj());
                const BSONObj& configCatalogBSON = configColl.getTimeseriesFields()
                    ? configColl.getTimeseriesFields()->toBSON()
                    : BSONObj();
                assertCollectionOptionsMismatchInconsistencyFound(
                    inconsistencies,
                    BSON(CollectionType::kTimeseriesFieldsFieldName << localCatalogBSON),
                    BSON(CollectionType::kTimeseriesFieldsFieldName << configCatalogBSON));
            } else {
                assertNoCollectionOptionsMismatchInconsistencyFound(inconsistencies);
            }
        };

    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl1"),
                           boost::none,
                           generateTimeseriesOptions("time"),
                           true);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl2"),
                           generateTimeseriesOptions("time", "meta"_sd),
                           boost::none,
                           true);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl3"),
                           generateTimeseriesOptions("time"),
                           generateTimeseriesOptions("x"),
                           true);
    testTimeseriesMismatch(
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl4"),
        generateTimeseriesOptions("time", "meta"_sd, BucketGranularityEnum::Minutes),
        generateTimeseriesOptions("time", "metaDiff"_sd, BucketGranularityEnum::Minutes),
        true);
    testTimeseriesMismatch(
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl5"),
        generateTimeseriesOptions("time", "meta"_sd, BucketGranularityEnum::Minutes),
        generateTimeseriesOptions("time", "meta"_sd, BucketGranularityEnum::Hours),
        true);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl6"),
                           generateTimeseriesOptions("time", "meta"_sd, boost::none, 111, 111),
                           generateTimeseriesOptions("time", "meta"_sd, boost::none, 222, 222),
                           true);
    testTimeseriesMismatch(
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl7"),
        generateTimeseriesOptions("time", "meta"_sd, BucketGranularityEnum::Hours),
        generateTimeseriesOptions("time", "meta"_sd, BucketGranularityEnum::Hours),
        false);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl8"),
                           generateTimeseriesOptions("time", "meta"_sd, boost::none, 3333, 3333),
                           generateTimeseriesOptions("time", "meta"_sd, boost::none, 3333, 3333),
                           false);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl9"),
                           boost::none,
                           boost::none,
                           false);
    testTimeseriesMismatch(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl10"),
                           generateTimeseriesOptions("x"),
                           generateTimeseriesOptions("x"),
                           false);
}

TEST_F(MetadataConsistencyTest, FindMissingDatabaseMetadataInShardCatalogCache) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, _shardId, {_dbUuid, dbTimestamp}};

    // Mock database metadata in the shard catalog.
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  dbInGlobalCatalog.toBSON());

    // Introduce an inconsistency in the shard catalog cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->clearDbMetadata(operationContext());
    }

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalogCache,
        inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindInconsistentDatabaseVersionInShardCatalogCache) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, {_dbUuid, dbTimestamp}};

    // Mock database metadata in the shard catalog.
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  dbInGlobalCatalog.toBSON());

    // Introduce an inconsistency in the shard catalog cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(),
                                 DatabaseType{_dbName, kMyShardName, {_dbUuid, Timestamp(2, 0)}});
    }

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
        inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindEmptyDurableDatabaseMetadataInShard) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, {_dbUuid, dbTimestamp}};

    // Introduce an inconsistency in the shard catalog while mocking it in the cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(), dbInGlobalCatalog);
    }

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog, inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindInconsistentDurableDatabaseMetadataInShardWithConfig) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, {_dbUuid, dbTimestamp}};

    // Mock database metadata in the shard catalog cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(), dbInGlobalCatalog);
    }

    // Introduce an inconsistency in the shard catalog
    DBDirectClient client(operationContext());
    DatabaseType shardDb{_dbName, kMyShardName, {_dbUuid, Timestamp(2, 0)}};
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace, shardDb.toBSON());

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    ASSERT_EQ(2, inconsistencies.size());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog,
              inconsistencies[0].getType());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
              inconsistencies[1].getType());
}

TEST_F(MetadataConsistencyTest, FindMatchingDurableDatabaseMetadataInWrongShard) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, {_dbUuid, dbTimestamp}};

    // Mock database metadata in the shard catalog cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(), dbInGlobalCatalog);
    }

    // Introduce an inconsistency in the shard catalog
    DBDirectClient client(operationContext());
    DatabaseType shardDb{_dbName, ShardId{"otherShard"}, {_dbUuid, dbTimestamp}};
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace, shardDb.toBSON());

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalog, inconsistencies);
}

TEST_F(MetadataConsistencyTest, CheckDatabaseMetadataConsistency_CriticalSection) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    // Use the same database metadata for the global catalog and the shard catalog.
    Timestamp dbTimestamp{1, 0};
    DatabaseVersion dbVersion{_dbUuid, dbTimestamp};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, dbVersion};
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  dbInGlobalCatalog.toBSON());

    // Mock that the critical section is acquired in the DSS.
    {
        AutoGetDb autoDb(operationContext(), _dbName, MODE_IX);
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(operationContext(), _dbName);
        scopedDsr->enterCriticalSectionCatchUpPhase(operationContext(), BSON("reason" << "test"));
        scopedDsr->enterCriticalSectionCommitPhase(operationContext(), BSON("reason" << "test"));
    }


    // Validate that throws in case the critical section is acquired by another thread.
    ASSERT_THROWS_CODE(metadata_consistency_util::checkDatabaseMetadataConsistency(
                           operationContext(), dbInGlobalCatalog),
                       AssertionException,
                       ErrorCodes::StaleDbVersion);

    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(operationContext(), _dbName);
    scopedDsr->exitCriticalSectionNoChecks(operationContext());
}

TEST_F(MetadataConsistencyTest, FindInconsistentDurableDatabaseMetadataInShard) {
    RAIIServerParameterControllerForTest featureFlagControllerForDDL(
        "featureFlagShardAuthoritativeDbMetadataDDL", true);
    RAIIServerParameterControllerForTest featureFlagControllerForCRUD(
        "featureFlagShardAuthoritativeDbMetadataCRUD", true);

    Timestamp dbTimestamp{1, 0};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, {_dbUuid, dbTimestamp}};

    // Mock database metadata in the shard catalog cache.
    {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(), dbInGlobalCatalog);
    }

    // Introduce an inconsistency in the shard catalog
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  BSON(DatabaseType::kDbNameFieldName << _dbName.toString_forTest()));

    // Validate that we can find the inconsistency.
    const auto inconsistencies = metadata_consistency_util::checkDatabaseMetadataConsistency(
        operationContext(), dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog, inconsistencies);
    ASSERT_EQ(inconsistencies[0].getDetails().getStringField("reason"),
              "BSON field 'DatabaseType.primary' is missing but a required field");
}

TEST_F(MetadataConsistencyTest, ShardUntrackedCollectionInconsistencyTest) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(_nss, localCatalogCollections[0]->uuid());

    auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
        inconsistencies);
    ASSERT_EQ("isTracked"_sd,
              inconsistencies[0].getDetails().getObjectField("details").getStringField("field"));

    // Clear the filtering information and check that no inconsistency is reported for unknown
    // filtering information.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->clearFilteringMetadata_nonAuthoritative(opCtx);
    }
    inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);
    ASSERT_EQ(0, inconsistencies.size());
}

TEST_F(MetadataConsistencyTest, ShardTrackedCollectionInconsistencyTest) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    {
        auto chunk = generateChunk(localCatalogCollections[0]->uuid(),
                                   _shardId,
                                   _keyPattern.globalMin(),
                                   _keyPattern.globalMax(),
                                   {ChunkHistory(Timestamp(1, 0), _shardId)});
        auto rt = RoutingTableHistory::makeNew(_nss,
                                               localCatalogCollections[0]->uuid(),
                                               _keyPattern,
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               chunk.getVersion().epoch(),
                                               chunk.getVersion().getTimestamp(),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* resharding Fields */,
                                               true /* allowMigrations */,
                                               {chunk});

        const auto version = rt.getVersion();
        const auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));

        const auto collectionMetadata = CollectionMetadata(CurrentChunkManager(rtHandle), _shardId);

        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->setFilteringMetadata_nonAuthoritative(opCtx, collectionMetadata);
    }

    auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {} /* shardingCatalogCollections */,
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
        inconsistencies);
    ASSERT_EQ("isTracked"_sd,
              inconsistencies[0].getDetails().getObjectField("details").getStringField("field"));

    // Clear the filtering information and check that no inconsistency is reported for unknown
    // filtering information.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->clearFilteringMetadata_nonAuthoritative(opCtx);
    }
    inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {} /* shardingCatalogCollections */,
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);
    ASSERT_EQ(0, inconsistencies.size());
}

void assertIncompatibleUniqueIndexFound(
    const std::vector<MetadataInconsistencyItem>& inconsistencies) {
    ASSERT_TRUE(std::any_of(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
        return item.getType() ==
            MetadataInconsistencyTypeEnum::kIncompatibleUniqueIndexOnShardedCollection;
    }));
}

void assertNoIncompatibleUniqueIndexFound(
    const std::vector<MetadataInconsistencyItem>& inconsistencies) {
    ASSERT_TRUE(std::none_of(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
        return item.getType() ==
            MetadataInconsistencyTypeEnum::kIncompatibleUniqueIndexOnShardedCollection;
    }));
}

TEST_F(MetadataConsistencyTest,
       UniqueIndexWithNonSimpleCollationOnShardKeyPrefixReportsInconsistency) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    DBDirectClient client(opCtx);
    client.createIndexes(
        _nss,
        {BSON("key" << BSON("x" << 1) << "name"
                    << "x_1_en_unique"
                    << "unique" << true << "collation" << BSON("locale" << "en"))});

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        true /*optionalCheckIndexes*/);

    assertIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest, NonUniqueIndexWithNonSimpleCollationDoesNotReportInconsistency) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_en"
                                     << "collation" << BSON("locale" << "en"))});

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        true /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest, UniqueIndexWithSimpleCollationDoesNotReportInconsistency) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_unique"
                                     << "unique" << true)});

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        true /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest,
       UniqueIndexWithNonSimpleCollationNotReportedWhenOptionalCheckIndexesFalse) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    DBDirectClient client(opCtx);
    client.createIndexes(
        _nss,
        {BSON("key" << BSON("x" << 1) << "name"
                    << "x_1_en_unique"
                    << "unique" << true << "collation" << BSON("locale" << "en"))});

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest, UniqueIndexWithNonSimpleCollationAllowedInUnsplittableCollections) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    DBDirectClient client(opCtx);
    client.createIndexes(
        _nss,
        {BSON("key" << BSON("x" << 1) << "name"
                    << "x_1_en_unique"
                    << "unique" << true << "collation" << BSON("locale" << "en"))});

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("_id" << 1)));
    configColl.setUnsplittable(true);

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        true /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

class MetadataConsistencyRandomRoutingTableTest : public MetadataConsistencyConfigTest {
protected:
    inline const static auto _shards = std::vector<ShardId>{kShard0, kShard1};
    PseudoRandom _random{SecureRandom().nextInt64()};

    std::vector<ChunkType> getRandomRoutingTable() {
        int numChunks = _random.nextInt32(9) + 1;  // minimum 1 chunks, maximum 10 chunks
        std::vector<ChunkType> chunks;

        // Loop generating random routing table: [MinKey, 0), [1, 2), [2, 3), ... [x, MaxKey]
        int nextMin;
        for (int nextMax = 0; nextMax < numChunks; nextMax++) {
            auto randomShard = _shards.at(_random.nextInt64(_shards.size()));
            // set min as `MinKey` during first iteration, otherwise next min
            auto min = nextMax == 0 ? _keyPattern.globalMin() : BSON("x" << nextMin);
            // set max as `MaxKey` during last iteration, otherwise next max
            auto max = nextMax == numChunks - 1 ? _keyPattern.globalMax() : BSON("x" << nextMax);
            auto chunk = generateChunk(
                _collUuid, randomShard, min, max, {ChunkHistory(Timestamp(1, 0), randomShard)});
            nextMin = nextMax;
            chunks.push_back(chunk);
        }

        return chunks;
    };
};

/*
 * Test function to check the correct behaviour of finding range gaps inconsistencies with random
 * ranges. In order to introduce inconsistencies, a random number of chunks are removed from the the
 * routing table to create range gaps.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeGapInconsistency) {
    auto chunks = getRandomRoutingTable();
    updateConfigChunks(chunks);

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);
    ASSERT_EQ(0, inconsistencies.size());

    // Remove randoms chunk from the routing table
    const auto kNumberOfChunksToRemove = _random.nextInt64(chunks.size()) + 1;
    for (int i = 0; i < kNumberOfChunksToRemove; i++) {
        const auto itChunkToRemove = _random.nextInt64(chunks.size());
        chunks.erase(chunks.begin() + itChunkToRemove);
    }
    updateConfigChunks(chunks);

    inconsistencies = metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    // Assert that there is at least one gap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        for (const auto& inconsistency : inconsistencies) {
            const auto type = inconsistency.getType();
            ASSERT_TRUE(type == MetadataInconsistencyTypeEnum::kRoutingTableRangeGap ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey ||
                        type == MetadataInconsistencyTypeEnum::kMissingRoutingTable);
        }
    } catch (...) {
        LOGV2_INFO(7424600,
                   "Expecting gap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

/*
 * Test function to check the correct behaviour of finding range overlaps inconsistencies with
 * random ranges. In order to introduce inconsistencies, one chunk is randomly selected and its max
 * or min bound is set to a random bigger or lower value, respectively.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeOverlapInconsistency) {
    auto chunks = getRandomRoutingTable();
    updateConfigChunks(chunks);

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);
    ASSERT_EQ(0, inconsistencies.size());

    // If there is only one chunk, we can't introduce an overlap
    if (chunks.size() == 1) {
        return;
    }

    const auto chunkIdx = static_cast<size_t>(_random.nextInt64(chunks.size()));
    auto& chunk = chunks.at(chunkIdx);

    auto overlapMax = [&]() {
        auto newMax = [&] {
            if (_random.nextInt64(10) == 0) {
                // With 1/10 probability, set max to MaxKey
                return _keyPattern.globalMax();
            } else {
                // Otherwise, set max to a random value bigger than actual
                auto max = chunk.getMax()["x"].numberInt();
                return BSON("x" << max + _random.nextInt64(10) + 1);
            }
        }();

        chunk.setRange({chunk.getMin(), newMax});
    };

    auto overlapMin = [&]() {
        auto newMin = [&] {
            if (_random.nextInt64(10) == 0) {
                // With 1/10 probability, set min to MinKey
                return _keyPattern.globalMin();
            } else {
                // Otherwise, set min to a random value smaller than actual
                auto min = chunk.getMin()["x"].numberInt();
                return BSON("x" << min - _random.nextInt64(10) - 1);
            }
        }();
        chunk.setRange({newMin, chunk.getMax()});
    };

    if (chunkIdx == 0) {
        overlapMax();
    } else if (chunkIdx == (chunks.size() - 1)) {
        overlapMin();
    } else {
        // With 1/2 probability, overlap min or max
        if (_random.nextInt64(2) == 0) {
            overlapMin();
        } else {
            overlapMax();
        }
    }

    updateConfigChunks(chunks);
    inconsistencies = metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    // Assert that there is at least one overlap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        ASSERT_TRUE(std::any_of(
            inconsistencies.begin(), inconsistencies.end(), [](const auto& inconsistency) {
                return inconsistency.getType() ==
                    MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap;
            }));
    } catch (...) {
        LOGV2_INFO(7424601,
                   "Expecting overlap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

class CatalogClientWithChunks : public ShardingCatalogClientMock {
public:
    void setChunksToReturn(std::vector<ChunkType> chunks) {
        _chunks = std::move(chunks);
    }

    StatusWith<std::vector<ChunkType>> getChunks(OperationContext* opCtx,
                                                 const BSONObj& filter,
                                                 const BSONObj& sort,
                                                 boost::optional<int> limit,
                                                 repl::OpTime* opTime,
                                                 const OID& epoch,
                                                 const Timestamp& timestamp,
                                                 repl::ReadConcernLevel readConcern,
                                                 const boost::optional<BSONObj>& hint) override {
        return _chunks;
    }

private:
    std::vector<ChunkType> _chunks;
};

class MetadataConsistencyShardCatalogTest : public MetadataConsistencyTest {
protected:
    void setUp() override {
        MetadataConsistencyTest::setUp();
        _catalogClient =
            dynamic_cast<CatalogClientWithChunks*>(Grid::get(operationContext())->catalogClient());
        invariant(_catalogClient);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<CatalogClientWithChunks>();
    }

    UUID setUpLocalCollection() {
        createTestCollection(operationContext(), _nss);
        std::tie(_localCatalogSnapshot, _localCatalogCollections) =
            getLocalCatalog(operationContext(), _nss);
        ASSERT_EQ(1, _localCatalogCollections.size());
        return _localCatalogCollections[0]->uuid();
    }

    void setAuthoritativeShardCatalogMetadata(const UUID& uuid,
                                              const KeyPattern& keyPattern,
                                              const std::vector<ChunkType>& chunks) {
        auto rt = RoutingTableHistory::makeNewAllowingGaps(_nss,
                                                           uuid,
                                                           keyPattern,
                                                           false,
                                                           nullptr,
                                                           false,
                                                           chunks[0].getVersion().epoch(),
                                                           chunks[0].getVersion().getTimestamp(),
                                                           boost::none,
                                                           boost::none,
                                                           true,
                                                           chunks);
        const auto version = rt.getVersion();
        const auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
        const auto collectionMetadata = CollectionMetadata(CurrentChunkManager(rtHandle), _shardId);
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        scopedCSR->setFilteringMetadata_authoritative(operationContext(), collectionMetadata);
    }

    void setShardCatalogMetadata(const UUID& uuid,
                                 const KeyPattern& keyPattern,
                                 const std::vector<ChunkType>& chunks) {
        auto rt = RoutingTableHistory::makeNew(_nss,
                                               uuid,
                                               keyPattern,
                                               false,
                                               nullptr,
                                               false,
                                               chunks[0].getVersion().epoch(),
                                               chunks[0].getVersion().getTimestamp(),
                                               boost::none,
                                               boost::none,
                                               true,
                                               chunks);
        const auto version = rt.getVersion();
        const auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
        const auto collectionMetadata = CollectionMetadata(CurrentChunkManager(rtHandle), _shardId);
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        scopedCSR->setFilteringMetadata_nonAuthoritative(operationContext(), collectionMetadata);
    }

    std::vector<MetadataInconsistencyItem> checkConsistency(
        const CollectionType& globalCatalogColl) {
        return metadata_consistency_util::checkCollectionMetadataConsistency(
            operationContext(),
            _shardId,
            _shardId,
            {globalCatalogColl},
            _localCatalogSnapshot,
            _localCatalogCollections,
            false /*checkRangeDeletionIndexes*/,
            false /*optionalCheckIndexes*/);
    }

    size_t countInconsistenciesWithDetailField(
        const std::vector<MetadataInconsistencyItem>& inconsistencies, StringData fieldValue) {
        return std::count_if(
            inconsistencies.begin(),
            inconsistencies.end(),
            [&fieldValue](const auto& inconsistency) {
                if (inconsistency.getType() !=
                    MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata) {
                    return false;
                }
                return inconsistency.getDetails().getObjectField("details").getStringField(
                           "field") == fieldValue;
            });
    }

    size_t countInconsistenciesWithReasonField(
        const std::vector<MetadataInconsistencyItem>& inconsistencies) {
        return std::count_if(
            inconsistencies.begin(), inconsistencies.end(), [](const auto& inconsistency) {
                if (inconsistency.getType() !=
                    MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata) {
                    return false;
                }
                return inconsistency.getDetails().getObjectField("details").hasField("reason");
            });
    }

    size_t countInconsistenciesWithDetailFieldAndSource(
        const std::vector<MetadataInconsistencyItem>& inconsistencies,
        StringData fieldValue,
        StringData sourceValue) {
        return std::count_if(
            inconsistencies.begin(), inconsistencies.end(), [&](const auto& inconsistency) {
                if (inconsistency.getType() !=
                    MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata) {
                    return false;
                }
                auto details = inconsistency.getDetails().getObjectField("details");
                return details.getStringField("field") == fieldValue &&
                    details.getStringField("source") == sourceValue;
            });
    }

    void setCSRAuthoritative() {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        auto metadata = scopedCSR->getCurrentMetadataIfKnown();
        if (metadata) {
            scopedCSR->setFilteringMetadata_authoritative(operationContext(), *metadata);
        } else {
            scopedCSR->clearFilteringMetadata_authoritative(operationContext());
        }
    }

    void insertDurableShardCatalogCollection(const CollectionType& coll) {
        DBDirectClient client(operationContext());
        auto res = client.insert(write_ops::InsertCommandRequest{
            NamespaceString::kConfigShardCatalogCollectionsNamespace, {coll.toBSON()}});
        write_ops::checkWriteErrors(res);
    }

    void insertDurableShardCatalogChunks(const std::vector<ChunkType>& chunks) {
        DBDirectClient client(operationContext());
        std::vector<BSONObj> docs;
        for (const auto& chunk : chunks) {
            docs.emplace_back(chunk.toConfigBSON());
        }
        auto res = client.insert(write_ops::InsertCommandRequest{
            NamespaceString::kConfigShardCatalogChunksNamespace, docs});
        write_ops::checkWriteErrors(res);
    }

    void clearDurableShardCatalog() {
        DBDirectClient client(operationContext());
        client.remove(write_ops::DeleteCommandRequest{
            NamespaceString::kConfigShardCatalogCollectionsNamespace,
            {write_ops::DeleteOpEntry{BSONObj(), true}}});
        client.remove(
            write_ops::DeleteCommandRequest{NamespaceString::kConfigShardCatalogChunksNamespace,
                                            {write_ops::DeleteOpEntry{BSONObj(), true}}});
    }

    CatalogClientWithChunks* _catalogClient = nullptr;

private:
    std::shared_ptr<const CollectionCatalog> _localCatalogSnapshot;
    std::vector<CollectionPtr> _localCatalogCollections;
};

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_AllMatch) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    _catalogClient->setChunksToReturn({chunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "uuid"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "shardKeyPattern"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_UuidMismatch) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    const auto differentUuid = UUID::gen();
    auto shardCatalogChunk = generateChunk(
        differentUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(differentUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "shardCatalogEntry"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_ShardKeyMismatch) {
    const auto localUuid = setUpLocalCollection();
    const KeyPattern globalCatalogKeyPattern{BSON("y" << 1)};
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, globalCatalogKeyPattern);

    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk = generateChunk(localUuid,
                                            _shardId,
                                            globalCatalogKeyPattern.globalMin(),
                                            globalCatalogKeyPattern.globalMax(),
                                            kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "shardCatalogEntry"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_UuidAndShardKeyMismatch) {
    const auto localUuid = setUpLocalCollection();
    const KeyPattern globalCatalogKeyPattern{BSON("y" << 1)};
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, globalCatalogKeyPattern);

    const auto differentUuid = UUID::gen();
    auto shardCatalogChunk = generateChunk(
        differentUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk = generateChunk(localUuid,
                                            _shardId,
                                            globalCatalogKeyPattern.globalMin(),
                                            globalCatalogKeyPattern.globalMax(),
                                            kShard0History);

    setShardCatalogMetadata(differentUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "shardCatalogEntry"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_SplitChunksSameDomain) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk1 =
        generateChunk(localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 0), kShard0History);
    auto globalCatalogChunk2 =
        generateChunk(localUuid, _shardId, BSON("x" << 0), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk1, globalCatalogChunk2});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_ChunkDomainMismatch_FoundChunksExhausted) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: [MinKey, MaxKey), global catalog: [MinKey, 0).
    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk =
        generateChunk(localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 0), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_ChunkDomainMismatch_MinBoundary) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: [MinKey, MaxKey), global catalog: [{x:5}, MaxKey).
    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk =
        generateChunk(localUuid, _shardId, BSON("x" << 5), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_ChunkDomainMismatch_GapInExpectedChunks) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: [MinKey, MaxKey), global catalog: [MinKey, 10) + [20, MaxKey).
    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 10), kShard0History);
    auto globalCatalogChunk2 = generateChunk(
        localUuid, _shardId, BSON("x" << 20), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk1, globalCatalogChunk2});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_ChunkDomainMismatch_GapInShardCatalogChunks) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: this shard owns [MinKey, 10) + [20, MaxKey) with kShard1 owning [10, 20).
    // Global catalog says this shard owns the entire [MinKey, MaxKey).
    const OID epoch = OID::gen();
    auto rtChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 10), kShard0History, epoch);
    auto rtChunk2 =
        generateChunk(localUuid, kShard1, BSON("x" << 10), BSON("x" << 20), kShard1History, epoch);
    auto rtChunk3 = generateChunk(
        localUuid, _shardId, BSON("x" << 20), _keyPattern.globalMax(), kShard0History, epoch);
    auto globalCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {rtChunk1, rtChunk2, rtChunk3});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_NotOwnedChunksDisallowed_DurableAuthoritativeShardCatalogChunks) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    const OID epoch = OID::gen();
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 10), kShard0History, epoch);
    setAuthoritativeShardCatalogMetadata(localUuid, _keyPattern, {chunk});

    _catalogClient->setChunksToReturn({chunk});
    insertDurableShardCatalogCollection(globalCatalogColl);

    auto foreignChunk =
        generateChunk(localUuid, kShard1, BSON("x" << 10), BSON("x" << 20), kShard1History, epoch);
    insertDurableShardCatalogChunks({chunk, foreignChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "chunkHistory"_sd, "durableShardCatalog"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_ChunkDomainMismatch_ExtraGlobalCatalogChunks) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: this shard owns [MinKey, 0), kShard1 owns [0, MaxKey).
    // Global catalog: this shard owns [MinKey, MaxKey).
    const OID epoch = OID::gen();
    auto rtChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 0), kShard0History, epoch);
    auto rtChunk2 = generateChunk(
        localUuid, kShard1, BSON("x" << 0), _keyPattern.globalMax(), kShard1History, epoch);
    auto globalCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {rtChunk1, rtChunk2});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_MultipleSplitChunksBothSides_SameDomain) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: [MinKey, 5) + [5, MaxKey) both owned by this shard.
    // Global catalog: [MinKey, 10) + [10, MaxKey) both owned by this shard.
    // Both cover [MinKey, MaxKey) but with different split points.
    const OID epoch = OID::gen();
    auto shardChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 5), kShard0History, epoch);
    auto shardChunk2 = generateChunk(
        localUuid, _shardId, BSON("x" << 5), _keyPattern.globalMax(), kShard0History, epoch);
    auto globalChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 10), kShard0History);
    auto globalChunk2 = generateChunk(
        localUuid, _shardId, BSON("x" << 10), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardChunk1, shardChunk2});
    _catalogClient->setChunksToReturn({globalChunk1, globalChunk2});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_EmptyGlobalCatalogChunks) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    auto shardCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    // When the global catalog returns no chunks for this shard, the shard catalog still has chunks,
    // so a chunksDomain mismatch should be reported (extraShardCatalogChunks).
    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_MaxBoundaryMismatch) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Shard catalog: this shard owns [MinKey, 10), kShard1 owns [10, MaxKey).
    // Global catalog: this shard owns [MinKey, 20).
    const OID epoch = OID::gen();
    auto rtChunk1 = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 10), kShard0History, epoch);
    auto rtChunk2 = generateChunk(
        localUuid, kShard1, BSON("x" << 10), _keyPattern.globalMax(), kShard1History, epoch);
    auto globalCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 20), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {rtChunk1, rtChunk2});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, ValidateCollectionMetadata_SkipsWhenMetadataUnknown) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Set up tracked metadata then clear it to make it unknown.
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});

    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        scopedCSR->clearFilteringMetadata_nonAuthoritative(operationContext());
    }

    _catalogClient->setChunksToReturn({chunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    // When metadata is unknown, no shard catalog inconsistencies should be reported.
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "uuid"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "shardKeyPattern"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "isTracked"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_SkipsWhenCriticalSectionActive) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // Intentionally use a different UUID in shard catalog to create a detectable mismatch.
    const auto differentUuid = UUID::gen();
    auto shardCatalogChunk = generateChunk(
        differentUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    auto globalCatalogChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    setShardCatalogMetadata(differentUuid, _keyPattern, {shardCatalogChunk});
    _catalogClient->setChunksToReturn({globalCatalogChunk});

    // Acquire the critical section to simulate a migration in progress.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        scopedCSR->enterCriticalSectionCatchUpPhase(operationContext(), BSON("reason" << "test"));
        scopedCSR->enterCriticalSectionCommitPhase(operationContext(), BSON("reason" << "test"));
    }

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    // No shard catalog inconsistencies should be reported because the critical section is active.
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "uuid"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "shardKeyPattern"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "isTracked"_sd));

    // Clean up the critical section.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(operationContext(), _nss);
        scopedCSR->exitCriticalSectionNoChecks(operationContext());
    }
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_DurablePathGuardedByFeatureFlag) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", false);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    _catalogClient->setChunksToReturn({chunk});

    // Both catalogs are consistent, and the durable shard catalog is intentionally left empty.
    // With the feature flag disabled, the durable path should be skipped entirely, and the
    // in-memory path should find no issues.
    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "uuid"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "shardKeyPattern"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "isTracked"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_AllMatch) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();

    insertDurableShardCatalogCollection(globalCatalogColl);
    insertDurableShardCatalogChunks({chunk});
    _catalogClient->setChunksToReturn({chunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "uuid"_sd, "durableShardCatalog"_sd));
    ASSERT_EQ(0,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "shardKeyPattern"_sd, "durableShardCatalog"_sd));
    ASSERT_EQ(0,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "chunksDomain"_sd, "durableShardCatalog"_sd));
    ASSERT_EQ(0, countInconsistenciesWithReasonField(inconsistencies));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_UuidMismatch) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();
    _catalogClient->setChunksToReturn({chunk});

    // Insert a durable shard catalog collection with a DIFFERENT UUID.
    const auto differentUuid = UUID::gen();
    auto durableColl = generateCollectionType(_nss, differentUuid, _keyPattern);
    insertDurableShardCatalogCollection(durableColl);
    auto durableChunk = generateChunk(
        differentUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    insertDurableShardCatalogChunks({durableChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "shardCatalogEntry"_sd, "durableShardCatalog"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_ShardKeyMismatch) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();
    _catalogClient->setChunksToReturn({chunk});

    // Insert a durable shard catalog collection with a DIFFERENT shard key.
    const KeyPattern differentKeyPattern{BSON("y" << 1)};
    auto durableColl = generateCollectionType(_nss, localUuid, differentKeyPattern);
    insertDurableShardCatalogCollection(durableColl);
    auto durableChunk = generateChunk(localUuid,
                                      _shardId,
                                      differentKeyPattern.globalMin(),
                                      differentKeyPattern.globalMax(),
                                      kShard0History);
    insertDurableShardCatalogChunks({durableChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "shardCatalogEntry"_sd, "durableShardCatalog"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_ChunksDomainMismatch) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();
    _catalogClient->setChunksToReturn({chunk});

    // Insert a durable shard catalog with a shorter chunk range (domain mismatch).
    insertDurableShardCatalogCollection(globalCatalogColl);
    auto durableChunk =
        generateChunk(localUuid, _shardId, _keyPattern.globalMin(), BSON("x" << 0), kShard0History);
    insertDurableShardCatalogChunks({durableChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "chunksDomain"_sd, "durableShardCatalog"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_MissingCollectionInDurableCatalog) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();
    _catalogClient->setChunksToReturn({chunk});

    // Intentionally leave the durable shard catalog empty (no collection entry).
    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithReasonField(inconsistencies));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_MissingChunksInDurableCatalog) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    setCSRAuthoritative();
    _catalogClient->setChunksToReturn({chunk});

    // Insert the collection entry but NO chunks in the durable catalog.
    insertDurableShardCatalogCollection(globalCatalogColl);

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    // Should report that no chunks were found in the durable shard catalog.
    ASSERT_EQ(1, countInconsistenciesWithReasonField(inconsistencies));
}

TEST_F(MetadataConsistencyShardCatalogTest, DurablePath_SkippedWhenNonAuthoritative) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagShardAuthoritativeCollMetadata", true);

    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);
    auto chunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);

    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});
    _catalogClient->setChunksToReturn({chunk});

    // Durable catalog is empty. If the durable path ran, it would report inconsistencies.
    // With non-authoritative state, the durable path should be skipped entirely.
    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0,
              countInconsistenciesWithDetailFieldAndSource(
                  inconsistencies, "uuid"_sd, "durableShardCatalog"_sd));
    ASSERT_EQ(0, countInconsistenciesWithReasonField(inconsistencies));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_SkipsValidationWhenPlacementVersionUnset) {
    // Simulate a shard that retains a routing table with stale metadata (e.g., after
    // moveCollection moved the collection away) but owns no chunks. The shard key in the shard
    // catalog ({x:1}) differs from the global catalog ({y:1}), but since the shard's placement
    // version is {0,0}, no inconsistency should be reported.
    const auto localUuid = setUpLocalCollection();
    const KeyPattern globalCatalogKeyPattern{BSON("y" << 1)};
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, globalCatalogKeyPattern);

    // All chunks on shard1, shard0 has placement version {0,0}.
    auto chunk = generateChunk(
        localUuid, kShard1, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard1History);
    setShardCatalogMetadata(localUuid, _keyPattern, {chunk});

    // Global catalog has no chunks for shard0.
    _catalogClient->setChunksToReturn({});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "shardKeyPattern"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "chunksDomain"_sd));
    ASSERT_EQ(0, countInconsistenciesWithDetailField(inconsistencies, "ownedChunks"_sd));
}

TEST_F(MetadataConsistencyShardCatalogTest,
       ValidateCollectionMetadata_OwnedChunksMismatchWhenPlacementVersionUnset) {
    const auto localUuid = setUpLocalCollection();
    auto globalCatalogColl = generateCollectionType(_nss, localUuid, _keyPattern);

    // All chunks on shard1, shard0 has placement version {0,0}.
    auto rtChunk = generateChunk(
        localUuid, kShard1, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard1History);
    setShardCatalogMetadata(localUuid, _keyPattern, {rtChunk});

    // Global catalog says shard0 owns a chunk.
    auto globalChunk = generateChunk(
        localUuid, _shardId, _keyPattern.globalMin(), _keyPattern.globalMax(), kShard0History);
    _catalogClient->setChunksToReturn({globalChunk});

    const auto inconsistencies = checkConsistency(globalCatalogColl);

    ASSERT_EQ(1, countInconsistenciesWithDetailField(inconsistencies, "ownedChunks"_sd));
}

// Tests for the `severity` field on `MetadataInconsistencyItem`.

class MakeInconsistencySeverityTest : public unittest::Test {
protected:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    const UUID _uuid = UUID::gen();

    MissingRoutingTableDetails makeDetails() {
        MissingRoutingTableDetails details;
        details.setNss(_nss);
        details.setCollectionUUID(_uuid);
        return details;
    }
};

TEST_F(MakeInconsistencySeverityTest, NoSeverityByDefault) {
    const auto item = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable, makeDetails());
    ASSERT_FALSE(item.getSeverity().has_value());
}

TEST_F(MakeInconsistencySeverityTest, SeverityHighIsPreserved) {
    const auto item = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable,
        makeDetails(),
        MetadataInconsistencySeverityEnum::kHigh);
    ASSERT_TRUE(item.getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kHigh, item.getSeverity().value());
}

TEST_F(MakeInconsistencySeverityTest, SeverityMediumIsPreserved) {
    const auto item = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable,
        makeDetails(),
        MetadataInconsistencySeverityEnum::kMedium);
    ASSERT_TRUE(item.getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kMedium, item.getSeverity().value());
}

TEST_F(MakeInconsistencySeverityTest, SeverityLowIsPreserved) {
    const auto item = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable,
        makeDetails(),
        MetadataInconsistencySeverityEnum::kLow);
    ASSERT_TRUE(item.getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kLow, item.getSeverity().value());
}

TEST_F(MakeInconsistencySeverityTest, ExplicitNoneSeverityIsAbsent) {
    const auto item = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable, makeDetails(), boost::none);
    ASSERT_FALSE(item.getSeverity().has_value());
}

TEST_F(MakeInconsistencySeverityTest, AbsentSeverityRoundTripsViaBSON) {
    const auto original = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable, makeDetails());
    const auto roundTripped = MetadataInconsistencyItem::parse(original.toBSON());
    ASSERT_FALSE(roundTripped.getSeverity().has_value());
}

TEST_F(MakeInconsistencySeverityTest, SeverityRoundTripsViaBSON) {
    const auto original = metadata_consistency_util::makeInconsistency(
        MetadataInconsistencyTypeEnum::kMissingRoutingTable,
        makeDetails(),
        MetadataInconsistencySeverityEnum::kHigh);
    const auto roundTripped = MetadataInconsistencyItem::parse(original.toBSON());
    ASSERT_TRUE(roundTripped.getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kHigh, roundTripped.getSeverity().value());
}

// Tests for low severity on config.system.sessions inconsistencies.

TEST_F(MetadataConsistencyTest, CollectionUUIDMismatchOnSessionsNamespaceHasLowSeverity) {
    OperationContext* opCtx = operationContext();
    const auto& nss = NamespaceString::kLogicalSessionsNamespace;

    createTestCollection(opCtx, nss);

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    // Use a different UUID to trigger a CollectionUUIDMismatch.
    auto configColl = generateCollectionType(nss, UUID::gen());

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);

    const auto it =
        std::find_if(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
            return item.getType() == MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch;
        });
    ASSERT_NE(it, inconsistencies.end());
    ASSERT_TRUE(it->getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kLow, it->getSeverity().value());
}

TEST_F(MetadataConsistencyTest, CollectionOptionsMismatchOnSessionsNamespaceHasLowSeverity) {
    OperationContext* opCtx = operationContext();
    const auto& nss = NamespaceString::kLogicalSessionsNamespace;

    // Create a capped local collection to trigger CollectionOptionsMismatch.
    CreateCommand cmd(nss);
    cmd.getCreateCollectionRequest().setCapped(true);
    cmd.getCreateCollectionRequest().setSize(100);
    createTestCollection(opCtx, nss, cmd.toBSON());

    const auto [localCatalogSnapshot, localCatalogCollections] = getLocalCatalog(opCtx, nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    // Config entry has the same UUID but does not mark it as unsplittable, triggering the mismatch.
    auto configColl = generateCollectionType(nss, localCatalogCollections[0]->uuid());

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogSnapshot,
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/,
        false /*optionalCheckIndexes*/);

    const auto it =
        std::find_if(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
            return item.getType() == MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch;
        });
    ASSERT_NE(it, inconsistencies.end());
    ASSERT_TRUE(it->getSeverity().has_value());
    ASSERT_EQ(MetadataInconsistencySeverityEnum::kLow, it->getSeverity().value());
}

}  // namespace
}  // namespace mongo
