// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/metadata_manager.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

// A history floor used by the tests below. It sits strictly between the two per-chunk history eras
// of prepareTestData: reads at (25,0) <= T < (60,0) are inside every chunk's per-chunk history, so
// only a key that falls in a dropped chunk's range (a gap) can be refused.
const Timestamp kHistoryFloor(60, 0);

class CollectionMetadataFilteringTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
    }

    /**
     * Prepares the CSS for 'kNss' and the standalone '_manager' to have their metadata be a
     * history array populated as follows:
     *  chunk1 - [min, -100)
     *  chunk2 - [-100, 0)
     *  chunk3 - [0, 100)
     *  chunk4 - [100, max)
     *
     * and the history:
     *  time (now,75) shard0(chunk1, chunk3) shard1(chunk2, chunk4)
     *  time (75,25) shard0(chunk2, chunk4) shard1(chunk1, chunk3)
     *  time (25,0) - no history
     *
     * When 'historyFloor' is set, the installed metadata carries it, mirroring a recovery that
     * skipped expired chunk documents with that boundary.
     *
     * When 'dropExpiredChunk2' is also set, chunk2 is given an 'onCurrentShardSince' below the
     * floor and then left out of the routing table entirely, reproducing what the expired-history
     * filter does to a chunk this shard no longer owns and whose history has expired: its range
     * [-100, 0) becomes a gap.
     */
    CollectionMetadata prepareTestData(
        boost::optional<Timestamp> historyFloor = boost::none,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields = boost::none,
        bool dropExpiredChunk2 = false) {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

        const auto& chunks = [&] {
            ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

            ChunkType chunk1(uuid,
                             {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                             version,
                             {"0"});
            chunk1.setOnCurrentShardSince(Timestamp(75, 0));
            chunk1.setHistory({ChunkHistory(*chunk1.getOnCurrentShardSince(), ShardId("0")),
                               ChunkHistory(Timestamp(25, 0), ShardId("1"))});
            version.incMinor();

            ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
            chunk2.setOnCurrentShardSince(Timestamp(75, 0));
            chunk2.setHistory({ChunkHistory(*chunk2.getOnCurrentShardSince(), ShardId("1")),
                               ChunkHistory(Timestamp(25, 0), ShardId("0"))});
            version.incMinor();

            ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
            chunk3.setOnCurrentShardSince(Timestamp(75, 0));
            chunk3.setHistory({ChunkHistory(*chunk3.getOnCurrentShardSince(), ShardId("0")),
                               ChunkHistory(Timestamp(25, 0), ShardId("1"))});
            version.incMinor();

            ChunkType chunk4(uuid,
                             {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                             version,
                             {"1"});
            chunk4.setOnCurrentShardSince(Timestamp(75, 0));
            chunk4.setHistory({ChunkHistory(*chunk4.getOnCurrentShardSince(), ShardId("1")),
                               ChunkHistory(Timestamp(25, 0), ShardId("0"))});
            version.incMinor();

            if (dropExpiredChunk2) {
                return std::vector<ChunkType>{chunk1, chunk3, chunk4};
            }
            return std::vector<ChunkType>{chunk1, chunk2, chunk3, chunk4};
        }();

        auto rt = (historyFloor || dropExpiredChunk2)
            ? RoutingTableHistory::makeNewAllowingGaps(kNss,
                                                       uuid,
                                                       shardKeyPattern.getKeyPattern(),
                                                       false, /* unsplittable */
                                                       nullptr,
                                                       false,
                                                       epoch,
                                                       Timestamp(1, 1),
                                                       timeseriesFields,
                                                       boost::none /* reshardingFields */,
                                                       true,
                                                       chunks,
                                                       std::move(historyFloor))
            : RoutingTableHistory::makeNew(kNss,
                                           uuid,
                                           shardKeyPattern.getKeyPattern(),
                                           false, /* unsplittable */
                                           nullptr,
                                           false,
                                           epoch,
                                           Timestamp(1, 1),
                                           timeseriesFields,
                                           boost::none /* reshardingFields */,
                                           true,
                                           chunks);

        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(std::move(rt)));
        ASSERT_EQ(dropExpiredChunk2 ? 3 : 4, cm.numChunks());

        CollectionMetadata metadata(std::move(cm), ShardId("0"));

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(operationContext(), kNss);
            scopedCsr->setCollectionMetadata(operationContext(), metadata);
        }

        _manager = std::make_shared<MetadataManager>(getServiceContext(), kNss, metadata);

        return metadata;
    }

    // Sets the operation's read concern to a snapshot read at 'atClusterTime'.
    void setSnapshotReadConcernAt(Timestamp atClusterTime) {
        resetReadConcern();
        BSONObj readConcern =
            BSON("readConcern" << BSON("level" << "snapshot"
                                               << "atClusterTime" << atClusterTime));
        auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
        ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));
    }

    // Resets the operation's read concern to no cluster time.
    void resetReadConcern() {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs{};
    }

    // Performs the read-side shard-role acquisition for 'kNss' (the path
    // CollectionShardingRuntime::_getMetadataWithVersionCheckAt runs on) using the operation's
    // current read concern, and runs 'fn' on the resulting ownership filter while the acquisition
    // is still held.
    template <typename Fn>
    void withOwnershipFilter(const CollectionMetadata& metadata, Fn&& fn) {
        auto acq = acquireCollection(operationContext(),
                                     CollectionAcquisitionRequest::fromOpCtx(
                                         operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                     MODE_IS);
        ScopedSetShardRole scopedSetShardRole{
            operationContext(),
            kNss,
            ShardVersionFactory::make(metadata) /* shardVersion */,
            boost::none /* databaseVersion */};
        auto scopedCss =
            CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
        fn(scopedCss->getOwnershipFilter(
            operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
    }

    std::shared_ptr<MetadataManager> _manager;
};

// Verifies that right set of documents is visible
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsInTheFuture) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(100, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                 MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

// Verifies that a different set of documents is visible for a timestamp in the past
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsInThePast) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(50, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                 MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

// Verifies that when accessing too far into the past we get the stale error
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsTooFarInThePastThrowsStaleChunkHistory) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(10, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                 MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

// Below the floor, only the keys whose chunk was dropped are unanswerable. A chunk that is still
// in the routing table carries its own complete history, so it answers as usual -- the floor must
// not turn the whole read into an error.
TEST_F(CollectionMetadataFilteringTest, OwnershipFilterRefusesOnlyDroppedRangeBelowHistoryFloor) {
    const auto metadata{
        prepareTestData(kHistoryFloor, boost::none /* timeseriesFields */, true /* dropChunk2 */)};

    setSnapshotReadConcernAt(Timestamp(50, 0));
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        // Chunks still present answer from their own history, at the pre-(75,0) era.
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));

        // The dropped chunk's range is a gap. Below the floor that is not "not mine": this shard
        // may have owned it at (50, 0), so the lookup must refuse instead of silently filtering
        // the document out.
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
    });
}

// The acquisition itself must not be refused: a read below the floor is allowed to start, because
// whether it can be answered depends on the keys it actually looks up.
TEST_F(CollectionMetadataFilteringTest, AcquisitionBelowHistoryFloorIsNotRefused) {
    const auto metadata{
        prepareTestData(kHistoryFloor, boost::none /* timeseriesFields */, true /* dropChunk2 */)};

    setSnapshotReadConcernAt(Timestamp(50, 0));
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter&) {});
}

// At or above the floor, every chunk the filter could have dropped had already moved off this
// shard, so the gap left behind means "not mine" and is answered normally.
TEST_F(CollectionMetadataFilteringTest, OwnershipFilterAnswersGapAtOrAboveHistoryFloor) {
    const auto metadata{
        prepareTestData(kHistoryFloor, boost::none /* timeseriesFields */, true /* dropChunk2 */)};

    setSnapshotReadConcernAt(kHistoryFloor);
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
    });

    setSnapshotReadConcernAt(Timestamp(100, 0));
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
    });
}

// Reads at the floor boundary and above it are served: the floor only constrains strictly older
// cluster times, and the answers come from the per-chunk history as usual.
TEST_F(CollectionMetadataFilteringTest, OwnershipFilterAcceptsReadsAtOrAboveHistoryFloor) {
    const auto metadata{prepareTestData(kHistoryFloor)};

    // At the floor itself (T == F), the pre-(75,0) era answers, same as the in-window past read.
    setSnapshotReadConcernAt(kHistoryFloor);
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    });

    // Above the floor, the current era answers.
    setSnapshotReadConcernAt(Timestamp(100, 0));
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    });
}

// A floor on a routing table that is not actually missing any chunk constrains nothing: every key
// is covered by a chunk whose own history answers, even well below the floor. This is the case
// where the shard catalog recorded a floor but the filter happened to drop nothing.
TEST_F(CollectionMetadataFilteringTest, OwnershipFilterAnswersBelowFloorWhenNoChunkWasDropped) {
    const auto metadata{prepareTestData(kHistoryFloor)};

    setSnapshotReadConcernAt(Timestamp(50, 0));
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    });
}

// A current-time read carries no cluster time, so the history floor never refuses it; the filter
// answers from the current era.
TEST_F(CollectionMetadataFilteringTest, OwnershipFilterIgnoresHistoryFloorWithoutClusterTime) {
    const auto metadata{prepareTestData(kHistoryFloor)};

    resetReadConcern();
    withOwnershipFilter(metadata, [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    });
}

}  // namespace
}  // namespace mongo
