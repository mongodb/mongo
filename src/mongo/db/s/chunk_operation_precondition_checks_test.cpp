// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/chunk_operation_precondition_checks.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>
#include <vector>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kPattern = "key";
const KeyPattern kShardKeyPattern(BSON(kPattern << 1));
// Must match ShardServerTestFixture::kMyShardName so that ShardingState::shardId() returns the
// same value used as _thisShardId when constructing CollectionMetadata. Otherwise
// getNextChunk would filter against a different shardId than the StaleConfigInfo emitted by the
// helpers.
const ShardId kThisShard{"myShardName"};
const ShardId kOtherShard{"otherShard"};

class ChunkOperationPreconditionChecksTest : public ShardServerTestFixture {
protected:
    /**
     * Builds a CollectionMetadata containing the supplied chunks. Each chunk is a (range, shardId)
     * pair. The supplied chunks must collectively cover the entire shard key space from MinKey to
     * MaxKey, be contiguous, and be listed in increasing key order. The allowMigrations flag is
     * forwarded to the underlying RoutingTableHistory so it can be read back via
     * CollectionMetadata::allowMigrations().
     */
    static CollectionMetadata makeMetadataWithChunks(
        const std::vector<std::pair<ChunkRange, ShardId>>& chunks, bool allowMigrations = true) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(1, 1);
        const UUID uuid = UUID::gen();

        std::vector<ChunkType> chunkTypes;
        chunkTypes.reserve(chunks.size());
        ChunkVersion version({epoch, timestamp}, {1, 0});
        for (const auto& [range, shardId] : chunks) {
            chunkTypes.emplace_back(uuid, range, version, shardId);
            version.incMajor();
        }

        auto rt = RoutingTableHistory::makeNew(kNss,
                                               uuid,
                                               kShardKeyPattern,
                                               false /* unsplittable */,
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               epoch,
                                               timestamp,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               allowMigrations,
                                               std::move(chunkTypes));

        return CollectionMetadata(
            CurrentChunkManager(makeStandaloneRoutingTableHistory(std::move(rt))), kThisShard);
    }

    /**
     * Builds a single-chunk CollectionMetadata spanning the full shard key space and owned by the
     * given shard.
     */
    static CollectionMetadata makeSingleChunkMetadata(const ShardId& owner) {
        return makeMetadataWithChunks(
            {{ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << MAXKEY)), owner}});
    }

    static ChunkRange range(int min, int max) {
        return ChunkRange(BSON(kPattern << min), BSON(kPattern << max));
    }

    /**
     * Validates the StaleConfigInfo payload attached to a StaleConfig exception thrown by the
     * helpers under test. Both helpers populate the payload identically: receivedVersion is the
     * IGNORED placeholder, wantedVersion is the placement version derived from the metadata,
     * shardId is the local shard's ShardingState shardId.
     */
    static void assertStaleConfigPayload(const DBException& ex,
                                         const CollectionMetadata& metadata) {
        const auto exInfo = ex.extraInfo<StaleConfigInfo>();
        ASSERT(exInfo);
        ASSERT_EQ(kNss, exInfo->getNss());
        ASSERT_EQ(ShardVersionFactory::make(ChunkVersion::IGNORED()), exInfo->getVersionReceived());
        ASSERT(exInfo->getVersionWanted().has_value());
        ASSERT_EQ(ShardVersionFactory::make(metadata), *exInfo->getVersionWanted());
        ASSERT_EQ(kThisShard, exInfo->getShardId());
    }

private:
    boost::optional<unittest::ServerParameterGuard> _featureFlagScope;
};

//
// checkChunkMatchesRange (split coordinator)
//

TEST_F(ChunkOperationPreconditionChecksTest, MatchesExactChunkOwnedByThisShard_Succeeds) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkChunkMatchesRange(operationContext(), kNss, metadata, range(10, 20));
}

TEST_F(ChunkOperationPreconditionChecksTest, RangeOwnedByOtherShard_ThrowsStaleConfigNotOwned) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kThisShard},
        {range(10, 20), kOtherShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kThisShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkChunkMatchesRange(operationContext(), kNss, metadata, range(10, 20)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeIsSubsetOfChunk_ThrowsStaleConfigBoundsDontExist) {
    // [10, 30) is owned by this shard. Querying [15, 25) finds the chunk starting at 10, so the
    // "is not owned" assertion passes; the subsequent "bounds match" assertion fails because the
    // chunk's bounds are [10, 30), not [15, 25).
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkChunkMatchesRange(operationContext(), kNss, metadata, range(15, 25)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeIsSupersetOfChunk_ThrowsStaleConfigBoundsDontExist) {
    // Two contiguous chunks [10, 20) and [20, 30) both owned by this shard. Querying [10, 30)
    // finds the chunk starting at 10 (so the ownership step passes) but its bounds are [10, 20),
    // not [10, 30); the second assertion fires with "do not exist".
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkChunkMatchesRange(operationContext(), kNss, metadata, range(10, 30)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "do not exist");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest, ChunkMatches_RangeMinNotAtBoundary_ThrowsStaleConfig) {
    // Querying [12, 20) where this shard owns [10, 20): getNextChunk(12) returns the
    // chunk starting at 10, whose min does not equal 12, so the "is not owned" assertion fires.
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkChunkMatchesRange(operationContext(), kNss, metadata, range(12, 20)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       ChunkMatches_RangeBeyondMetadata_ThrowsStaleConfigNotOwned) {
    // All chunks owned by this shard lie below 100. Querying [100, 200) gets no result from
    // getNextChunk, so the first assertion ("is not owned") fires immediately.
    const auto metadata = makeSingleChunkMetadata(kOtherShard);

    ASSERT_THROWS_WITH_CHECK(
        checkChunkMatchesRange(operationContext(), kNss, metadata, range(100, 200)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

//
// checkRangeOwnership (merge coordinator)
//

TEST_F(ChunkOperationPreconditionChecksTest, SingleChunkExactlyFillsRange_Succeeds) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkRangeOwnership(operationContext(), kNss, metadata, range(10, 20));
}

TEST_F(ChunkOperationPreconditionChecksTest, TwoContiguousChunksFillRange_Succeeds) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkRangeOwnership(operationContext(), kNss, metadata, range(10, 30));
}

TEST_F(ChunkOperationPreconditionChecksTest, ThreeContiguousChunksFillRange_Succeeds) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 30), kThisShard},
        {range(30, 40), kThisShard},
        {ChunkRange(BSON(kPattern << 40), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkRangeOwnership(operationContext(), kNss, metadata, range(10, 40));
}

TEST_F(ChunkOperationPreconditionChecksTest, GapToOtherShardInMiddle_ThrowsStaleConfigNotOwned) {
    // Querying [10, 40) where this shard owns [10, 20) and [30, 40) but not [20, 30):
    // after [10, 20) is consumed, getNextChunk(20) skips the [20, 30) chunk owned by
    // the other shard and returns [30, 40), whose min (30) does not equal the expected boundary
    // (20). The helper throws "is not owned by this shard".
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 30), kOtherShard},
        {range(30, 40), kThisShard},
        {ChunkRange(BSON(kPattern << 40), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(checkRangeOwnership(operationContext(), kNss, metadata, range(10, 40)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
                                 assertStaleConfigPayload(ex, metadata);
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest, RangeStartsAtNonOwnedChunk_ThrowsStaleConfigNotOwned) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kOtherShard},
        {range(20, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(checkRangeOwnership(operationContext(), kNss, metadata, range(10, 30)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
                                 assertStaleConfigPayload(ex, metadata);
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeOwnership_RangeMinNotAtBoundary_ThrowsStaleConfig) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(checkRangeOwnership(operationContext(), kNss, metadata, range(12, 20)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "is not owned by this shard");
                                 assertStaleConfigPayload(ex, metadata);
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeExtendsBeyondOwnedChunks_ThrowsStaleConfigPartialFill) {
    // Range [10, 30) extends past the single owned chunk [10, 20). The loop walks [10, 20), then
    // looks for the next chunk after 20. The only owned chunk has already been consumed, so
    // getNextChunk returns either nothing (if no more chunks exist after the range) or a chunk
    // whose min differs from 20; either way the "is not owned" assertion fires.
    //
    // To hit the "does not contain a sequence of chunks that exactly fills the range" branch we
    // need the chunks owned by this shard to be contiguous with the query range start, and end
    // before the query range max, with the next chunk after the owned run also belonging to
    // this shard but past the query range max — so the inner loop exits cleanly but the final
    // boundary check rejects the partial fill.
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 25), kThisShard},
        {ChunkRange(BSON(kPattern << 25), BSON(kPattern << MAXKEY)), kThisShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkRangeOwnership(operationContext(), kNss, metadata, range(10, 30)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(
                ex.reason(), "does not contain a sequence of chunks that exactly fills the range");
            assertStaleConfigPayload(ex, metadata);
        });
}

//
// checkShardKeyPattern
//

TEST_F(ChunkOperationPreconditionChecksTest, ShardKeyPattern_ValidBounds_Succeeds) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    checkShardKeyPattern(operationContext(), kNss, metadata, range(10, 20));
}

TEST_F(ChunkOperationPreconditionChecksTest,
       ShardKeyPattern_MinDoesNotMatchPattern_ThrowsStaleConfig) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    // Key pattern is {key: 1}; a min using a field name that sorts before "key" preserves
    // the ChunkRange::min < max invariant while still failing isValidKey.
    const ChunkRange invalidRange(BSON("aaa" << 5), BSON(kPattern << 20));

    ASSERT_THROWS_WITH_CHECK(checkShardKeyPattern(operationContext(), kNss, metadata, invalidRange),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "is not valid for collection");
                                 assertStaleConfigPayload(ex, metadata);
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       ShardKeyPattern_MaxDoesNotMatchPattern_ThrowsStaleConfig) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    // Max uses a field name that sorts after "key" so min < max still holds.
    const ChunkRange invalidRange(BSON(kPattern << 10), BSON("zzz" << 20));

    ASSERT_THROWS_WITH_CHECK(checkShardKeyPattern(operationContext(), kNss, metadata, invalidRange),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "is not valid for collection");
                                 assertStaleConfigPayload(ex, metadata);
                             });
}

//
// checkRangeWithinChunk
//

TEST_F(ChunkOperationPreconditionChecksTest, RangeWithinChunk_RangeEqualsOwnedChunk_Succeeds) {
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {ChunkRange(BSON(kPattern << 20), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkRangeWithinChunk(operationContext(), kNss, metadata, range(10, 20));
}

TEST_F(ChunkOperationPreconditionChecksTest, RangeWithinChunk_StrictSubsetOfOwnedChunk_Succeeds) {
    // Owned chunk [10, 30) covers strict subset [15, 25).
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    checkRangeWithinChunk(operationContext(), kNss, metadata, range(15, 25));
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeWithinChunk_RangeExtendsBeyondOwnedChunk_ThrowsStaleConfig) {
    // Owned chunk [10, 20); the next owned chunk is [10, 20) itself. covers([10, 25)) is false
    // because max 20 < 25.
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kThisShard},
        {range(20, 30), kOtherShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkRangeWithinChunk(operationContext(), kNss, metadata, range(10, 25)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(),
                                   "is not contained within a chunk owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeWithinChunk_RangeStartsBeforeOwnedChunk_ThrowsStaleConfig) {
    // getNextChunk(15) skips other-shard chunks and returns the owned [20, 30), whose
    // min (20) does not cover the query range [15, 25).
    const auto metadata = makeMetadataWithChunks({
        {ChunkRange(BSON(kPattern << MINKEY), BSON(kPattern << 10)), kOtherShard},
        {range(10, 20), kOtherShard},
        {range(20, 30), kThisShard},
        {ChunkRange(BSON(kPattern << 30), BSON(kPattern << MAXKEY)), kOtherShard},
    });

    ASSERT_THROWS_WITH_CHECK(
        checkRangeWithinChunk(operationContext(), kNss, metadata, range(15, 25)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(),
                                   "is not contained within a chunk owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       RangeWithinChunk_NoOwnedChunksAfterRangeMin_ThrowsStaleConfig) {
    // No owned chunk at or after key 100; getNextChunk returns false, first assertion
    // fires.
    const auto metadata = makeSingleChunkMetadata(kOtherShard);

    ASSERT_THROWS_WITH_CHECK(
        checkRangeWithinChunk(operationContext(), kNss, metadata, range(100, 200)),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(),
                                   "is not contained within a chunk owned by this shard");
            assertStaleConfigPayload(ex, metadata);
        });
}

//
// validateSplitPoints
//

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_SingleKeyInsideChunk_Succeeds) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    validateSplitPoints(operationContext(), kNss, metadata, range(10, 20), {BSON(kPattern << 15)});
}

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_MultipleStrictlyIncreasing_Succeeds) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    validateSplitPoints(operationContext(),
                        kNss,
                        metadata,
                        range(10, 20),
                        {BSON(kPattern << 12), BSON(kPattern << 15), BSON(kPattern << 18)});
}

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_KeyEqualToChunkMax_Succeeds) {
    // The helper explicitly allows endKey == chunkRange.getMax() ("not contained" is only
    // asserted when the key is also not equal to the upper bound).
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    validateSplitPoints(operationContext(), kNss, metadata, range(10, 20), {BSON(kPattern << 20)});
}

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_KeyEqualToChunkMin_ThrowsLowerBound) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    ASSERT_THROWS_WITH_CHECK(
        validateSplitPoints(
            operationContext(), kNss, metadata, range(10, 20), {BSON(kPattern << 10)}),
        ExceptionFor<ErrorCodes::InvalidOptions>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "Split on lower bound");
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       SplitPoints_DecreasingSequence_ThrowsStrictlyIncreasing) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    ASSERT_THROWS_WITH_CHECK(validateSplitPoints(operationContext(),
                                                 kNss,
                                                 metadata,
                                                 range(10, 20),
                                                 {BSON(kPattern << 15), BSON(kPattern << 13)}),
                             ExceptionFor<ErrorCodes::InvalidOptions>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "strictly increasing order");
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_DuplicatedKeys_ThrowsLowerBound) {
    // After the first iteration, startKey is set to 15. The second key 15 satisfies the "strictly
    // increasing" assertion (>= startKey) but fails the "different from startKey" one, which is
    // emitted as the "Split on lower bound" error.
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    ASSERT_THROWS_WITH_CHECK(validateSplitPoints(operationContext(),
                                                 kNss,
                                                 metadata,
                                                 range(10, 20),
                                                 {BSON(kPattern << 15), BSON(kPattern << 15)}),
                             ExceptionFor<ErrorCodes::InvalidOptions>,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.reason(), "Split on lower bound");
                             });
}

TEST_F(ChunkOperationPreconditionChecksTest, SplitPoints_KeyOutsideChunk_ThrowsNotContained) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    ASSERT_THROWS_WITH_CHECK(
        validateSplitPoints(
            operationContext(), kNss, metadata, range(10, 20), {BSON(kPattern << 25)}),
        ExceptionFor<ErrorCodes::InvalidOptions>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "not contained within chunk");
        });
}

//
// checkCollectionIdentity
//

TEST_F(ChunkOperationPreconditionChecksTest, MetadataIdentity_MatchingEpochAndTimestamp_Succeeds) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    const auto placementVersion = metadata.getShardPlacementVersion();

    checkCollectionIdentity(operationContext(),
                            kNss,
                            placementVersion.epoch(),
                            placementVersion.getTimestamp(),
                            metadata);
}

TEST_F(ChunkOperationPreconditionChecksTest, MetadataIdentity_NoExpectedEpochOrTimestamp_Succeeds) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);

    checkCollectionIdentity(operationContext(), kNss, boost::none, boost::none, metadata);
}

TEST_F(ChunkOperationPreconditionChecksTest, MetadataIdentity_EpochMismatch_ThrowsStaleConfig) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    const auto placementVersion = metadata.getShardPlacementVersion();

    ASSERT_THROWS_WITH_CHECK(
        checkCollectionIdentity(
            operationContext(), kNss, OID::gen(), placementVersion.getTimestamp(), metadata),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "has changed since operation was sent");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest, MetadataIdentity_TimestampMismatch_ThrowsStaleConfig) {
    const auto metadata = makeSingleChunkMetadata(kThisShard);
    const auto placementVersion = metadata.getShardPlacementVersion();

    ASSERT_THROWS_WITH_CHECK(
        checkCollectionIdentity(
            operationContext(), kNss, placementVersion.epoch(), Timestamp(99, 99), metadata),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "has changed since operation was sent");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest, MetadataIdentity_NotSharded_ThrowsStaleConfig) {
    const auto metadata = CollectionMetadata::UNTRACKED();

    ASSERT_THROWS_WITH_CHECK(
        checkCollectionIdentity(operationContext(), kNss, boost::none, boost::none, metadata),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "is not sharded");
            assertStaleConfigPayload(ex, metadata);
        });
}

TEST_F(ChunkOperationPreconditionChecksTest,
       MetadataIdentity_ThisShardOwnsNoChunks_ThrowsStaleConfig) {
    // The collection is sharded but every chunk belongs to another shard, so this shard's placement
    // major version is 0.
    const auto metadata = makeSingleChunkMetadata(kOtherShard);

    ASSERT_THROWS_WITH_CHECK(
        checkCollectionIdentity(operationContext(), kNss, boost::none, boost::none, metadata),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.reason(), "does not contain any chunks");
            assertStaleConfigPayload(ex, metadata);
        });
}

}  // namespace
}  // namespace mongo
