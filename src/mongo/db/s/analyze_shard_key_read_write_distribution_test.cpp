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

#include "mongo/db/hasher.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const auto kSampledReadCommandNames =
    std::vector<SampledCommandNameEnum>{SampledCommandNameEnum::kFind,
                                        SampledCommandNameEnum::kAggregate,
                                        SampledCommandNameEnum::kCount,
                                        SampledCommandNameEnum::kDistinct};

struct ReadTargetMetricsBundle {
    int64_t numTargetedOneShard = 0;
    int64_t numTargetedMultipleShards = 0;
    int64_t numTargetedAllShards = 0;
    std::vector<int64_t> numDispatchedByRange;
};

struct WriteTargetMetricsBundle {
    int64_t numTargetedOneShard = 0;
    int64_t numTargetedMultipleShards = 0;
    int64_t numTargetedAllShards = 0;
    std::vector<int64_t> numDispatchedByRange;
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
                                         std::move(defaultCollator) /* collator */,
                                         false /* unique */,
                                         OID::gen(),
                                         timestamp,
                                         boost::none /* timeseriesFields */,
                                         boost::none /* reshardingFields */,
                                         boost::none /* maxChunkSizeBytes */,
                                         true /* allowMigrations */,
                                         chunks);

        auto cm = ChunkManager(ShardId("dummyPrimaryShard"),
                               DatabaseVersion(UUID::gen(), timestamp),
                               RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(
                                   std::move(routingTableHistory))),
                               boost::none);
        return CollectionRoutingInfoTargeter(
            CollectionRoutingInfo{std::move(cm), boost::optional<GlobalIndexesCache>(boost::none)});
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

    SampledQueryDocument makeSampledReadQueryDocument(SampledCommandNameEnum cmdName,
                                                      const BSONObj& filter,
                                                      const BSONObj& collation = BSONObj()) const {
        auto cmd = SampledReadCommand{filter, collation};
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
        const std::vector<write_ops::UpdateOpEntry>& updateOps) const {
        write_ops::UpdateCommandRequest cmd(nss);
        cmd.setUpdates(updateOps);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kUpdate,
                cmd.toBSON(BSON("$db" << nss.db().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(
        const std::vector<write_ops::DeleteOpEntry>& deleteOps) const {
        write_ops::DeleteCommandRequest cmd(nss);
        cmd.setDeletes(deleteOps);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kDelete,
                cmd.toBSON(BSON("$db" << nss.db().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(
        const BSONObj& filter,
        const write_ops::UpdateModification& update,
        bool upsert,
        bool remove,
        const BSONObj& collation = BSONObj()) const {
        write_ops::FindAndModifyCommandRequest cmd(nss);
        cmd.setQuery(filter);
        cmd.setUpdate(update);
        cmd.setUpsert(upsert);
        cmd.setRemove(remove);
        cmd.setCollation(collation);
        return {UUID::gen(),
                nss,
                collUuid,
                SampledCommandNameEnum::kFindAndModify,
                cmd.toBSON(BSON("$db" << nss.db().toString())),
                Date_t::now() +
                    mongo::Milliseconds(
                        analyze_shard_key::gQueryAnalysisSampleExpirationSecs.load() * 1000)};
    }

    void assertTargetMetricsForReadQuery(const CollectionRoutingInfoTargeter& targeter,
                                         const SampledQueryDocument& queryDoc,
                                         const ReadTargetMetricsBundle& expectedMetrics) const {
        ReadDistributionMetricsCalculator readDistributionCalculator(targeter);
        readDistributionCalculator.addQuery(operationContext(), queryDoc);

        auto metrics = readDistributionCalculator.getMetrics();
        auto expectedNumTotal = expectedMetrics.numTargetedOneShard +
            expectedMetrics.numTargetedMultipleShards + expectedMetrics.numTargetedAllShards;

        ASSERT_EQ(*metrics.getNumTargetedOneShard(), expectedMetrics.numTargetedOneShard);
        ASSERT_EQ(*metrics.getPercentageOfTargetedOneShard(),
                  calculatePercentage(expectedMetrics.numTargetedOneShard, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumTargetedMultipleShards(),
                  expectedMetrics.numTargetedMultipleShards);
        ASSERT_EQ(*metrics.getPercentageOfTargetedMultipleShards(),
                  calculatePercentage(expectedMetrics.numTargetedMultipleShards, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumTargetedAllShards(), expectedMetrics.numTargetedAllShards);
        ASSERT_EQ(*metrics.getPercentageOfTargetedAllShards(),
                  calculatePercentage(expectedMetrics.numTargetedAllShards, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumDispatchedByRange(), expectedMetrics.numDispatchedByRange);
    }

    void assertTargetMetricsForWriteQuery(const CollectionRoutingInfoTargeter& targeter,
                                          const SampledQueryDocument& queryDoc,
                                          const WriteTargetMetricsBundle& expectedMetrics) const {
        WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);
        writeDistributionCalculator.addQuery(operationContext(), queryDoc);

        auto metrics = writeDistributionCalculator.getMetrics();
        auto expectedNumTotal = expectedMetrics.numTargetedOneShard +
            expectedMetrics.numTargetedMultipleShards + expectedMetrics.numTargetedAllShards;

        ASSERT_EQ(*metrics.getNumTargetedOneShard(), expectedMetrics.numTargetedOneShard);
        ASSERT_EQ(*metrics.getPercentageOfTargetedOneShard(),
                  calculatePercentage(expectedMetrics.numTargetedOneShard, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumTargetedMultipleShards(),
                  expectedMetrics.numTargetedMultipleShards);
        ASSERT_EQ(*metrics.getPercentageOfTargetedMultipleShards(),
                  calculatePercentage(expectedMetrics.numTargetedMultipleShards, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumTargetedAllShards(), expectedMetrics.numTargetedAllShards);
        ASSERT_EQ(*metrics.getPercentageOfTargetedAllShards(),
                  calculatePercentage(expectedMetrics.numTargetedAllShards, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumDispatchedByRange(), expectedMetrics.numDispatchedByRange);

        ASSERT_EQ(*metrics.getNumShardKeyUpdates(), expectedMetrics.numShardKeyUpdates);
        ASSERT_EQ(*metrics.getPercentageOfShardKeyUpdates(),
                  calculatePercentage(expectedMetrics.numShardKeyUpdates, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumSingleWritesWithoutShardKey(),
                  expectedMetrics.numSingleWritesWithoutShardKey);
        ASSERT_EQ(
            *metrics.getPercentageOfSingleWritesWithoutShardKey(),
            calculatePercentage(expectedMetrics.numSingleWritesWithoutShardKey, expectedNumTotal));

        ASSERT_EQ(*metrics.getNumMultiWritesWithoutShardKey(),
                  expectedMetrics.numMultiWritesWithoutShardKey);
        ASSERT_EQ(
            *metrics.getPercentageOfMultiWritesWithoutShardKey(),
            calculatePercentage(expectedMetrics.numMultiWritesWithoutShardKey, expectedNumTotal));
    }

    const NamespaceString nss{"testDb", "testColl"};
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

    ASSERT_FALSE(metrics.getNumTargetedOneShard());
    ASSERT_FALSE(metrics.getNumTargetedMultipleShards());
    ASSERT_FALSE(metrics.getNumTargetedAllShards());
    ASSERT_FALSE(metrics.getNumDispatchedByRange());
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

    ASSERT_FALSE(metrics.getNumTargetedOneShard());
    ASSERT_FALSE(metrics.getNumTargetedMultipleShards());
    ASSERT_FALSE(metrics.getNumTargetedAllShards());
    ASSERT_FALSE(metrics.getNumDispatchedByRange());
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

    // Add three update queries.
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

    // Add two delete queries.
    auto filter3 = BSON("a.x" << 3);
    auto deleteOp3 = write_ops::DeleteOpEntry(filter3, false /* multi */);
    auto filter4 = BSON("a.x" << 4);
    auto deleteOp4 = write_ops::DeleteOpEntry(filter4, true /* multi */);
    writeDistributionCalculator.addQuery(operationContext(),
                                         makeSampledDeleteQueryDocument({deleteOp3, deleteOp4}));

    // Add one findAndModify query.
    auto filter5 = BSON("a.x" << 5);
    auto updateMod = write_ops::UpdateModification(BSON("$set" << BSON("a.x" << 3)));
    writeDistributionCalculator.addQuery(
        operationContext(),
        makeSampledFindAndModifyQueryDocument(
            filter5, updateMod, false /* upsert */, false /* remove */));

    auto metrics = writeDistributionCalculator.getMetrics();
    auto sampleSize = metrics.getSampleSize();
    ASSERT_EQ(sampleSize.getTotal(), 6);
    ASSERT_EQ(sampleSize.getUpdate(), 3);
    ASSERT_EQ(sampleSize.getDelete(), 2);
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
                             const std::vector<int64_t>& numDispatchedByRange,
                             bool hasSimpleCollation,
                             bool hasCollatableType) const {
        ReadTargetMetricsBundle metrics;
        if (hasSimpleCollation || !hasCollatableType) {
            metrics.numTargetedOneShard = 1;
        } else {
            metrics.numTargetedMultipleShards = 1;
        }
        metrics.numDispatchedByRange = numDispatchedByRange;
        assertTargetMetricsForReadQuery(targeter, queryDoc, metrics);
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
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    for (const auto& filter : filters) {
        assertTargetMetrics(targeter,
                            makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);
    }
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityNotOrdered) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("b.y"
                       << "A"
                       << "a.x" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityAdditionalFields) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0 << "a.x" << 100 << "b.y"
                             << "A");
    auto numDispatchedByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyContainsCollatableFields) {
    auto hasSimpleCollation = false;
    auto hasCollatableType = true;

    auto assertMetrics = [&](const ChunkSplitInfo& chunkSplitInfo,
                             const BSONObj& filter,
                             const std::vector<int64_t>& numDispatchedByRange) {
        // The collection has a non-simple default collation and the query specifies an empty
        // collation.
        auto targeter0 = makeCollectionRoutingInfoTargeter(
            chunkSplitInfo,
            uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                                ->makeFromBSON(caseInsensitiveCollation)));
        assertTargetMetrics(
            targeter0,
            makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
            numDispatchedByRange,
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
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);

        // The collection doesn't have a default collation and the query specifies a non-simple
        // collation.
        auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfo);
        assertTargetMetrics(targeter2,
                            makeSampledReadQueryDocument(getRandomSampledReadCommandName(),
                                                         filter,
                                                         caseInsensitiveCollation),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);
    };

    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 0});
    assertMetrics(chunkSplitInfoRangeSharding0, filter, numDispatchedByRange);
    assertMetrics(chunkSplitInfoHashedSharding0, filter, numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyDoesNotContainCollatableFields) {
    auto filter = BSON("a.x" << -100 << "b.y" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({1, 0, 0});
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
        numDispatchedByRange,
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
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a non-simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter2,
                        makeSampledReadQueryDocument(
                            getRandomSampledReadCommandName(), filter, caseInsensitiveCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySimpleCollation) {
    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
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
        numDispatchedByRange,
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
        numDispatchedByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter2,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, simpleCollation),
        numDispatchedByRange,
        hasSimpleCollation,
        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies an empty
    // collation.
    auto targeter3 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter3,
        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter, emptyCollation),
        numDispatchedByRange,
        hasSimpleCollation,
        hasCollatableType);
}

TEST_F(ReadDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    auto filter = BSON("a" << -100);  // The hash of -100 is -1979677326953392702LL.
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;

    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

class ReadDistributionFilterByShardKeyRangeTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             const std::vector<int64_t>& numDispatchedByRange) const {
        ReadTargetMetricsBundle metrics;
        metrics.numTargetedMultipleShards = 1;
        metrics.numDispatchedByRange = numDispatchedByRange;
        assertTargetMetricsForReadQuery(targeter, queryDoc, metrics);
    }

    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc) const {
        ReadTargetMetricsBundle metrics;
        metrics.numTargetedAllShards = 1;
        metrics.numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
        assertTargetMetricsForReadQuery(targeter, queryDoc, metrics);
    }
};

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    auto filter = BSON("a" << BSON("x" << 0));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMinKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 1));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << 2));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 1});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeNoMinOrMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -3 << "$lt" << 3));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, FullShardKeyRange) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -4 << "$lt" << 4) << "b.y"
                             << BSON("$gte"
                                     << "A"
                                     << "$lt"
                                     << "Z"));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
}

TEST_F(ReadDistributionFilterByShardKeyRangeTest, ShardKeyNonEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$ne" << 5));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
    assertTargetMetrics(targeter,
                        makeSampledReadQueryDocument(getRandomSampledReadCommandName(), filter),
                        numDispatchedByRange);
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
        ReadTargetMetricsBundle metrics;
        metrics.numTargetedAllShards = 1;
        metrics.numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
        assertTargetMetricsForReadQuery(targeter, queryDoc, metrics);
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

class WriteDistributionFilterByShardKeyEqualityTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             const std::vector<int64_t>& numDispatchedByRange,
                             bool hasSimpleCollation,
                             bool hasCollatableType) const {
        WriteTargetMetricsBundle metrics;
        if (hasSimpleCollation || !hasCollatableType) {
            metrics.numTargetedOneShard = 1;
        } else {
            metrics.numTargetedMultipleShards = 1;
        }
        metrics.numDispatchedByRange = numDispatchedByRange;
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    // For a write that filters by shard key equality, the targeting metrics do not depend on
    // whether it is an upsert or multi.

    SampledQueryDocument makeSampledUpdateQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        const BSONObj& collation = BSONObj()) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(getRandomBool());
        updateOp.setUpsert(getRandomBool());
        updateOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }

    SampledQueryDocument makeSampledDeleteQueryDocument(
        const BSONObj& filter, const BSONObj& collation = BSONObj()) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, getRandomBool() /* multi */);
        deleteOp.setCollation(collation);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp});
    }

    SampledQueryDocument makeSampledFindAndModifyQueryDocument(
        const BSONObj& filter,
        const BSONObj& updateMod,
        const BSONObj& collation = BSONObj()) const {

        return ReadWriteDistributionTest::makeSampledFindAndModifyQueryDocument(
            filter,
            updateMod,
            getRandomBool() /* upsert */,
            getRandomBool() /* remove */,
            collation);
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
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    for (const auto& filter : filters) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledDeleteQueryDocument(filter),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);
        assertTargetMetrics(targeter,
                            makeSampledFindAndModifyQueryDocument(filter, updateMod),
                            numDispatchedByRange,
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
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityAdditionalFields) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0 << "a.x" << 100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numDispatchedByRange = std::vector<int64_t>({0, 0, 1});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numDispatchedByRange,
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
                             const std::vector<int64_t>& numDispatchedByRange) {
        // The collection has a non-simple default collation and the query specifies an empty
        // collation.
        auto targeter0 = makeCollectionRoutingInfoTargeter(
            chunkSplitInfo,
            uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                                ->makeFromBSON(caseInsensitiveCollation)));
        assertTargetMetrics(targeter0,
                            makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);

        // The collection has a simple default collation and the query specifies a non-simple
        // collation.
        auto targeter1 = makeCollectionRoutingInfoTargeter(
            chunkSplitInfo,
            uassertStatusOK(
                CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
        assertTargetMetrics(targeter1,
                            makeSampledDeleteQueryDocument(filter, caseInsensitiveCollation),
                            numDispatchedByRange,
                            hasSimpleCollation,
                            hasCollatableType);

        // The collection doesn't have a default collation and the query specifies a non-simple
        // collation.
        auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfo);
        assertTargetMetrics(
            targeter2,
            makeSampledFindAndModifyQueryDocument(filter, updateMod, caseInsensitiveCollation),
            numDispatchedByRange,
            hasSimpleCollation,
            hasCollatableType);
    };

    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 0});
    assertMetrics(chunkSplitInfoRangeSharding0, filter, updateMod, numDispatchedByRange);
    assertMetrics(chunkSplitInfoHashedSharding0, filter, updateMod, numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest,
       ShardKeyEqualityNonSimpleCollation_ShardKeyDoesNotContainCollatableFields) {
    auto filter = BSON("a.x" << -100 << "b.y" << 0);
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numDispatchedByRange = std::vector<int64_t>({1, 0, 0});
    auto hasSimpleCollation = false;
    auto hasCollatableType = false;

    // The collection has a non-simple default collation and the query specifies an empty collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(targeter0,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection has a simple default collation and the query specifies a non-simple collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(targeter1,
                        makeSampledDeleteQueryDocument(filter, caseInsensitiveCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a non-simple
    // collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(
        targeter2,
        makeSampledFindAndModifyQueryDocument(filter, updateMod, caseInsensitiveCollation),
        numDispatchedByRange,
        hasSimpleCollation,
        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualitySimpleCollation) {
    auto filter = BSON("a.x" << -100 << "b.y"
                             << "A");
    auto updateMod = BSON("$set" << BSON("c" << -100));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = true;

    // The collection has a simple default collation and the query specifies an empty collation.
    auto targeter0 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(
            CollatorFactoryInterface::get(getServiceContext())->makeFromBSON(simpleCollation)));
    assertTargetMetrics(targeter0,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection has a non-simple default collation and the query specifies a simple collation.
    auto targeter1 = makeCollectionRoutingInfoTargeter(
        chunkSplitInfoRangeSharding0,
        uassertStatusOK(CollatorFactoryInterface::get(getServiceContext())
                            ->makeFromBSON(caseInsensitiveCollation)));
    assertTargetMetrics(targeter1,
                        makeSampledDeleteQueryDocument(filter, simpleCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies a simple collation.
    auto targeter2 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter2,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod, simpleCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);

    // The collection doesn't have a default collation and the query specifies an empty collation.
    auto targeter3 = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    assertTargetMetrics(targeter3,
                        makeSampledUpdateQueryDocument(filter, updateMod, emptyCollation),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

TEST_F(WriteDistributionFilterByShardKeyEqualityTest, ShardKeyEqualityHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    auto filter = BSON("a" << -100);  // The hash of -100 is -1979677326953392702LL.
    auto updateMod = BSON("$set" << BSON("c" << 100));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    auto hasSimpleCollation = true;
    auto hasCollatableType = false;

    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledDeleteQueryDocument(filter),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        numDispatchedByRange,
                        hasSimpleCollation,
                        hasCollatableType);
}

class WriteDistributionFilterByShardKeyRangeTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi,
                             const std::vector<int64_t>& numDispatchedByRange) const {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedMultipleShards = 1;
        metrics.numDispatchedByRange = numDispatchedByRange;
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    }

    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi) const {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedAllShards = 1;
        metrics.numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
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

    SampledQueryDocument makeSampledDeleteQueryDocument(const BSONObj& filter, bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp});
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
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixEqualityNotDotted) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding1);
    auto filter = BSON("a" << BSON("x" << 0));
    auto updateMod = BSON("$set" << BSON("c" << 0));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMinKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 1));
    auto updateMod = BSON("$set" << BSON("c" << 1));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << 2));
    auto updateMod = BSON("$set" << BSON("c" << 2));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyPrefixRangeNoMinOrMaxKey) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -3 << "$lt" << 3));
    auto updateMod = BSON("$set" << BSON("c" << 3));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, FullShardKeyRange) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$gte" << -4 << "$lt" << 4) << "b.y"
                             << BSON("$gte"
                                     << "A"
                                     << "$lt"
                                     << "Z"));
    auto updateMod = BSON("$set" << BSON("c" << 4));
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyNonEquality) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$ne" << 5));
    auto updateMod = BSON("$set" << BSON("c" << 5));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(targeter,
                            makeSampledUpdateQueryDocument(filter, updateMod, multi),
                            multi,
                            numDispatchedByRange);
        assertTargetMetrics(
            targeter, makeSampledDeleteQueryDocument(filter, multi), multi, numDispatchedByRange);
    }
    assertTargetMetrics(targeter,
                        makeSampledFindAndModifyQueryDocument(filter, updateMod),
                        false /* multi */,
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeTest, ShardKeyRangeHashed) {
    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoHashedSharding2);
    // For hashed sharding, range queries always target all shards and chunks.
    auto filter = BSON("a" << BSON("$gte" << -100));
    auto updateMod = BSON("$set" << BSON("c" << 5));
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
    for (auto& multi : {true, false}) {
        assertTargetMetrics(
            targeter, makeSampledUpdateQueryDocument(filter, updateMod, multi), multi);
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
    }
    assertTargetMetrics(
        targeter, makeSampledFindAndModifyQueryDocument(filter, updateMod), false /* multi */);
}

class WriteDistributionFilterByShardKeyRangeReplacementUpdateTest
    : public ReadWriteDistributionTest {
protected:
    SampledQueryDocument makeSampledUpdateQueryDocument(const BSONObj& filter,
                                                        const BSONObj& updateMod,
                                                        bool upsert) const {
        auto updateOp = write_ops::UpdateOpEntry(filter, write_ops::UpdateModification(updateMod));
        updateOp.setMulti(false);  // replacement-style update cannot be multi.
        updateOp.setUpsert(upsert);
        return ReadWriteDistributionTest::makeSampledUpdateQueryDocument({updateOp});
    }
};

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, NotUpsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc,
                                   const std::vector<int64_t> numDispatchedByRange) {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedOneShard = 1;
        metrics.numDispatchedByRange = numDispatchedByRange;
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0));
    auto updateMod = BSON("a" << BSON("x" << 0) << "b"
                              << BSON("y"
                                      << "A")
                              << "c" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */),
                        numDispatchedByRange);
}

TEST_F(WriteDistributionFilterByShardKeyRangeReplacementUpdateTest, Upsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc,
                                   const std::vector<int64_t> numDispatchedByRange) {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedMultipleShards = 1;
        metrics.numDispatchedByRange = numDispatchedByRange;
        metrics.numSingleWritesWithoutShardKey = 1;
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("a.x" << BSON("$lt" << 0));
    auto updateMod = BSON("a" << BSON("x" << 0) << "b"
                              << BSON("y"
                                      << "A")
                              << "c" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({1, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, true /* upsert */),
                        numDispatchedByRange);
}

class WriteDistributionNotFilterByShardKeyTest : public ReadWriteDistributionTest {
protected:
    void assertTargetMetrics(const CollectionRoutingInfoTargeter& targeter,
                             const SampledQueryDocument& queryDoc,
                             bool multi) const {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedAllShards = 1;
        metrics.numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
        if (multi) {
            metrics.numMultiWritesWithoutShardKey = 1;
        } else {
            metrics.numSingleWritesWithoutShardKey = 1;
        }
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
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

    SampledQueryDocument makeSampledDeleteQueryDocument(const BSONObj& filter, bool multi) const {
        auto deleteOp = write_ops::DeleteOpEntry(filter, multi);
        return ReadWriteDistributionTest::makeSampledDeleteQueryDocument({deleteOp});
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
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
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
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
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
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
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
        assertTargetMetrics(targeter, makeSampledDeleteQueryDocument(filter, multi), multi);
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
};

TEST_F(WriteDistributionNotFilterByShardKeyReplacementUpdateTest, NotUpsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc,
                                   const std::vector<int64_t> numDispatchedByRange) {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedOneShard = 1;
        metrics.numDispatchedByRange = numDispatchedByRange;
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0);
    auto updateMod = BSON("_id" << 0 << "a" << BSON("x" << 0) << "b"
                                << BSON("y"
                                        << "A")
                                << "c" << 0);
    auto numDispatchedByRange = std::vector<int64_t>({0, 1, 0});
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, false /* upsert */),
                        numDispatchedByRange);
}

TEST_F(WriteDistributionNotFilterByShardKeyReplacementUpdateTest, Upsert) {
    auto assertTargetMetrics = [&](const CollectionRoutingInfoTargeter& targeter,
                                   const SampledQueryDocument& queryDoc) {
        WriteTargetMetricsBundle metrics;
        metrics.numTargetedAllShards = 1;
        metrics.numDispatchedByRange = std::vector<int64_t>({1, 1, 1});
        metrics.numSingleWritesWithoutShardKey = 1;
        assertTargetMetricsForWriteQuery(targeter, queryDoc, metrics);
    };

    auto targeter = makeCollectionRoutingInfoTargeter(chunkSplitInfoRangeSharding0);
    auto filter = BSON("_id" << 0);
    auto updateMod = BSON("_id" << 0 << "a" << BSON("x" << 0) << "b"
                                << BSON("y"
                                        << "A")
                                << "c" << 0);
    assertTargetMetrics(targeter,
                        makeSampledUpdateQueryDocument(filter, updateMod, true /* upsert */));
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
    metrics0.setNumTargetedOneShard(1);
    metrics0.setNumTargetedMultipleShards(2);
    metrics0.setNumTargetedAllShards(3);
    metrics0.setNumDispatchedByRange(std::vector<int64_t>{1, 2, 3});

    ReadDistributionMetrics metrics1;

    ReadSampleSize sampleSize1;
    sampleSize1.setFind(10);
    sampleSize1.setAggregate(20);
    sampleSize1.setCount(30);
    sampleSize1.setDistinct(40);
    auto numTotal1 = 10 + 20 + 30 + 40;
    sampleSize1.setTotal(numTotal1);

    metrics1.setSampleSize(sampleSize1);
    metrics1.setNumTargetedOneShard(10);
    metrics1.setNumTargetedMultipleShards(20);
    metrics1.setNumTargetedAllShards(30);
    metrics1.setNumDispatchedByRange(std::vector<int64_t>{10, 20, 30});

    ReadDistributionMetrics expectedMetrics;

    ReadSampleSize expectedSampleSize;
    expectedSampleSize.setFind(11);
    expectedSampleSize.setAggregate(22);
    expectedSampleSize.setCount(33);
    expectedSampleSize.setDistinct(44);
    auto expectedNumtotal = 11 + 22 + 33 + 44;
    expectedSampleSize.setTotal(expectedNumtotal);
    expectedMetrics.setSampleSize(expectedSampleSize);

    expectedMetrics.setNumTargetedOneShard(11);
    expectedMetrics.setPercentageOfTargetedOneShard(calculatePercentage(11, expectedNumtotal));

    expectedMetrics.setNumTargetedMultipleShards(22);
    expectedMetrics.setPercentageOfTargetedMultipleShards(
        calculatePercentage(22, expectedNumtotal));

    expectedMetrics.setNumTargetedAllShards(33);
    expectedMetrics.setPercentageOfTargetedAllShards(calculatePercentage(33, expectedNumtotal));

    expectedMetrics.setNumDispatchedByRange(std::vector<int64_t>{11, 22, 33});

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

    metrics0.setNumTargetedOneShard(1);
    metrics0.setNumTargetedMultipleShards(2);
    metrics0.setNumTargetedAllShards(3);
    metrics0.setNumDispatchedByRange(std::vector<int64_t>{1, 2, 3});
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

    metrics1.setNumTargetedOneShard(10);
    metrics1.setNumTargetedMultipleShards(20);
    metrics1.setNumTargetedAllShards(30);
    metrics1.setNumDispatchedByRange(std::vector<int64_t>{10, 20, 30});
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

    expectedMetrics.setNumTargetedOneShard(11);
    expectedMetrics.setPercentageOfTargetedOneShard(calculatePercentage(11, expectedNumtotal));

    expectedMetrics.setNumTargetedMultipleShards(22);
    expectedMetrics.setPercentageOfTargetedMultipleShards(
        calculatePercentage(22, expectedNumtotal));

    expectedMetrics.setNumTargetedAllShards(33);
    expectedMetrics.setPercentageOfTargetedAllShards(calculatePercentage(33, expectedNumtotal));

    expectedMetrics.setNumDispatchedByRange(std::vector<int64_t>{11, 22, 33});

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
