/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/hasher.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/shard_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const auto kSampledReadCommandNames =
    std::vector<SampledCommandNameEnum>{SampledCommandNameEnum::kFind,
                                        SampledCommandNameEnum::kAggregate,
                                        SampledCommandNameEnum::kCount,
                                        SampledCommandNameEnum::kDistinct};

struct ReadMetrics {
    int64_t numSingleShard = 0;
    int64_t numMultiShard = 0;
    int64_t numScatterGather = 0;
    std::vector<int64_t> numByRange;
};

struct WriteMetrics {
    int64_t numSingleShard = 0;
    int64_t numMultiShard = 0;
    int64_t numScatterGather = 0;
    std::vector<int64_t> numByRange;
    int64_t numShardKeyUpdates = 0;
    int64_t numSingleWritesWithoutShardKey = 0;
    int64_t numMultiWritesWithoutShardKey = 0;
};

struct ChunkSplitInfo {
    ShardKeyPattern shardKeyPattern;
    std::vector<BSONObj> splitPoints;
};

struct ReadWriteDistributionTest : public ShardServerTestFixture {
protected:
    /**
     * Returns a CollectionRoutingInfoTargeter with the given shard key pattern, split points and
     * default collator.
     */
    CollectionRoutingInfoTargeter makeCollectionRoutingInfoTargeter(
        const ChunkSplitInfo& chunkSplitInfo,
        std::unique_ptr<CollatorInterface> defaultCollator = nullptr) const {
        auto splitPointsIncludingEnds(chunkSplitInfo.splitPoints);
        splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                        chunkSplitInfo.shardKeyPattern.getKeyPattern().globalMin());
        splitPointsIncludingEnds.push_back(
            chunkSplitInfo.shardKeyPattern.getKeyPattern().globalMax());

        const Timestamp timestamp{Timestamp(100, 1)};
        ChunkVersion version({OID::gen(), timestamp}, {1, 0});

        std::vector<ChunkType> chunks;
        for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
            ChunkType chunk(collUuid,
                            {chunkSplitInfo.shardKeyPattern.getKeyPattern().extendRangeBound(
                                 splitPointsIncludingEnds[i - 1], false),
                             chunkSplitInfo.shardKeyPattern.getKeyPattern().extendRangeBound(
                                 splitPointsIncludingEnds[i], false)},
                            version,
                            ShardId{str::stream() << (i - 1)});
            chunk.setName(OID::gen());

            chunks.push_back(chunk);
            version.incMajor();
        }

        auto routingTableHistory =
            RoutingTableHistory::makeNew(nss,
                                         collUuid,
                                         chunkSplitInfo.shardKeyPattern.getKeyPattern(),
                                         false, /* unsplittable */
                                         std::move(defaultCollator) /* collator */,
                                         false /* unique */,
                                         OID::gen(),
                                         timestamp,
                                         boost::none /* timeseriesFields */,
                                         boost::none /* reshardingFields */,
                                         true /* allowMigrations */,
                                         chunks);

        auto cm = ChunkManager(ShardId("dummyPrimaryShard"),
                               DatabaseVersion(UUID::gen(), timestamp),
                               RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(
                                   std::move(routingTableHistory))),
                               boost::none);
        return CollectionRoutingInfoTargeter(
            nss,
            CollectionRoutingInfo{std::move(cm),
                                  boost::optional<ShardingIndexesCatalogCache>(boost::none)});
    }

    int32_t getRandomInt(int32_t limit) const {
        return _random->nextInt32(limit);
    }

    bool getRandomBool() const {
        return getRandomInt(2) == 0;
    }

    SampledCommandNameEnum getRandomSampledReadCommandName() const {
        return kSampledReadCommandNames[getRandomInt(kSampledReadCommandNames.size())];
    }

    SampledQueryDocument makeSampledReadQueryDocument(
        SampledCommandNameEnum cmdName,
        const BSONObj& filter,
        const BSONObj& collation = BSONObj(),
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        auto cmd = SampledReadCommand{filter, collation};
        cmd.setLet(letParameters);
        return {UUID::gen(),
                nss,
                collUuid,
                cmdName,
                cmd.toBSON(),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledUpdateQueryDocument(
        const std::vector<write_ops::UpdateOpEntry>& updateOps,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        write_ops::UpdateCommandRequest cmd(nss);
        cmd.setUpdates(updateOps);
        cmd.setLet(letParameters);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kUpdate,
                cmd.toBSON(BSON("$db" << nss.db_forTest().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(
        const std::vector<write_ops::DeleteOpEntry>& deleteOps,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        write_ops::DeleteCommandRequest cmd(nss);
        cmd.setDeletes(deleteOps);
        cmd.setLet(letParameters);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kDelete,
                cmd.toBSON(BSON("$db" << nss.db_forTest().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(
        const BSONObj& filter,
        const write_ops::UpdateModification& update,
        bool upsert,
        bool remove,
        const BSONObj& collation = BSONObj(),
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        write_ops::FindAndModifyCommandRequest cmd(nss);
        cmd.setQuery(filter);
        cmd.setUpdate(update);
        cmd.setUpsert(upsert);
        cmd.setRemove(remove);
        cmd.setCollation(collation);
        cmd.setLet(letParameters);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kFindAndModify,
                cmd.toBSON(BSON("$db" << nss.db_forTest().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(
        const std::vector<write_ops::UpdateOpEntry>& updateOps,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        BulkWriteCommandRequest cmd;

        NamespaceInfoEntry nsEntry;
        nsEntry.setNs(nss);
        cmd.setNsInfo({nsEntry});

        std::vector<std::variant<BulkWriteInsertOp, BulkWriteUpdateOp, BulkWriteDeleteOp>> ops;
        for (const auto& updateOp : updateOps) {
            BulkWriteUpdateOp op;
            op.setUpdate(0);
            op.setFilter(updateOp.getQ());
            op.setMulti(updateOp.getMulti());
            op.setConstants(updateOp.getC());
            op.setUpdateMods(updateOp.getU());
            op.setHint(updateOp.getHint());
            op.setSort(updateOp.getSort());
            op.setCollation(updateOp.getCollation());
            op.setArrayFilters(updateOp.getArrayFilters());
            op.setUpsert(updateOp.getUpsert());
            op.setUpsertSupplied(updateOp.getUpsertSupplied());
            op.setSampleId(updateOp.getSampleId());
            op.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                updateOp.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());
            ops.push_back(std::move(op));
        }
        cmd.setOps(ops);

        cmd.setLet(letParameters);
        cmd.setDbName(DatabaseName::kAdmin);

        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kBulkWrite,
                cmd.toBSON(
                    BSON(BulkWriteCommandRequest::kDbNameFieldName << DatabaseNameUtil::serialize(
                             DatabaseName::kAdmin, SerializationContext::stateCommandRequest()))),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledBulkWriteDeleteQueryDocument(
        const std::vector<write_ops::DeleteOpEntry>& deleteOps,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        BulkWriteCommandRequest cmd;

        NamespaceInfoEntry nsEntry;
        nsEntry.setNs(nss);
        cmd.setNsInfo({nsEntry});

        std::vector<std::variant<BulkWriteInsertOp, BulkWriteUpdateOp, BulkWriteDeleteOp>> ops;
        for (const auto& deleteOp : deleteOps) {
            BulkWriteDeleteOp op;
            op.setDeleteCommand(0);
            op.setFilter(deleteOp.getQ());
            op.setMulti(deleteOp.getMulti());
            op.setHint(deleteOp.getHint());
            op.setCollation(deleteOp.getCollation());
            ops.push_back(std::move(op));
        }
        cmd.setOps(ops);

        cmd.setLet(letParameters);
        cmd.setDbName(DatabaseName::kAdmin);

        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kBulkWrite,
                cmd.toBSON(
                    BSON(BulkWriteCommandRequest::kDbNameFieldName << DatabaseNameUtil::serialize(
                             DatabaseName::kAdmin, SerializationContext::stateCommandRequest()))),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    void assertReadMetrics(const ReadDistributionMetricsCalculator& calculator,
                           const ReadMetrics& expectedMetrics) const {
        auto actualMetrics = calculator.getMetrics();
        auto expectedNumTotal = expectedMetrics.numSingleShard + expectedMetrics.numMultiShard +
            expectedMetrics.numScatterGather;

        ASSERT_EQ(*actualMetrics.getNumSingleShard(), expectedMetrics.numSingleShard);
        ASSERT_EQ(*actualMetrics.getPercentageOfSingleShard(),
                  calculatePercentage(expectedMetrics.numSingleShard, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumMultiShard(), expectedMetrics.numMultiShard);
        ASSERT_EQ(*actualMetrics.getPercentageOfMultiShard(),
                  calculatePercentage(expectedMetrics.numMultiShard, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumScatterGather(), expectedMetrics.numScatterGather);
        ASSERT_EQ(*actualMetrics.getPercentageOfScatterGather(),
                  calculatePercentage(expectedMetrics.numScatterGather, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumByRange(), expectedMetrics.numByRange);
    }

    void assertMetricsForReadQuery(const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc,
                                   const ReadMetrics& expectedMetrics) const {
        ReadDistributionMetricsCalculator readDistributionCalculator(targeter);
        readDistributionCalculator.addQuery(operationContext(), queryDoc);
        assertReadMetrics(readDistributionCalculator, expectedMetrics);
    }

    void assertWriteMetrics(const WriteDistributionMetricsCalculator& calculator,
                            const WriteMetrics& expectedMetrics) const {
        auto actualMetrics = calculator.getMetrics();
        auto expectedNumTotal = expectedMetrics.numSingleShard + expectedMetrics.numMultiShard +
            expectedMetrics.numScatterGather;

        ASSERT_EQ(*actualMetrics.getNumSingleShard(), expectedMetrics.numSingleShard);
        ASSERT_EQ(*actualMetrics.getPercentageOfSingleShard(),
                  calculatePercentage(expectedMetrics.numSingleShard, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumMultiShard(), expectedMetrics.numMultiShard);
        ASSERT_EQ(*actualMetrics.getPercentageOfMultiShard(),
                  calculatePercentage(expectedMetrics.numMultiShard, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumScatterGather(), expectedMetrics.numScatterGather);
        ASSERT_EQ(*actualMetrics.getPercentageOfScatterGather(),
                  calculatePercentage(expectedMetrics.numScatterGather, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumByRange(), expectedMetrics.numByRange);

        ASSERT_EQ(*actualMetrics.getNumShardKeyUpdates(), expectedMetrics.numShardKeyUpdates);
        ASSERT_EQ(*actualMetrics.getPercentageOfShardKeyUpdates(),
                  calculatePercentage(expectedMetrics.numShardKeyUpdates, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumSingleWritesWithoutShardKey(),
                  expectedMetrics.numSingleWritesWithoutShardKey);
        ASSERT_EQ(
            *actualMetrics.getPercentageOfSingleWritesWithoutShardKey(),
            calculatePercentage(expectedMetrics.numSingleWritesWithoutShardKey, expectedNumTotal));

        ASSERT_EQ(*actualMetrics.getNumMultiWritesWithoutShardKey(),
                  expectedMetrics.numMultiWritesWithoutShardKey);
        ASSERT_EQ(
            *actualMetrics.getPercentageOfMultiWritesWithoutShardKey(),
            calculatePercentage(expectedMetrics.numMultiWritesWithoutShardKey, expectedNumTotal));
    }

    void assertMetricsForWriteQuery(const CollectionRoutingInfoTargeter& targeter,
                                    const SampledQueryDocument& queryDoc,
                                    const WriteMetrics& expectedMetrics) const {
        WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);
        writeDistributionCalculator.addQuery(operationContext(), queryDoc);
        assertWriteMetrics(writeDistributionCalculator, expectedMetrics);
    }

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const UUID collUuid = UUID::gen();

    // Define two set of ChunkSplintInfo's for testing.

    // 'chunkSplitInfoRangeSharding0' makes the collection have the following chunks:
    // {a.x: MinKey, b.y: Minkey} -> {a.x: -100, b.y: "A"}
    // {a.x: -100, b.y: "A"} -> {a.x: 100, b.y: "A"}
    // {a.x: 100, b.y: "A"} -> {a.x: MaxKey, b.y: MaxKey}
    const ChunkSplitInfo chunkSplitInfoRangeSharding0{
        ShardKeyPattern{BSON("a.x" << 1 << "b.y" << 1)},
        std::vector<BSONObj>{{BSON("a.x" << -100 << "b.y"
                                         << "A"),
                              BSON("a.x" << 100 << "b.y"
                                         << "A")}}};

    // 'chunkSplitInfoHashedSharding0' makes the collection have the following chunks:
    // {a.x: MinKey, b.y: Minkey} -> {a.x: -100, b.y: hash("A")}
    // {a.x: -100, b.y: hash("A")} -> {a.x: 100, b.y: hash("A")}
    // {a.x: 100, b.y: hash("A")} -> {a.x: MaxKey, b.y: MaxKey}
    const ChunkSplitInfo chunkSplitInfoHashedSharding0{
        ShardKeyPattern{BSON("a.x" << 1 << "b.y"
                                   << "hashed")},
        std::vector<BSONObj>{
            BSON("a.x" << -100 << "b.y"
                       << BSONElementHasher::hash64(BSON(""
                                                         << "A")
                                                        .firstElement(),
                                                    BSONElementHasher::DEFAULT_HASH_SEED)),
            BSON("a.x" << 100 << "b.y"
                       << BSONElementHasher::hash64(BSON(""
                                                         << "A")
                                                        .firstElement(),
                                                    BSONElementHasher::DEFAULT_HASH_SEED))}};

    // 'chunkSplitInfoRangeSharding1' makes the collection have the following chunks:
    // {a: MinKey, b.y: Minkey} -> {a: {x: -100}, b.y: "A"}
    // {a: {x: -100}, b.y: "A"} -> {a: {x: 100}, b.y: "A"}
    // {a: {x: 100}, b.y: "A"} -> {a: MaxKey, b.y: MaxKey}
    const ChunkSplitInfo chunkSplitInfoRangeSharding1{
        ShardKeyPattern{BSON("a" << 1 << "b.y" << 1)},
        std::vector<BSONObj>{BSON("a" << BSON("x" << -100) << "b.y"
                                      << "A"),
                             BSON("a" << BSON("x" << 100) << "b.y"
                                      << "A")}};

    // 'chunkSplitInfoHashedSharding2' makes the collection have the following chunks:
    // {a: MinKey} -> {a: -4611686018427387902LL}
    // {a: -4611686018427387902LL} -> {a: 4611686018427387902LL}
    // {a: -4611686018427387902LL} -> {a: MaxKey}
    const ChunkSplitInfo chunkSplitInfoHashedSharding2{ShardKeyPattern{BSON("a"
                                                                            << "hashed")},
                                                       std::vector<BSONObj>{
                                                           BSON("a" << -4611686018427387902LL),
                                                           BSON("a" << 4611686018427387902LL)}};

    const BSONObj emptyCollation = {};
    const BSONObj simpleCollation = CollationSpec::kSimpleSpec;
    // Using a case-insensitive collation would cause a collatable point-query involving a chunk
    // bound to touch more than one chunk.
    const BSONObj caseInsensitiveCollation =
        BSON(Collation::kLocaleFieldName << "en_US"
                                         << "strength" << 1 << "caseLevel" << false);
    const std::unique_ptr<PseudoRandom> _random =
        std::make_unique<PseudoRandom>(SecureRandom().nextInt64());
};

TEST_F(ReadWriteDistributionTest, ReadDistributionNoQueries) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);
    auto metrics = readDistributionCalculator.getMetrics();

    auto sampleSize = metrics.getSampleSize();
    ASSERT_EQ(sampleSize.getTotal(), 0);
    ASSERT_EQ(sampleSize.getFind(), 0);
    ASSERT_EQ(sampleSize.getAggregate(), 0);
    ASSERT_EQ(sampleSize.getCount(), 0);
    ASSERT_EQ(sampleSize.getDistinct(), 0);

    ASSERT_FALSE(metrics.getNumSingleShard());
    ASSERT_FALSE(metrics.getNumMultiShard());
    ASSERT_FALSE(metrics.getNumScatterGather());
    ASSERT_FALSE(metrics.getNumByRange());
}

TEST_F(ReadWriteDistributionTest, WriteDistributionNoQueries) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);
    auto metrics = writeDistributionCalculator.getMetrics();

    auto sampleSize = metrics.getSampleSize();
    ASSERT_EQ(sampleSize.getTotal(), 0);
    ASSERT_EQ(sampleSize.getUpdate(), 0);
    ASSERT_EQ(sampleSize.getDelete(), 0);
    ASSERT_EQ(sampleSize.getFindAndModify(), 0);

    ASSERT_FALSE(metrics.getNumSingleShard());
    ASSERT_FALSE(metrics.getNumMultiShard());
    ASSERT_FALSE(metrics.getNumScatterGather());
    ASSERT_FALSE(metrics.getNumByRange());
}

TEST_F(ReadWriteDistributionTest, ReadDistributionSampleSize) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);

    // Add one find query.
    auto filter0 = BSON("a.x" << 0);
    readDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kFind, filter0));

    // Add two aggregate queries.
    auto filter1 = BSON("a.x" << 1);
    readDistributionCalculator.addQuery(
        operationContext(),
        makeSampledReadQueryDocument(SampledCommandNameEnum::kAggregate, filter1));
    auto filter2 = BSON("a.x" << 2);
    readDistributionCalculator.addQuery(
        operationContext(),
        makeSampledReadQueryDocument(SampledCommandNameEnum::kAggregate, filter2));

    // Add three count queries.
    auto filter3 = BSON("a.x" << 3);
    readDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kCount, filter3));
    auto filter4 = BSON("a.x" << 4);
    readDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kCount, filter4));
    auto filter5 = BSON("a.x" << 5);
    readDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kCount, filter5));

    // Add one distinct query.
    auto filter6 = BSON("a.x" << 6);
    readDistributionCalculator.addQuery(
        operationContext(),
        makeSampledReadQueryDocument(SampledCommandNameEnum::kDistinct, filter6));

    auto metrics = readDistributionCalculator.getMetrics();
    auto sampleSize = metrics.getSampleSize();
    ASSERT_EQ(sampleSize.getTotal(), 7);
    ASSERT_EQ(sampleSize.getFind(), 1);
    ASSERT_EQ(sampleSize.getAggregate(), 2);
    ASSERT_EQ(sampleSize.getCount(), 3);
    ASSERT_EQ(sampleSize.getDistinct(), 1);
}

TEST_F(ReadWriteDistributionTest, WriteDistributionSampleSize) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    // Add four update queries, one of them via bulkWrite.
    auto filter0 = BSON("a.x" << 0);
    auto updateOp0 = write_ops::UpdateOpEntry(
        filter0, write_ops::UpdateModification(BSON("$set" << BSON("c" << 0))));
    auto filter1 = BSON("a.x" << 1);
    auto updateOp1 = write_ops::UpdateOpEntry(
        filter1, write_ops::UpdateModification(BSON("$set" << BSON("c" << 1))));
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledUpdateQueryDocument({updateOp0, updateOp1}));
    auto filter2 = BSON("a.x" << 2);
    auto updateOp2 = write_ops::UpdateOpEntry(
        filter2, write_ops::UpdateModification(BSON("$set" << BSON("c" << 2))));
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledUpdateQueryDocument({updateOp2}));
    auto filter3 = BSON("a.x" << 3);
    auto updateOp3 = write_ops::UpdateOpEntry(
        filter2, write_ops::UpdateModification(BSON("$set" << BSON("c" << 3))));
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledBulkWriteUpdateQueryDocument({updateOp3}));

    // Add three delete queries, one of them via bulkWrite.
    auto filter4 = BSON("a.x" << 4);
    auto deleteOp4 = write_ops::DeleteOpEntry(filter4, false /* multi */);
    auto filter5 = BSON("a.x" << 5);
    auto deleteOp5 = write_ops::DeleteOpEntry(filter5, true /* multi */);
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledDeleteQueryDocument({deleteOp4, deleteOp5}));
    auto filter6 = BSON("a.x" << 6);
    auto deleteOp6 = write_ops::DeleteOpEntry(filter6, true /* multi */);
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledBulkWriteDeleteQueryDocument({deleteOp6}));

    // Add one findAndModify query.
    auto filter7 = BSON("a.x" << 7);
    auto updateMod = write_ops::UpdateModification(BSON("$set" << BSON("a.x" << 5)));
    writeDistributionCalculator.addQuery(
        operationContext(),
        makeSampledFindAndModifyQueryDocument(
            filter7, updateMod, false /* upsert */, false /* remove */));

    auto metrics = writeDistributionCalculator.getMetrics();
    auto sampleSize = metrics.getSampleSize();
    ASSERT_EQ(sampleSize.getTotal(), 8);
    ASSERT_EQ(sampleSize.getUpdate(), 4);
    ASSERT_EQ(sampleSize.getDelete(), 3);
    ASSERT_EQ(sampleSize.getFindAndModify(), 1);
}

DEATH_TEST_F(ReadWriteDistributionTest, ReadDistributionCannotAddUpdateQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    auto updateOp = write_ops::UpdateOpEntry(
        filter, write_ops::UpdateModification(BSON("$set" << BSON("c" << 0))));
    readDistributionCalculator.addQuery(operationContext(),
                                        makeSampledUpdateQueryDocument({updateOp}));
}

DEATH_TEST_F(ReadWriteDistributionTest, ReadDistributionCannotAddDeleteQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    auto deleteOp = write_ops::DeleteOpEntry(filter, false /* multi */);
    readDistributionCalculator.addQuery(operationContext(),
                                        makeSampledDeleteQueryDocument({deleteOp}));
}

DEATH_TEST_F(ReadWriteDistributionTest, ReadDistributionCannotAddFindAndModifyQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    auto updateMod = write_ops::UpdateModification(BSON("$set" << BSON("c" << 0)));
    readDistributionCalculator.addQuery(
        operationContext(),
        makeSampledFindAndModifyQueryDocument(
            filter, updateMod, false /* upsert */, false /* remove */));
}

DEATH_TEST_F(ReadWriteDistributionTest, ReadDistributionCannotAddBulkWriteQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    auto updateOp = write_ops::UpdateOpEntry(
        filter, write_ops::UpdateModification(BSON("$set" << BSON("c" << 0))));
    readDistributionCalculator.addQuery(operationContext(),
                                        makeSampledBulkWriteUpdateQueryDocument({updateOp}));
}

DEATH_TEST_F(ReadWriteDistributionTest, WriteDistributionCannotAddFindQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    writeDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kFind, filter));
}

DEATH_TEST_F(ReadWriteDistributionTest, WriteDistributionCannotAddAggregateQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    writeDistributionCalculator.addQuery(
        operationContext(),
        makeSampledReadQueryDocument(SampledCommandNameEnum::kAggregate, filter));
}

DEATH_TEST_F(ReadWriteDistributionTest, WriteDistributionCannotAddCountQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    writeDistributionCalculator.addQuery(
        operationContext(), makeSampledReadQueryDocument(SampledCommandNameEnum::kCount, filter));
}

DEATH_TEST_F(ReadWriteDistributionTest, WriteDistributionCannotAddDistinctQuery, "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    auto filter = BSON("a.x" << 0);
    writeDistributionCalculator.addQuery(
        operationContext(),
        makeSampledReadQueryDocument(SampledCommandNameEnum::kDistinct, filter));
}

class ReadDistributionFilterByShardKeyEqualityTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             const std::vector<int64_t>& numByRange,
                             bool hasSimpleCollation,
                             bool hasCollatableType) const {
        ReadMetrics metrics;
        if (hasSimpleCollation || !hasCollatableType) {
            metrics.numSingleShard = 1;
        } else {
            metrics.numMultiShard = 1;
        }
        metrics.numByRange = numByRange;
        assertMetricsForReadQuery(targeter, queryDoc, metrics);
    }
};

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityOrdered) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filters = std::vector<BSONObj>{BSON("a.x" << -100 << "b.y"
                                                   << "A"),
                                        BSON("a" << BSON("x" << 0) << "b.y"
                                                 << "A"),
                                        BSON("a" << BSON("x" << 0) << "b"
                                                 << BSON("y"
                                                         << "A")),
                                        BSON("a" << BSON("x" << 0) << "b"
                                                 << BSON("y"
                                                         << "A"))};
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    for (const auto& filter : filters) {
        assertTargetMetrics(targeter,
                            makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
    }
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityNotOrdered) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("b.y"
                       << "A"
                       << "a.x" << 0);
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityEveryFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSONNULL << "b.y" << BSONNULL);
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityPrefixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSONNULL << "b.y"
                             << "A");
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySuffixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << 0 << "b.y" << BSONNULL);
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityAdditionalFields) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0 << "a.x" << 100 << "b.y"
                             << "A");
    auto numByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyContainsCollatableFields) {
    auto hasSimpleCollation = false;
    auto hasCollatableType = true;

    auto assertMetrics = [&](const ChunkSplitInfo& chunkSplitInfo,
                             const BSONObj& filter,
                             const std::vector<int64_t>& numByRange) {
        // The collection has a non-simple default collation and the query specifies an empty
        // collation.
        auto targeter0 = makeCollectionRoutingInfoTargeter(
            chunkSplitInfo,
            uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                                ->makeFromBSON(caseInsensitiveCollation)));
        assertTargetMetrics(
            targeter0,
            makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
            numByRange,
            hasSimpleCollation,
            hasCollatableType);

        // The collection has a simple default collation and the query specifies a non-simple
        // collation.
        auto targeter1 = makeCollectionRoutingInfoTargeter(
            chunkSplitInfo,
            uassertStatusOK(
                CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
        assertTargetMetrics(targeter1,
                            makeSampledReadQueryDocument(getRandomSampledReadCommandName(),
                                                         filter,
                                                         caseInsensitiveCollation),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);

        // The collection doesn't have a default collation and the query specifies a non-simple
        // collation.
        auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfo);
        assertTargetMetrics(targeter2,
                            makeSampledReadQueryDocument(getRandomSampledReadCommandName(),
                                                         filter,
                                                         caseInsensitiveCollation),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
    };

    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto numByRange = std::vector<int64_t>({1, 1, 0});
    assertMetrics(chunkSplitInfoRangeSharding0, filter, numByRange);
    assertMetrics(chunkSplitInfoHashedSharding0, filter, numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyDoesNotContainCollatableFields) {
    auto filter = BSON("a.x" << -100 << "b.y" << 0);
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = false;
    auto hasCollatableType = false;

    // The collection has a non-simple default collation and the query specifies an empty
    // collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(
        targeter0,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection has a simple default collation and the query specifies a non-simple
    // collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(targeter1,
                        makeSampledReadQueryDocument(
                            getRandomSampledReadCommandName(), filter, caseInsensitiveCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a non-simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter2,
                        makeSampledReadQueryDocument(
                            getRandomSampledReadCommandName(), filter, caseInsensitiveCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySimpleCollation) {
    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    // The collection has a simple default collation and the query specifies an empty collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(
        targeter0,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection has a non-simple default collation and the query specifies a simple
    // collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(
        targeter1,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, simpleCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter2,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, simpleCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies an empty
    // collation.
    auto targeter3 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter3,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    auto filter = BSON("a" << -100);  // The hash of -100 is -1979677326953392702LL.
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;

    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityExpressionWithLetParameters) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("$expr" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$a.x"
                                                                                     << "$$value"))
                                                            << BSON("$eq" << BSON_ARRAY("$b.y"
                                                                                        << "A")))));
    auto collation = BSONObj();
    auto letParameters = BSON("value" << 100);

    auto numByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(
                            getRandomSampledReadCommandName(), filter, collation, letParameters),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

class ReadDistributionFilterByShardKeyRangeTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             const std::vector<int64_t>& numByRange) const {
        ReadMetrics metrics;
        metrics.numMultiShard = 1;
        metrics.numByRange = numByRange;
        assertMetricsForReadQuery(targeter, queryDoc, metrics);
    }

    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc) const {
        ReadMetrics metrics;
        metrics.numScatterGather = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 1});
        assertMetricsForReadQuery(targeter, queryDoc, metrics);
    }
};

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << 0);
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    auto filter = BSON("a" << BSON("x" << 0));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMinKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 1));
    auto numByRange = std::vector<int64_t>({1, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << 2));
    auto numByRange = std::vector<int64_t>({0, 1, 1});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeNoMinOrMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -3 << "$lt" << 3));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, FullShardKeyRange) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -4 << "$lt" << 4) << "b.y"
                             << BSON("$gte"
                                     << "A"
                                     << "$lt"
                                     << "Z"));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyNonEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$ne" << 5));
    auto numByRange = std::vector<int64_t>({1, 1, 1});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyRangeHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    // For hashed sharding, range queries always target all shards and chunks.
    auto filter = BSON("a" << BSON("$gte" << -100));
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

class ReadDistributionNotFilterByShardKeyTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc) const {
        ReadMetrics metrics;
        metrics.numScatterGather = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 1});
        assertMetricsForReadQuery(targeter, queryDoc, metrics);
    }
};

TEST_F(ReadDistributionNotFilterByShardKeyTest, ShardKeySuffixEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("b.y"
                       << "A");
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

TEST_F(ReadDistributionNotFilterByShardKeyTest, NoShardKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 1);
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

TEST_F(ReadDistributionNotFilterByShardKeyTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    // This query filters by "a" (exact match) not "a.x" (the shard key prefix). Currently, this
    // is handled as a query not filtering by the shard key. As a result, it still targets all
    // the chunks although it only matches the data in the chunk {a.x: -100, b.y: "A"} -> {a.x:
    // 100, b.y: "A"}. Please see
    // ReadDistributionFilterByShardKeyRangeTest/ShardKeyPrefixEqualityDotted for the case where
    // the query filters by "a.x".
    auto filter = BSON("a" << BSON("x" << 0));
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

TEST_F(ReadDistributionNotFilterByShardKeyTest, ShardKeyPrefixEqualityDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    // This query filters by "a.x" not "a" (the shard key prefix). Currently, this is handled as
    // a query not filtering by the shard key. As a result, it still targets all the chunks
    // although it only matches the data in the chunk {a.x: -100, b.y: "A"} -> {a.x: 100, b.y:
    // "A"}.
    auto filter = BSON("a.x" << 0);
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

TEST_F(ReadDistributionNotFilterByShardKeyTest, RuntimeConstants) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON(
        "$expr" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$ts"
                                                                      << "$$NOW"))
                                             << BSON("$eq" << BSON_ARRAY("$clusterTime"
                                                                         << "$$CLUSTER_TIME")))));
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter));
}

class WriteDistributionFilterByShardKeyEqualityTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             const std::vector<int64_t>& numByRange,
                             bool hasSimpleCollation,
                             bool hasCollatableType,
                             bool multi = false) const {
        WriteMetrics metrics;
        if (hasSimpleCollation || !hasCollatableType) {
            metrics.numSingleShard = 1;
        } else {
            metrics.numMultiShard = 1;
            if (multi) {
                metrics.numMultiWritesWithoutShardKey = 1;
            } else {
                metrics.numSingleWritesWithoutShardKey = 1;
            }
        }
        metrics.numByRange = numByRange;
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    // For a write that filters by shard key equality, the targeting metrics do not depend on
    // whether it is an upsert or multi.

    SampledQueryDocument makeSampledUpdateQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        const BSONObj& collation = BSONObj(),
        bool multi = false,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        updateOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp}, letParameters);
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        const BSONObj& collation = BSONObj(),
        bool multi = false,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        updateOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledBulkWriteUpdateQueryDocument({updateOp},
                                                                                  letParameters);
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(
        const BSONObj& filter,
        const BSONObj& collation = BSONObj(),
        bool multi = false,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        deleteOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp}, letParameters);
    }

    SampledQueryDocument makeSampledBulkWriteDeleteQueryDocument(
        const BSONObj& filter,
        const BSONObj& collation = BSONObj(),
        bool multi = false,
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        deleteOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledBulkWriteDeleteQueryDocument({deleteOp},
                                                                                  letParameters);
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        const BSONObj& collation = BSONObj(),
        const boost::optional<BSONObj>& letParameters = boost::none) const {
        return ReadWriteDistributionTest::makeSampledFindAndModifyQueryDocument(
            filter,
            updateMod,
            getRandomBool() /* upsert */,
            getRandomBool() /* remove */,
            collation,
            letParameters);
    }
};

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityOrdered) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filters = std::vector<BSONObj>{BSON("a.x" << -100 << "b.y"
                                                   << "A"),
                                        BSON("a" << BSON("x" << 0) << "b.y"
                                                 << "A"),
                                        BSON("a" << BSON("x" << 0) << "b"
                                                 << BSON("y"
                                                         << "A")),
                                        BSON("a" << BSON("x" << 0) << "b"
                                                 << BSON("y"
                                                         << "A"))};
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    for (const auto& filter : filters) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledDeleteQueryDocument(filter),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteDeleteQueryDocument(filter),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledFindAndModifyQueryDocument(filter, updateMod),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType);
    }
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityNotOrdered) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("b.y"
                       << "A"
                       << "a.x" << 0);
    auto updateMod = BSON("$set" << BSON("c" << 0));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityAdditionalFields) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0 << "a.x" << 100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyContainsCollatableFields) {
    auto hasSimpleCollation = false;
    auto hasCollatableType = true;

    auto assertMetrics = [&](const ChunkSplitInfo& chunkSplitInfo,
                             const BSONObj& filter,
                             const BSONObj& updateMod,
                             const std::vector<int64_t>& numByRange) {
        for (auto& multi : {true, false}) {
            // The collection has a non-simple default collation and the query specifies an empty
            // collation.
            auto targeter0 = makeCollectionRoutingInfoTargeter(
                chunkSplitInfo,
                uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                                    ->makeFromBSON(caseInsensitiveCollation)));
            assertTargetMetrics(
                targeter0,
                makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation, multi),
                numByRange,
                hasSimpleCollation,
                hasCollatableType,
                multi);
            assertTargetMetrics(
                targeter0,
                makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, emptyCollation, multi),
                numByRange,
                hasSimpleCollation,
                hasCollatableType,
                multi);

            // The collection has a simple default collation and the query specifies a non-simple
            // collation.
            auto targeter1 = makeCollectionRoutingInfoTargeter(
                chunkSplitInfo,
                uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                                    ->makeFromBSON(simpleCollation)));
            assertTargetMetrics(
                targeter1,
                makeSampledDeleteQueryDocument(filter, caseInsensitiveCollation, multi),
                numByRange,
                hasSimpleCollation,
                hasCollatableType,
                multi);
            assertTargetMetrics(
                targeter1,
                makeSampledBulkWriteDeleteQueryDocument(filter, caseInsensitiveCollation, multi),
                numByRange,
                hasSimpleCollation,
                hasCollatableType,
                multi);
        }

        // The collection doesn't have a default collation and the query specifies a non-simple
        // collation.
        auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfo);
        assertTargetMetrics(
            targeter2,
            makeSampledFindAndModifyQueryDocument(filter, updateMod, caseInsensitiveCollation),
            numByRange,
            hasSimpleCollation,
            hasCollatableType);
    };

    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numByRange = std::vector<int64_t>({1, 1, 0});
    assertMetrics(chunkSplitInfoRangeSharding0, filter, updateMod, numByRange);
    assertMetrics(chunkSplitInfoHashedSharding0, filter, updateMod, numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyDoesNotContainCollatableFields) {
    auto filter = BSON("a.x" << -100 << "b.y" << 0);
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = false;
    auto hasCollatableType = false;

    // The collection has a non-simple default collation and the query specifies an empty collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(targeter0,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter0,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection has a simple default collation and the query specifies a non-simple collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(targeter1,
                        makeSampledDeleteQueryDocument(filter, caseInsensitiveCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter1,
                        makeSampledBulkWriteDeleteQueryDocument(filter, caseInsensitiveCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a non-simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter2,
        makeSampledFindAndModifyQueryDocument(filter, updateMod, caseInsensitiveCollation),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySimpleCollation) {
    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    // The collection has a simple default collation and the query specifies an empty collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(targeter0,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter0,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection has a non-simple default collation and the query specifies a simple collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(targeter1,
                        makeSampledDeleteQueryDocument(filter, simpleCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter1,
                        makeSampledBulkWriteDeleteQueryDocument(filter, simpleCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a simple collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter2,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod, simpleCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies an empty collation.
    auto targeter3 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter3,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter3,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    auto filter = BSON("a" << -100);  // The hash of -100 is -1979677326953392702LL.
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityEveryFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSONNULL << "b.y" << BSONNULL);
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityPrefixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSONNULL << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySuffixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << 0 << "b.y" << BSONNULL);
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledBulkWriteDeleteQueryDocument(filter),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityExpressionWithLetParameters) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("$expr" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$a.x"
                                                                                     << "$$value"))
                                                            << BSON("$eq" << BSON_ARRAY("$b.y"
                                                                                        << "A")))));
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto collation = BSONObj();
    auto letParameters = BSON("value" << 100);

    auto numByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter,
            makeSampledUpdateQueryDocument(filter, updateMod, collation, multi, letParameters),
            numByRange,
            hasSimpleCollation,
            hasCollatableType,
            multi);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(
                                filter, updateMod, collation, multi, letParameters),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType,
                            multi);
        assertTargetMetrics(targeter,
                            makeSampledDeleteQueryDocument(filter, collation, multi, letParameters),
                            numByRange,
                            hasSimpleCollation,
                            hasCollatableType,
                            multi);
        assertTargetMetrics(
            targeter,
            makeSampledBulkWriteDeleteQueryDocument(filter, collation, multi, letParameters),
            numByRange,
            hasSimpleCollation,
            hasCollatableType,
            multi);
    }
    assertTargetMetrics(
        targeter,
        makeSampledFindAndModifyQueryDocument(filter, updateMod, collation, letParameters),
        numByRange,
        hasSimpleCollation,
        hasCollatableType);
}

class WriteDistributionFilterByShardKeyRangeTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi,
                             const std::vector<int64_t>& numByRange) const {
        WriteMetrics metrics;
        metrics.numMultiShard = 1;
        metrics.numByRange = numByRange;
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi) const {
        WriteMetrics metrics;
        metrics.numScatterGather = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 1});
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    // For a write that filters by shard key range, the targeting metrics do not depend on whether
    // it is an upsert.

    SampledQueryDocument makeSampledUpdateQueryDocument(const BSONObj& filter,
                                                        const BSONObj& updateMod,
                                                        bool multi) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(const BSONObj& filter,
                                                                 const BSONObj& updateMod,
                                                                 bool multi) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        return ReadWriteDistributionTest::makeSampledBulkWriteUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(const BSONObj& filter, bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp});
    }

    SampledQueryDocument makeSampledBulkWriteDeleteQueryDocument(const BSONObj& filter,
                                                                 bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledBulkWriteDeleteQueryDocument({deleteOp});
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(const BSONObj& filter,
                                                               const BSONObj& updateMod) const {
        return ReadWriteDistributionTest::makeSampledFindAndModifyQueryDocument(
            filter, updateMod, getRandomBool() /* upsert */, getRandomBool() /* remove */);
    }
};

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << 0);
    auto updateMod = BSON("$set" << BSON("c" << 0));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    auto filter = BSON("a" << BSON("x" << 0));
    auto updateMod = BSON("$set" << BSON("c" << 0));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMinKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 1));
    auto updateMod = BSON("$set" << BSON("c" << 1));
    auto numByRange = std::vector<int64_t>({1, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << 2));
    auto updateMod = BSON("$set" << BSON("c" << 2));
    auto numByRange = std::vector<int64_t>({0, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeNoMinOrMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -3 << "$lt" << 3));
    auto updateMod = BSON("$set" << BSON("c" << 3));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, FullShardKeyRange) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -4 << "$lt" << 4) << "b.y"
                             << BSON("$gte"
                                     << "A"
                                     << "$lt"
                                     << "Z"));
    auto updateMod = BSON("$set" << BSON("c" << 4));
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyNonEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$ne" << 5));
    auto updateMod = BSON("$set" << BSON("c" << 5));
    auto numByRange = std::vector<int64_t>({1, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi, numByRange);
        assertTargetMetrics(targeter,
                            makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numByRange);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi, numByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyRangeHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    // For hashed sharding, range queries always target all shards and chunks.
    auto filter = BSON("a" << BSON("$gte" << -100));
    auto updateMod = BSON("$set" << BSON("c" << 5));
    auto numByRange = std::vector<int64_t>({1, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

class WriteDistributionFilterByShardKeyRangeReplacementUpdateTest
    : public ReadWriteDistributionTest {
protected:
    SampledQueryDocument makeSampledUpdateQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        bool upsert,
        const BSONObj& collation = BSONObj()) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(false);  // replacement-style update cannot be multi.
        updateOp.setUpsert(upsert);
        updateOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        bool upsert,
        const BSONObj& collation = BSONObj()) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(false);  // replacement-style update cannot be multi.
        updateOp.setUpsert(upsert);
        updateOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledBulkWriteUpdateQueryDocument({updateOp});
    }

private:
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithoutShardKey", true};
};

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NotUpsert) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0) << "_id" << 0);
    auto updateMod = BSON("a" << BSON("x" << 0) << "b"
                              << BSON("y"
                                      << "A")
                              << "c" << 0);

    WriteMetrics metrics;
    metrics.numSingleShard = 1;
    metrics.numByRange = std::vector<int64_t>({0, 1, 0});
    assertMetricsForWriteQuery(
        targeter, makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */), metrics);
    assertMetricsForWriteQuery(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, false /* upsert */),
        metrics);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NotUpsertEveryFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0) << "_id" << 0);
    auto updateMod = BSON("a" << BSON("x" << 0));

    WriteMetrics metrics;
    metrics.numSingleShard = 1;
    metrics.numByRange = std::vector<int64_t>({0, 1, 0});
    assertMetricsForWriteQuery(
        targeter, makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */), metrics);
    assertMetricsForWriteQuery(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, false /* upsert */),
        metrics);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NotUpsertPrefixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0) << "_id" << 0);
    auto updateMod = BSON("b" << BSON("y"
                                      << "A"));

    WriteMetrics metrics;
    metrics.numSingleShard = 1;
    metrics.numByRange = std::vector<int64_t>({1, 0, 0});
    assertMetricsForWriteQuery(
        targeter, makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */), metrics);
    assertMetricsForWriteQuery(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, false /* upsert */),
        metrics);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NotUpsertSuffixFieldIsNull) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0) << "_id" << 0);
    auto updateMod = BSON("a" << BSON("x" << 0));

    WriteMetrics metrics;
    metrics.numSingleShard = 1;
    metrics.numByRange = std::vector<int64_t>({0, 1, 0});
    assertMetricsForWriteQuery(
        targeter, makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */), metrics);
    assertMetricsForWriteQuery(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, false /* upsert */),
        metrics);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NonUpsertNotExactIdQuery) {
    auto runTest = [&](const BSONObj& filter, const BSONObj& collation = BSONObj()) {
        auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
        auto updateMod = BSON("a" << BSON("x" << 0) << "b"
                                  << BSON("y"
                                          << "A")
                                  << "c" << 0);

        WriteMetrics metrics;
        metrics.numMultiShard = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 0});
        metrics.numSingleWritesWithoutShardKey = 1;
        assertMetricsForWriteQuery(
            targeter,
            makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */, collation),
            metrics);
        assertMetricsForWriteQuery(targeter,
                                   makeSampledBulkWriteUpdateQueryDocument(
                                       filter, updateMod, false /* upsert */, collation),
                                   metrics);
    };

    runTest(BSON("a.x" << BSON("$lt" << 0)));
    runTest(BSON("a.x" << BSON("$lt" << 0) << "_id" << BSON("$lt" << 0)));
    runTest(BSON("a.x" << BSON("$lt" << 0) << "_id"
                       << "0"),
            caseInsensitiveCollation);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, Upsert) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0));
    auto updateMod = BSON("a" << BSON("x" << 0) << "b"
                              << BSON("y"
                                      << "A")
                              << "c" << 0);

    WriteMetrics metrics;
    metrics.numMultiShard = 1;
    metrics.numByRange = std::vector<int64_t>({1, 1, 0});
    metrics.numSingleWritesWithoutShardKey = 1;
    assertMetricsForWriteQuery(
        targeter, makeSampledUpdateQueryDocument(filter, updateMod, true /* upsert */), metrics);
    assertMetricsForWriteQuery(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, true /* upsert */),
        metrics);
}

class WriteDistributionNotFilterByShardKeyTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi) const {
        WriteMetrics metrics;
        metrics.numScatterGather = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 1});
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    // For a write that doesn't filter by shard key equality or range, the targeting metrics do not
    // depend on whether it is an upsert.
    SampledQueryDocument makeSampledUpdateQueryDocument(const BSONObj& filter,
                                                        const BSONObj& updateMod,
                                                        bool multi) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(const BSONObj& filter,
                                                                 const BSONObj& updateMod,
                                                                 bool multi) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(multi);
        updateOp.setUpsert(getRandomBool());
        return ReadWriteDistributionTest::makeSampledBulkWriteUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(const BSONObj& filter, bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp});
    }

    SampledQueryDocument makeSampledBulkWriteDeleteQueryDocument(const BSONObj& filter,
                                                                 bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledBulkWriteDeleteQueryDocument({deleteOp});
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(const BSONObj& filter,
                                                               const BSONObj& updateMod) const {
        return ReadWriteDistributionTest::makeSampledFindAndModifyQueryDocument(
            filter, updateMod, getRandomBool() /* upsert */, getRandomBool() /* remove */);
    }
};

TEST_F(WriteDistributionNotFilterByShardKeyTest, ShardKeySuffixEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("b.y"
                       << "A");
    auto updateMod = BSON("$set" << BSON("c" << 0));
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

TEST_F(WriteDistributionNotFilterByShardKeyTest, NoShardKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 100);
    auto updateMod = BSON("$set" << BSON("c" << 100));
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

TEST_F(WriteDistributionNotFilterByShardKeyTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    // This query filters by "a" (exact match) not "a.x" (the shard key prefix). Currently, this is
    // handled as a query not filtering by the shard key. As a result, it still targets all the
    // chunks although it only matches the data in the chunk {a.x: -100, b.y: "A"} -> {a.x: 100,
    // b.y: "A"}. Please see WriteDistributionFilterByShardKeyRangeTest/ShardKeyPrefixEqualityDotted
    // for the case where the query filters by "a.x".
    auto filter = BSON("a" << BSON("x" << 0));
    auto updateMod = BSON("$set" << BSON("c" << 100));
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

TEST_F(WriteDistributionNotFilterByShardKeyTest, ShardKeyPrefixEqualityDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    // This query filters by "a.x" not "a" (the shard key prefix). Currently, this is handled as a
    // query not filtering by the shard key. As a result, it still targets all the chunks although
    // it only matches the data in the chunk {a.x: -100, b.y: "A"} -> {a.x: 100, b.y: "A"}.
    auto filter = BSON("a.x" << 0);
    auto updateMod = BSON("$set" << BSON("c" << 100));
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
        assertTargetMetrics(
            targeter, makeSampledBulkWriteDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

class WriteDistributionNotFilterByShardKeyReplacementUpdateTest : public ReadWriteDistributionTest {
protected:
    SampledQueryDocument makeSampledUpdateQueryDocument(const BSONObj& filter,
                                                        const BSONObj& updateMod,
                                                        bool upsert) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(false);  // replacement-style update cannot be multi.
        updateOp.setUpsert(upsert);
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledBulkWriteUpdateQueryDocument(const BSONObj& filter,
                                                                 const BSONObj& updateMod,
                                                                 bool upsert) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(false);  // replacement-style update cannot be multi.
        updateOp.setUpsert(upsert);
        return ReadWriteDistributionTest::makeSampledBulkWriteUpdateQueryDocument({updateOp});
    }
};

TEST_F(WriteDistributionNotFilterByShardKeyReplacementUpdateTest, NotUpsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc,
                                   const std::vector<int64_t> numByRange) {
        WriteMetrics metrics;
        metrics.numSingleShard = 1;
        metrics.numByRange = numByRange;
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0);
    auto updateMod = BSON("_id" << 0 << "a" << BSON("x" << 0) << "b"
                                << BSON("y"
                                        << "A")
                                << "c" << 0);
    auto numByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */),
                        numByRange);
    assertTargetMetrics(
        targeter,
        makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, false /* upsert */),
        numByRange);
}

TEST_F(WriteDistributionNotFilterByShardKeyReplacementUpdateTest, Upsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc) {
        WriteMetrics metrics;
        metrics.numScatterGather = 1;
        metrics.numByRange = std::vector<int64_t>({1, 1, 1});
        metrics.numSingleWritesWithoutShardKey = 1;
        assertMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0);
    auto updateMod = BSON("_id" << 0 << "a" << BSON("x" << 0) << "b"
                                << BSON("y"
                                        << "A")
                                << "c" << 0);
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, true /* upsert */));
    assertTargetMetrics(
        targeter, makeSampledBulkWriteUpdateQueryDocument(filter, updateMod, true /* upsert */));
}

TEST_F(ReadWriteDistributionTest, WriteDistributionShardKeyUpdates) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    // Add two updates.
    auto filter0 = BSON("a.x" << 0 << "b.y"
                              << "A");
    auto updateOp0 = write_ops::UpdateOpEntry(
        filter0, write_ops::UpdateModification(BSON("$set" << BSON("c" << 0))));
    auto filter1 = BSON("a.x" << 1 << "b.y"
                              << "A");
    auto updateOp1 = write_ops::UpdateOpEntry(
        filter1, write_ops::UpdateModification(BSON("$set" << BSON("c" << 1))));
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledUpdateQueryDocument({updateOp0, updateOp1}));

    WriteMetrics metrics;
    metrics.numSingleShard = 2;
    metrics.numByRange = std::vector<int64_t>({0, 2, 0});

    // Can set number of shard key updates to > 0.
    auto numShardKeyUpdates = 1;
    writeDistributionCalculator.setNumShardKeyUpdates(numShardKeyUpdates);
    metrics.numShardKeyUpdates = numShardKeyUpdates;
    assertWriteMetrics(writeDistributionCalculator, metrics);

    // Can set number of shard key updates to 0.
    numShardKeyUpdates = 0;
    writeDistributionCalculator.setNumShardKeyUpdates(numShardKeyUpdates);
    metrics.numShardKeyUpdates = numShardKeyUpdates;
    assertWriteMetrics(writeDistributionCalculator, metrics);
}

DEATH_TEST_F(ReadWriteDistributionTest,
             WriteDistributionCannotSetNumShardKeyUpdatesToNegative,
             "invariant") {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);
    writeDistributionCalculator.setNumShardKeyUpdates(-1);
}

TEST(ReadDistributionMetricsTest, AddOperator) {
    ReadDistributionMetrics metrics0;

    ReadSampleSize sampleSize0;
    sampleSize0.setFind(1);
    sampleSize0.setAggregate(2);
    sampleSize0.setCount(3);
    sampleSize0.setDistinct(4);
    auto numTotal0 = 1 + 2 + 3 + 4;
    sampleSize0.setTotal(numTotal0);

    metrics0.setSampleSize(sampleSize0);
    metrics0.setNumSingleShard(1);
    metrics0.setNumMultiShard(2);
    metrics0.setNumScatterGather(3);
    metrics0.setNumByRange(std::vector<int64_t>{1, 2, 3});

    ReadDistributionMetrics metrics1;

    ReadSampleSize sampleSize1;
    sampleSize1.setFind(10);
    sampleSize1.setAggregate(20);
    sampleSize1.setCount(30);
    sampleSize1.setDistinct(40);
    auto numTotal1 = 10 + 20 + 30 + 40;
    sampleSize1.setTotal(numTotal1);

    metrics1.setSampleSize(sampleSize1);
    metrics1.setNumSingleShard(10);
    metrics1.setNumMultiShard(20);
    metrics1.setNumScatterGather(30);
    metrics1.setNumByRange(std::vector<int64_t>{10, 20, 30});

    ReadDistributionMetrics expectedMetrics;

    ReadSampleSize expectedSampleSize;
    expectedSampleSize.setFind(11);
    expectedSampleSize.setAggregate(22);
    expectedSampleSize.setCount(33);
    expectedSampleSize.setDistinct(44);
    auto expectedNumtotal = 11 + 22 + 33 + 44;
    expectedSampleSize.setTotal(expectedNumtotal);
    expectedMetrics.setSampleSize(expectedSampleSize);

    expectedMetrics.setNumSingleShard(11);
    expectedMetrics.setPercentageOfSingleShard(calculatePercentage(11, expectedNumtotal));

    expectedMetrics.setNumMultiShard(22);
    expectedMetrics.setPercentageOfMultiShard(calculatePercentage(22, expectedNumtotal));

    expectedMetrics.setNumScatterGather(33);
    expectedMetrics.setPercentageOfScatterGather(calculatePercentage(33, expectedNumtotal));

    expectedMetrics.setNumByRange(std::vector<int64_t>{11, 22, 33});

    ASSERT((metrics0 + metrics1) == expectedMetrics);
}

TEST(WriteDistributionMetricsTest, AddOperator) {
    WriteDistributionMetrics metrics0;

    WriteSampleSize sampleSize0;
    sampleSize0.setUpdate(1);
    sampleSize0.setDelete(2);
    sampleSize0.setFindAndModify(3);
    auto numTotal0 = 1 + 2 + 3;
    sampleSize0.setTotal(numTotal0);
    metrics0.setSampleSize(sampleSize0);

    metrics0.setNumSingleShard(1);
    metrics0.setNumMultiShard(2);
    metrics0.setNumScatterGather(3);
    metrics0.setNumByRange(std::vector<int64_t>{1, 2, 3});
    metrics0.setNumShardKeyUpdates(1);
    metrics0.setNumSingleWritesWithoutShardKey(2);
    metrics0.setNumMultiWritesWithoutShardKey(3);

    WriteDistributionMetrics metrics1;

    WriteSampleSize sampleSize1;
    sampleSize1.setUpdate(10);
    sampleSize1.setDelete(20);
    sampleSize1.setFindAndModify(30);
    auto numTotal1 = 10 + 20 + 30;
    sampleSize1.setTotal(numTotal1);
    metrics1.setSampleSize(sampleSize1);

    metrics1.setNumSingleShard(10);
    metrics1.setNumMultiShard(20);
    metrics1.setNumScatterGather(30);
    metrics1.setNumByRange(std::vector<int64_t>{10, 20, 30});
    metrics1.setNumShardKeyUpdates(10);
    metrics1.setNumSingleWritesWithoutShardKey(20);
    metrics1.setNumMultiWritesWithoutShardKey(30);

    WriteDistributionMetrics expectedMetrics;

    WriteSampleSize expectedSampleSize;
    expectedSampleSize.setUpdate(11);
    expectedSampleSize.setDelete(22);
    expectedSampleSize.setFindAndModify(33);
    auto expectedNumtotal = 11 + 22 + 33;
    expectedSampleSize.setTotal(expectedNumtotal);
    expectedMetrics.setSampleSize(expectedSampleSize);

    expectedMetrics.setNumSingleShard(11);
    expectedMetrics.setPercentageOfSingleShard(calculatePercentage(11, expectedNumtotal));

    expectedMetrics.setNumMultiShard(22);
    expectedMetrics.setPercentageOfMultiShard(calculatePercentage(22, expectedNumtotal));

    expectedMetrics.setNumScatterGather(33);
    expectedMetrics.setPercentageOfScatterGather(calculatePercentage(33, expectedNumtotal));

    expectedMetrics.setNumByRange(std::vector<int64_t>{11, 22, 33});

    expectedMetrics.setNumShardKeyUpdates(11);
    expectedMetrics.setPercentageOfShardKeyUpdates(calculatePercentage(11, expectedNumtotal));

    expectedMetrics.setNumSingleWritesWithoutShardKey(22);
    expectedMetrics.setPercentageOfSingleWritesWithoutShardKey(
        calculatePercentage(22, expectedNumtotal));

    expectedMetrics.setNumMultiWritesWithoutShardKey(33);
    expectedMetrics.setPercentageOfMultiWritesWithoutShardKey(
        calculatePercentage(33, expectedNumtotal));

    ASSERT((metrics0 + metrics1) == expectedMetrics);
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
