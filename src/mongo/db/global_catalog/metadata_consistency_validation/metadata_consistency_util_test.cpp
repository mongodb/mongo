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
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_mock.h"
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/timeseries/timeseries_options.h"
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

ChunkType generateChunk(const UUID& collUuid,
                        const ShardId& shardId,
                        const BSONObj& minKey,
                        const BSONObj& maxKey,
                        const std::vector<ChunkHistory>& history) {
    const OID epoch = OID::gen();
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

std::vector<CollectionPtr> getLocalCatalogCollections(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    std::vector<CollectionPtr> localCatalogCollections;
    auto collCatalogSnapshot = [&] {
        AutoGetCollection coll(
            opCtx,
            nss,
            MODE_IS,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
        return CollectionCatalog::get(opCtx);
    }();

    if (auto coll = collCatalogSnapshot->lookupCollectionByNamespace(opCtx, nss)) {
        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        localCatalogCollections.emplace_back(CollectionPtr::CollectionPtr_UNSAFE(coll));
    }
    return localCatalogCollections;
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

    void assertCollectionMetadataMismatchInconsistencyFound(
        const std::vector<MetadataInconsistencyItem>& inconsistencies,
        const BSONObj& localMetadata,
        const BSONObj& configMetadata) {
        ASSERT_GT(inconsistencies.size(), 0);
        ASSERT_TRUE(std::any_of(
            inconsistencies.begin(), inconsistencies.end(), [&](const auto& inconsistency) {
                if (inconsistency.getType() !=
                    MetadataInconsistencyTypeEnum::kShardCatalogCacheCollectionMetadataMismatch) {
                    return false;
                }

                const auto& allMetadata = inconsistency.getDetails().getField("details").Array();
                if (std::none_of(allMetadata.begin(), allMetadata.end(), [&](const BSONElement& o) {
                        return localMetadata.woCompare(o.Obj().getField("metadata").Obj()) == 0;
                    })) {
                    return false;
                }
                if (std::none_of(allMetadata.begin(), allMetadata.end(), [&](const BSONElement& o) {
                        return configMetadata.woCompare(o.Obj().getField("metadata").Obj()) == 0;
                    })) {
                    return false;
                }
                return true;
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

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
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
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/);
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

        const auto localCatalogCollections = getLocalCatalogCollections(opCtx, nss);
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
            localCatalogCollections,
            false /*checkRangeDeletionIndexes*/);

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

            const auto& actualNss = (localTimeseries ? nss.makeTimeseriesBucketsNamespace() : nss);

            const auto localCatalogCollections = getLocalCatalogCollections(opCtx, actualNss);
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
                    localCatalogCollections,
                    false /*checkRangeDeletionIndexes*/);

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
        scopedDsr->clearDbMetadata();
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
}

TEST_F(MetadataConsistencyTest, ShardUntrackedCollectionInconsistencyTest) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());

    auto configColl = generateCollectionType(_nss, localCatalogCollections[0]->uuid());

    auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kShardCatalogCacheCollectionMetadataMismatch,
        inconsistencies);
    assertCollectionMetadataMismatchInconsistencyFound(
        inconsistencies, BSON("tracked" << false), BSON("tracked" << true));

    // Clear the filtering information and check that no inconsistency is reported for unknown
    // filtering information.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->clearFilteringMetadata(opCtx);
    }
    inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/);
    ASSERT_EQ(0, inconsistencies.size());
}

TEST_F(MetadataConsistencyTest, ShardTrackedCollectionInconsistencyTest) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, _nss);

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
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

        const auto collectionMetadata =
            CollectionMetadata(ChunkManager(rtHandle, boost::none), _shardId);

        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->setFilteringMetadata(opCtx, collectionMetadata);
    }

    auto inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {},
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kShardCatalogCacheCollectionMetadataMismatch,
        inconsistencies);
    assertCollectionMetadataMismatchInconsistencyFound(
        inconsistencies, BSON("tracked" << false), BSON("tracked" << true));

    // Clear the filtering information and check that no inconsistency is reported for unknown
    // filtering information.
    {
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(opCtx, _nss);
        scopedCSR->clearFilteringMetadata(opCtx);
    }
    inconsistencies = metadata_consistency_util::checkCollectionMetadataConsistency(
        opCtx,
        _shardId,
        _shardId,
        {},
        localCatalogCollections,
        false /*checkRangeDeletionIndexes*/);
    ASSERT_EQ(0, inconsistencies.size());
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

}  // namespace
}  // namespace mongo
