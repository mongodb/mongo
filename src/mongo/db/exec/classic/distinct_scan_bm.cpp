/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/distinct_scan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/exec/classic/distinct_scan.h"
#include "mongo/db/exec/classic/fetch.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/query_shard_server_test_fixture.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/benchmark_util.h"
#include "mongo/util/assert_util.h"

#include <iterator>
#include <memory>

#include <benchmark/benchmark.h>
#include <boost/optional/optional.hpp>

/**
 * This benchmark is designed to measure the performance of distinct scan execution in the
 * presence/absence of shard filtering. In the simple case where our index is prefixed by the
 * distinct key, the execution of this stage is proportional to the number of distinct values for
 * that key, therefore we used one collection size of 100k documents and varied the number of
 * distinct values (low, medium, and high number of distinct values- NDV).
 *
 * In the absence of shard filtering, the size of the chunk map makes no difference to distinct scan
 * execution, so we only included one "NoShardFilter" case per NDV. For the remaining cases, every
 * NDV saw several different chunk maps for the "ShardFilter" cases, identified by the
 * "OrphanDistribution" enum below.
 */
namespace mongo {
namespace {

IndexBounds makeFullScanIndexBound(StringData fieldName) {
    IndexBounds bounds;
    bounds.isSimpleRange = true;
    OrderedIntervalList oil(std::string{fieldName});
    oil.intervals = {IndexBoundsBuilder::allValues()};
    bounds.fields.push_back(oil);
    return bounds;
}

const int kMinChunkSize = 1;
const int kMaxChunkSize = 10;
const int kMinBlockSize = 1;
const int kMedBlockSize = 10;
const int kMaxBlockSize = 100;
const int kDataSetSize = 100000;
const int klowNDVNum = 100;
const int kMediumNDVNum = kDataSetSize / 100;
const int kHighNDVNum = kDataSetSize / 10;

std::vector<BSONObj> generateDataset(int nDocs) {
    std::vector<BSONObj> dataSet;
    dataSet.reserve(nDocs);
    for (int i = 0; i < nDocs; i++) {
        dataSet.push_back(BSON("_id" << i << "highNDVField" << (i % kHighNDVNum) << "mediumNDVField"
                                     << (i % kMediumNDVNum) << "lowNDVField" << (i % klowNDVNum)));
    }
    return dataSet;
}

const auto kDataSet = generateDataset(kDataSetSize);

/**
 * Enum representing the distribution of orphans in the chunk map. In the examples below we
 * illustrate the chunk map as a vector of chunks for simplicity. Note that all chunks have the same
 * size for a given test.
 *
 * Legend: |O| is an orphan chunk, |X| is a chunk owned by the current shard.
 */
enum OrphanDistribution {
    // This means a chunk map with a single trivial owned entry [MinKey, MaxKey].
    // e.g. |X|
    NoOrphans,
    // This means a chunk map with ~NDV chunks, all owned by the current shard.
    // e.g. |X|X|X|X|X|X|
    NoOrphansLargeChunkMap,
    // This means a chunk map with ~NDV chunks, where there are large gaps between orphan chunks,
    // and there are no consecutive orphan chunks.
    // e.g. |X|X|X|X|O|X|
    VerySparseOrphans,
    SparseOrphans,
    // This means a chunk map with ~NDV chunks, where we alternate between owned/unowned chunks (no
    // consecutive orphans or owned chunks).
    // e.g. |O|X|O|X|O|X|
    AlternatingChunks,
    // This means a chunk map with ~NDV chunks, where we alternate between owned/unowned blocks of
    // chunks with the same ownership.
    // e.g. |X|X|O|O|X|X|
    AlternatingBlocks,
    LargeAlternatingBlocks,
    // This means a chunk map with ~NDV chunks, where there are large gaps between owned chunks,
    // and there are no consecutive owned chunks.
    // e.g. |O|O|O|O|X|O|
    DenseOrphans,
    VeryDenseOrphans,
    // This means a chunk map with ~NDV chunks, all orphans.
    // e.g. |O|O|O|O|O|O|
    AllOrphans,
    // This means a chunk map with a single trivial orphan entry [MinKey, MaxKey].
    // e.g. |O|
    AllOrphansLargeChunkMap
};

enum ShardFilteringStrategy { NoShardFilter, ShardFilter };

class ShardFilteringDistinctScanPerfTestFixture : public QueryShardServerTestFixture {
public:
    void _doTest() final { /* Unused. */ }

    struct DistinctScanParamsForTest {
        // Collection & sharding set-up.
        const KeyPattern& shardKey;
        const std::vector<BSONObj>& docsOnShard;
        const std::vector<ChunkDesc>& chunks;

        // Distinct scan params.
        BSONObj idxKey;
        int scanDirection;
        int fieldNo;
        IndexBounds bounds;
        bool shouldShardFilter;
        bool shouldFetch;
    };

    void prepareTest(DistinctScanParamsForTest&& testParams) {
        // Should only call this once.
        invariant(!_params);

        createIndex(testParams.idxKey, "some_index");
        insertDocs(testParams.docsOnShard);

        const auto metadata{prepareTestData(testParams.shardKey, testParams.chunks)};

        auto opCtx = operationContext();
        auto ns = nss();

        _shardRole = std::make_unique<ScopedSetShardRole>(
            opCtx,
            ns,
            ShardVersionFactory::make(metadata) /* shardVersion */,
            boost::none /* databaseVersion */);

        _coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, ns, AcquisitionPrerequisites::kRead),
            MODE_IS);
        const CollectionPtr& collPtr = _coll->getCollectionPtr();
        const auto& idxDesc = getIndexDescriptor(collPtr, "some_index");

        // Set-up DistinctParams for a full distinct scan on the first field in the index.
        _params = DistinctParams{opCtx, collPtr, &idxDesc};
        _params->scanDirection = testParams.scanDirection;
        _params->fieldNo = testParams.fieldNo;
        _params->bounds = std::move(testParams.bounds);
        _shouldFetch = testParams.shouldFetch;
        _shouldShardFilter = testParams.shouldShardFilter;

        // Create a shard filterer.
        _scopedCss = std::make_unique<CollectionShardingState::ScopedCollectionShardingState>(
            CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, ns));
    }

    std::unique_ptr<PlanStage> prepareDistinctScan() {
        invariant(_coll);
        invariant(_scopedCss);
        invariant(_shardRole);

        auto sfi = _shouldShardFilter
            ? std::make_unique<ShardFiltererImpl>(
                  (*_scopedCss)
                      ->getOwnershipFilter(
                          operationContext(),
                          CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup))
            : nullptr;

        // Construct distinct, and verify its expected execution pattern on the given data.
        auto root = std::make_unique<DistinctScan>(expressionContext(),
                                                   *_coll,
                                                   *_params,
                                                   &_ws,
                                                   std::move(sfi),
                                                   _shouldFetch && _shouldShardFilter);
        if (_shouldFetch && !_shouldShardFilter) {
            return std::make_unique<FetchStage>(
                expressionContext(), &_ws, std::move(root), nullptr /* no filter */, *_coll);
        }
        return root;
    }

    ~ShardFilteringDistinctScanPerfTestFixture() override {
        _scopedCss.reset();
        _shardRole.reset();
        _coll.reset();
        tearDown();
    }

private:
    WorkingSet _ws;
    boost::optional<CollectionAcquisition> _coll;
    std::unique_ptr<ScopedSetShardRole> _shardRole;
    std::unique_ptr<CollectionShardingState::ScopedCollectionShardingState> _scopedCss;
    boost::optional<DistinctParams> _params;
    bool _shouldFetch;
    bool _shouldShardFilter;
};

using ChunkDesc = QueryShardServerTestFixture::ChunkDesc;

class ShardFilteringDistinctScanBM : public unittest::BenchmarkWithProfiler {
public:
    void setUpSharedResources(benchmark::State& state) override {
        invariant(state.threads == 1, "must be single threaded");
        _fixture = std::make_unique<ShardFilteringDistinctScanPerfTestFixture>();
        _fixture->setUp();
    }

    void tearDownSharedResources(benchmark::State& state) override {
        _fixture.reset();
    }

    static void setUpOrphanDistribution(int n, bool sparse, int& step, int& chunkSize) {
        if (n < 100) {
            // For ndv <= 100, sparse == dense.
            step = 2;
            chunkSize = 1;
        } else if (sparse) {
            step = (98 * n - 9800) / 900 + 2;  // n = 100 => step = 2, n = 1000 => step = 100.
            chunkSize = 1;
        } else {
            // Dense case.
            step = (8 * n - 800) / 900 + 2;  // n = 100 => step = 2, n = 1000 => step = 10.
            chunkSize =
                (4 * n - 400) / 900 + 1;  // n = 100 => chunkSize = 1, n = chunkSize => step = 6.
        }
    }

    static auto generateUniformChunkMap(const std::vector<BSONObj>& dataSet,
                                        StringData fieldName,
                                        int chunkSize,
                                        bool onCurrentShard) {
        BSONObjSet sortedValues = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        sortedValues.insert(BSONObjBuilder().appendMinKey(fieldName).obj());
        for (auto&& obj : dataSet) {
            auto elem = obj.getField(fieldName);
            sortedValues.insert(BSON(elem));
        }

        std::vector<ChunkDesc> chunkMap;
        auto it = sortedValues.begin();
        while (it != sortedValues.end()) {
            auto& min = *it;
            // Make the chunk include 'chunkSize' ndvs.
            std::advance(it, chunkSize);
            auto& max =
                it == sortedValues.end() ? BSONObjBuilder().appendMaxKey(fieldName).obj() : *it;
            chunkMap.push_back({{min, max}, onCurrentShard});
        }
        return chunkMap;
    }

    /**
     * Returns the smallest possible chunk map, [MinKey, MaxKey], such that the ownership of the
     * current chunk is as specified by 'onCurrentShard'.
     */
    static auto generateMinimalChunkMap(StringData fieldName, bool onCurrentShard) {
        return generateUniformChunkMap(
            std::vector<BSONObj>(), fieldName, 1 /* doesn't matter */, onCurrentShard);
    }

    static void updateOwnershipOfChunks(std::vector<ChunkDesc>& chunkMap,
                                        int largeBlockSize,
                                        int smallBlockSize,
                                        bool isSmallBlockOwned) {
        auto chunkIt = chunkMap.begin();
        size_t i = 0;
        while (chunkIt != chunkMap.end()) {
            // Skip large block.
            if (i + largeBlockSize < chunkMap.size()) {
                i += largeBlockSize;
                std::advance(chunkIt, largeBlockSize);
            } else {
                break;
            }
            // Update ownership of small block.
            for (int chunksInBlockSoFar = 0; chunksInBlockSoFar < smallBlockSize;
                 chunksInBlockSoFar++) {
                if (chunkIt == chunkMap.end()) {
                    return;
                }
                chunkIt->isOnCurShard = isSmallBlockOwned;
                chunkIt = std::next(chunkIt);
                i++;
            }
        }
    }

    void runBenchmark(StringData fieldName,
                      const std::vector<BSONObj>& dataSet,
                      OrphanDistribution dist,
                      ShardFilteringStrategy strategy,
                      bool fetch,
                      benchmark::State& state) {
        const ShardKeyPattern shardKeyPattern(BSON(fieldName << 1));
        const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();

        bool useDataset;
        bool updateChunkOwnership;
        bool mostChunksOwned;
        int chunkSize = kMinChunkSize;
        int largeBlockSize;
        int smallBlockSize;
        switch (dist) {
            case NoOrphans:
            case AllOrphans:
            case NoOrphansLargeChunkMap:
            case AllOrphansLargeChunkMap: {
                // Chunk is always [MinKey, MaxKey] for the non-LargeChunkMap variants.
                useDataset = dist == NoOrphansLargeChunkMap || dist == AllOrphansLargeChunkMap;
                updateChunkOwnership = false;
                mostChunksOwned = dist == NoOrphans || dist == NoOrphansLargeChunkMap;
                // No-op.
                largeBlockSize = smallBlockSize = kMinBlockSize;
                break;
            }
            case VerySparseOrphans:
            case SparseOrphans:
            case DenseOrphans:
            case VeryDenseOrphans: {
                useDataset = true;
                updateChunkOwnership = true;
                mostChunksOwned = dist == SparseOrphans || dist == VerySparseOrphans;
                largeBlockSize = dist == VerySparseOrphans || dist == VeryDenseOrphans
                    ? kMaxBlockSize
                    : kMedBlockSize;
                smallBlockSize = kMinBlockSize;
                break;
            }
            case AlternatingChunks:
            case LargeAlternatingBlocks:
            case AlternatingBlocks: {
                useDataset = true;
                updateChunkOwnership = true;
                mostChunksOwned = true;
                largeBlockSize = smallBlockSize =
                    (dist == AlternatingChunks
                         ? kMinBlockSize
                         : (dist == LargeAlternatingBlocks ? kMaxBlockSize : kMedBlockSize));
                break;
            }
            default:
                MONGO_UNREACHABLE;
        };

        // Set up chunk map such that all chunks have the same ownership initially.
        auto chunkMap = useDataset
            ? generateUniformChunkMap(dataSet, fieldName, chunkSize, mostChunksOwned)
            : generateMinimalChunkMap(fieldName, mostChunksOwned);

        if (updateChunkOwnership) {
            // Flip the ownership for the specified minority of chunks.
            updateOwnershipOfChunks(chunkMap, largeBlockSize, smallBlockSize, !mostChunksOwned);
        }

        _fixture->prepareTest({.shardKey = shardKey,
                               .docsOnShard = dataSet,
                               .chunks = chunkMap,
                               .idxKey = shardKey.toBSON(),
                               .scanDirection = 1,
                               .fieldNo = 0,
                               .bounds = makeFullScanIndexBound(fieldName),
                               .shouldShardFilter = strategy == ShardFilter,
                               .shouldFetch = fetch});
        runBenchmarkWithProfiler(
            [&]() {
                auto root = _fixture->prepareDistinctScan();
                WorkingSetID wsid;
                PlanStage::StageState st;
                do {
                    st = root->work(&wsid);
                } while (st != PlanStage::IS_EOF);
            },
            state);
    }

private:
    std::unique_ptr<ShardFilteringDistinctScanPerfTestFixture> _fixture;
};

#define BENCHMARK_DISTINCT(fieldName, distribution, shouldShardFilter) \
    _BENCHMARK_DISTINCT(fieldName, distribution, shouldShardFilter);   \
    _BENCHMARK_DISTINCT_FETCH(fieldName, distribution, shouldShardFilter);

#define _BENCHMARK_NAME(fieldName, distribution, shouldShardFilter, shouldFetch) \
    fieldName##_##distribution##_##shouldShardFilter##_##shouldFetch

#define _BENCHMARK_DISTINCT(fieldName, distribution, shouldShardFilter)                        \
    __BENCHMARK_DISTINCT(_BENCHMARK_NAME(fieldName, distribution, shouldShardFilter, NoFetch), \
                         fieldName,                                                            \
                         distribution,                                                         \
                         shouldShardFilter,                                                    \
                         false /* shouldFetch */);
#define _BENCHMARK_DISTINCT_FETCH(fieldName, distribution, shouldShardFilter)                \
    __BENCHMARK_DISTINCT(_BENCHMARK_NAME(fieldName, distribution, shouldShardFilter, Fetch), \
                         fieldName,                                                          \
                         distribution,                                                       \
                         shouldShardFilter,                                                  \
                         true /* shouldFetch */);

#define __BENCHMARK_DISTINCT(NAME, fieldName, distribution, shouldShardFilter, shouldFetch)      \
    BENCHMARK_DEFINE_F(ShardFilteringDistinctScanBM, NAME)                                       \
    (benchmark::State & state) {                                                                 \
        runBenchmark(#fieldName, kDataSet, distribution, shouldShardFilter, shouldFetch, state); \
    }                                                                                            \
    BENCHMARK_REGISTER_F(ShardFilteringDistinctScanBM, NAME);

// Note: we only run one NoShardFilter case per field because orphan distribution makes no
// difference to execution time in the case where we are not shard filtering.

BENCHMARK_DISTINCT(lowNDVField, NoOrphans, NoShardFilter);
BENCHMARK_DISTINCT(lowNDVField, NoOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, NoOrphansLargeChunkMap, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, VerySparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, SparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, AlternatingChunks, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, AlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, LargeAlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, DenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, VeryDenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, AllOrphans, ShardFilter);
BENCHMARK_DISTINCT(lowNDVField, AllOrphansLargeChunkMap, ShardFilter);

BENCHMARK_DISTINCT(mediumNDVField, NoOrphans, NoShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, NoOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, NoOrphansLargeChunkMap, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, VerySparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, SparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, AlternatingChunks, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, AlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, LargeAlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, DenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, VeryDenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, AllOrphans, ShardFilter);
BENCHMARK_DISTINCT(mediumNDVField, AllOrphansLargeChunkMap, ShardFilter);

BENCHMARK_DISTINCT(highNDVField, NoOrphans, NoShardFilter);
BENCHMARK_DISTINCT(highNDVField, NoOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, NoOrphansLargeChunkMap, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, VerySparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, SparseOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, AlternatingChunks, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, AlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, LargeAlternatingBlocks, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, DenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, VeryDenseOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, AllOrphans, ShardFilter);
BENCHMARK_DISTINCT(highNDVField, AllOrphansLargeChunkMap, ShardFilter);

}  // namespace
}  // namespace mongo
