/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/hybrid_hash_join.h"

#include "mongo/db/sorter/sorter_file_name.h"
#include "mongo/db/sorter/sorter_template_defs.h"  // IWYU pragma: keep
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_options.h"

#include <bit>

namespace mongo {
namespace sbe {

// Number of partitions to use for partitioning.
// This should be a power of two.
constexpr size_t kNumPartitions = 256;
static_assert(std::has_single_bit(kNumPartitions), "kNumPartitions must be a power of two");

constexpr int kMaxRecursionDepth = 2;
constexpr uint32_t kPartitionMask = (kNumPartitions - 1);
constexpr int kNumBitsInPartitionMask = std::countr_one(kPartitionMask);

// ==================== JoinCursor ====================

class JoinCursor::Impl {
public:
    virtual ~Impl() = default;
    virtual boost::optional<MatchResult> next() = 0;
    virtual void saveState() {}
};

JoinCursor::~JoinCursor() = default;
JoinCursor::JoinCursor(JoinCursor&&) noexcept = default;
JoinCursor& JoinCursor::operator=(JoinCursor&&) noexcept = default;

JoinCursor::JoinCursor(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

boost::optional<MatchResult> JoinCursor::next() {
    return _impl ? _impl->next() : boost::none;
}

void JoinCursor::saveState() {
    if (_impl) {
        _impl->saveState();
    }
}

void JoinCursor::reset() {
    _impl = nullptr;
}

JoinCursor JoinCursor::empty() {
    return JoinCursor{nullptr};
}

// Iterates over matches from the in-memory hash table for a single probe key.
class InMemoryJoinCursor final : public JoinCursor::Impl {
public:
    InMemoryJoinCursor(HHJTableType& ht,
                       value::MaterializedRow probeKey,
                       value::MaterializedRow probeProject)
        : _probeKey(std::move(probeKey)), _probeProject(std::move(probeProject)) {
        // Probe the hash table for matches into the cursor to stream them.
        std::tie(_htIt, _htItEnd) = ht.equal_range(_probeKey);
    }

    boost::optional<MatchResult> next() override;

    void saveState() override {
        _probeKey.makeOwned();
        _probeProject.makeOwned();
    }

private:
    HHJTableType::iterator _htIt{};
    HHJTableType::iterator _htItEnd{};
    value::MaterializedRow _probeKey{};
    value::MaterializedRow _probeProject{};
};

boost::optional<MatchResult> InMemoryJoinCursor::next() {
    if (_htIt == _htItEnd) {
        return boost::none;
    }

    MatchResult result;
    result.buildKeyRow = &_htIt->first;
    result.buildProjectRow = &_htIt->second;
    result.probeKeyRow = &_probeKey;
    result.probeProjectRow = &_probeProject;

    _htIt++;

    return result;
}

// Iterates over matches for a spilled partition by scanning the probe spill file
// and looking up each probe key in the rebuilt hash table.
class SpilledPartitionJoinCursor final : public JoinCursor::Impl {
public:
    SpilledPartitionJoinCursor(HHJTableType& ht,
                               std::unique_ptr<SpillIterator> buildIterator,
                               std::unique_ptr<SpillIterator> probeIterator,
                               bool swapped)
        : _ht(ht), _probeIterator(std::move(probeIterator)), _swapped(swapped) {
        // Load the build side into the hash table.
        while (buildIterator->more()) {
            auto [keyRow, projectRow] = buildIterator->next();
            _ht.emplace(std::move(keyRow), std::move(projectRow));
        }
    }

    boost::optional<MatchResult> next() override;

private:
    HHJTableType& _ht;
    std::unique_ptr<SpillIterator> _probeIterator;
    bool _swapped;
    HHJTableType::iterator _htIt{};
    HHJTableType::iterator _htItEnd{};
    value::MaterializedRow _probeKey{};
    value::MaterializedRow _probeProject{};
};

boost::optional<MatchResult> SpilledPartitionJoinCursor::next() {
    while (true) {
        if (_htIt != _htItEnd) {
            // Continue yielding matches for the current probe row.
            MatchResult result;
            result.buildKeyRow = &_htIt->first;
            result.buildProjectRow = &_htIt->second;
            result.probeKeyRow = &_probeKey;
            result.probeProjectRow = &_probeProject;

            if (_swapped) {
                result.swapRecords();
            }

            ++_htIt;
            return result;
        }

        // Read the next probe row from the spill file and look up matches.
        if (!_probeIterator->more()) {
            return boost::none;
        }

        auto record = _probeIterator->next();
        _probeKey = std::move(record.first);
        _probeProject = std::move(record.second);

        std::tie(_htIt, _htItEnd) = _ht.equal_range(_probeKey);
    }
}

// Handles recursive partitioning when a spilled partition exceeds memory limit.
// Creates a nested HybridHashJoin to process the oversized partition.
class RecursiveJoinJoinCursor final : public JoinCursor::Impl {
public:
    RecursiveJoinJoinCursor(std::unique_ptr<HybridHashJoin> join,
                            std::unique_ptr<SpillIterator> buildIterator,
                            std::unique_ptr<SpillIterator> probeIterator,
                            bool swapped,
                            HashJoinStats& stats)
        : _join(std::move(join)),
          _probeIterator(std::move(probeIterator)),
          _swapped(swapped),
          _stats(stats) {
        while (buildIterator->more()) {
            auto [keyRow, projectRow] = buildIterator->next();
            _join->addBuild(std::move(keyRow), std::move(projectRow));
        }
        _join->finishBuild();
    }

    boost::optional<MatchResult> next() override;

private:
    enum class JoinPhase {
        kProbing,          // Processing probe side
        kSpillProcessing,  // Processing spilled partitions
        kComplete          // All done
    };
    std::unique_ptr<HybridHashJoin> _join;
    std::unique_ptr<SpillIterator> _probeIterator;
    bool _swapped;
    HashJoinStats& _stats;
    JoinCursor _cursor{nullptr};
    JoinPhase _joinPhase{JoinPhase::kProbing};
};

/**
 * Processes matches from a recursively-partitioned join using a state machine:
 * - kProbing: Reading probe rows from spill file and probing the nested join.
 * - kSpillProcessing: Processing any spilled partitions from the nested join.
 * - kComplete: All matches have been yielded.
 */
boost::optional<MatchResult> RecursiveJoinJoinCursor::next() {
    while (true) {
        if (auto matchResult = _cursor.next()) {
            if (_swapped) {
                matchResult->swapRecords();
            }
            return matchResult;
        }
        switch (_joinPhase) {
            case JoinPhase::kProbing:
                if (_probeIterator->more()) {
                    auto [keyRow, projectRow] = _probeIterator->next();
                    _cursor = _join->probe(std::move(keyRow), std::move(projectRow));
                    continue;
                }
                _join->finishProbe();
                _joinPhase = JoinPhase::kSpillProcessing;
                _cursor.reset();
                [[fallthrough]];
            case JoinPhase::kSpillProcessing:
                if (auto cursorOpt = _join->nextSpilledJoinCursor()) {
                    _cursor = std::move(*cursorOpt);
                    continue;
                }
                _cursor.reset();
                _joinPhase = JoinPhase::kComplete;
                return boost::none;
            case JoinPhase::kComplete:
                return boost::none;
        }
    }
}


// ==================== Build Phase ====================

void HybridHashJoin::addBuild(value::MaterializedRow key, value::MaterializedRow project) {
    tassert(11538800, "called addBuild() outside of kBuild phase", _phase == Phase::kBuild);

    auto memUsage = key.memUsageForSorter() + project.memUsageForSorter();

    if (!_isPartitioned) {
        // Pure in-memory mode: insert directly into hash table.
        // Switch to hybrid mode if memory limit exceeded.
        _memUsage += memUsage;
        _stats.peakTrackedMemBytes =
            std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));
        _ht->emplace(std::move(key), std::move(project));
        if (_memUsage > _memLimit) {
            enablePartitioning();
        }
        return;
    }

    int pIdx = getPartitionId(key);

    if (_isSpilled[pIdx]) {
        // This partition is spilled; write to file using SortedFileWriter.
        _partitionSpills[pIdx]->buildSpill.writer->addAlreadySorted(key, project);
        _partitionSpills[pIdx]->buildSpill.size += memUsage;
        _recordsAddedToWriter++;
        return;
    }

    _memUsage += memUsage;
    _partitionMemUsage[pIdx] += memUsage;
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));
    _partitionBuffers[pIdx].emplace_back(std::move(key), std::move(project));
    while (_memUsage >= _memLimit) {
        int victim = selectVictimPartition();
        if (victim == kNumPartitions) {
            // No more partitions to spill (shouldn't happen with reasonable threshold)
            break;
        }
        spillPartition(victim);
    }
}

/**
 * Transitions from pure in-memory hash join to hybrid mode with partitioning.
 * Called when memory usage first exceeds the limit during build phase.
 * Redistributes existing hash table entries into partition buffers and spills
 * the largest partitions until memory usage is under the limit.
 */
void HybridHashJoin::enablePartitioning() {
    // Initialize the partition buffers and metadata
    _partitionBuffers.resize(kNumPartitions);
    _partitionMemUsage.resize(kNumPartitions, 0);
    _isSpilled.resize(kNumPartitions, false);
    _partitionSpills.resize(kNumPartitions);

    // Partition all hash table records into partition buffers
    for (auto& [key, project] : *_ht) {
        int pIdx = getPartitionId(key);

        auto rowSize = key.memUsageForSorter() + project.memUsageForSorter();
        _partitionBuffers[pIdx].emplace_back(std::move(key), std::move(project));
        _partitionMemUsage[pIdx] += rowSize;
    }

    // Clear the hash table
    _htBucketCountBeforePartitioning = _ht->bucket_count();
    _ht->clear();
    _ht->rehash(0);

    // Spill victim partitions until under threshold
    while (_memUsage >= _memLimit) {
        int victim = selectVictimPartition();
        if (victim == kNumPartitions) {
            // No more partitions to spill (shouldn't happen with reasonable threshold)
            break;
        }
        spillPartition(victim);
    }

    _stats.usedDisk = true;
    _isPartitioned = true;
}

void HybridHashJoin::buildHashTableFromInMemPartitions() {
    tassert(11538801, "hash table is not empty", _ht->empty());

    _ht->rehash(_htBucketCountBeforePartitioning);

    // Create hash table for the build side.
    for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
        if (!_isSpilled[pIdx] && !_partitionBuffers[pIdx].empty()) {
            for (auto& [key, value] : _partitionBuffers[pIdx]) {
                _ht->emplace(std::move(key), std::move(value));
            }
            _partitionBuffers[pIdx].clear();
            _partitionBuffers[pIdx].shrink_to_fit();
        }
    }
}

/**
 * Selects the largest non-spilled partition for spilling.
 * Returns the partition index, or kNumPartitions if no partitions remain in memory.
 */
int HybridHashJoin::selectVictimPartition() const {
    int bestIdx = kNumPartitions;
    int32_t maxSize = 0;
    for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
        if (!_isSpilled[pIdx]) {
            auto partitionSize = _partitionMemUsage[pIdx];
            if (partitionSize > maxSize) {
                maxSize = partitionSize;
                bestIdx = pIdx;
            }
        }
    }
    return bestIdx;
}

void HybridHashJoin::spillPartition(int pIdx) {
    tassert(11538802, "Partition is already spilled", !_isSpilled[pIdx]);

    auto spilledPartition = createSpilledPartition();

    auto& partitionBuffer = _partitionBuffers[pIdx];
    for (auto& [key, project] : partitionBuffer) {
        spilledPartition->buildSpill.writer->addAlreadySorted(key, project);
    }
    auto prevMemUsage = _partitionMemUsage[pIdx];

    spilledPartition->buildSpill.size = prevMemUsage;
    _partitionSpills[pIdx] = std::move(spilledPartition);

    _stats.numPartitionsSpilled += 1;
    updateSpillingStats(partitionBuffer.size());

    _memUsage -= prevMemUsage;
    _memUsage += SpillHandle::kWriteBufferSize;
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));

    _isSpilled[pIdx] = true;

    // reset partition metadata
    _partitionMemUsage[pIdx] = 0;
    partitionBuffer.clear();
    partitionBuffer.shrink_to_fit();
}

std::unique_ptr<HybridHashJoin::SpilledPartition> HybridHashJoin::createSpilledPartition() {
    if (!_fileStats) {
        _fileStats = std::make_shared<SorterFileStats>(nullptr);
    }

    return std::make_unique<SpilledPartition>(getTempDir(), _fileStats.get());
}

void HybridHashJoin::finishBuild() {
    tassert(11538803, "called finishBuild() outside of kBuild() phase", _phase == Phase::kBuild);

    if (_isPartitioned) {
        // Flush any pending writes for spilled partitions
        for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
            if (_isSpilled[pIdx]) {
                _partitionSpills[pIdx]->buildSpill.finishWrite();
                // Build buffer flushed and released, but no need to update _memUsage here since the
                // probe phase will require the same amount of buffers, so we can update it in
                // finishProbe()
            }
        }
        if (_recordsAddedToWriter > 0) {
            updateSpillingStats(_recordsAddedToWriter);
            _recordsAddedToWriter = 0;
        }

        // Re-initialize hash table from memory-resident partitions
        buildHashTableFromInMemPartitions();
    }

    // Transition to probe phase
    _phase = Phase::kProbe;
}


// ==================== Probe Phase ====================

JoinCursor HybridHashJoin::probe(value::MaterializedRow key, value::MaterializedRow project) {
    tassert(11538804, "called probe() outside of kProbe phase", _phase == Phase::kProbe);

    if (_isPartitioned) {
        int pIdx = getPartitionId(key);

        if (_isSpilled[pIdx]) {
            // This partition is spilled; write to file using SortedFileWriter.

            auto memUsage = key.memUsageForSorter() + project.memUsageForSorter();

            _partitionSpills[pIdx]->probeSpill.writer->addAlreadySorted(key, project);
            _partitionSpills[pIdx]->probeSpill.size += memUsage;
            _recordsAddedToWriter++;
            return JoinCursor::empty();
        }
    }

    return JoinCursor(
        std::make_unique<InMemoryJoinCursor>(*_ht, std::move(key), std::move(project)));
}

void HybridHashJoin::finishProbe() {
    tassert(11538805, "called finishProbe() outside of kProbe phase", _phase == Phase::kProbe);

    // Flush any pending writes for spilled partitions
    if (_isPartitioned) {
        for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
            if (_isSpilled[pIdx]) {
                _partitionSpills[pIdx]->probeSpill.finishWrite();
                _memUsage -= SpillHandle::kWriteBufferSize;
            }
        }
        if (_recordsAddedToWriter > 0) {
            updateSpillingStats(_recordsAddedToWriter);
            _recordsAddedToWriter = 0;
        }
    }

    // Clear the in-memory hash table to free memory for spill processing
    _ht->clear();
    _memUsage = 0;

    // Reset iterator for spilled partition processing
    _currentSpilledPartitionIdx = 0;

    // Transition to spill processing phase
    _phase = Phase::kProcessSpilled;
}

// ==================== Spill Processing Phase ====================

boost::optional<JoinCursor> HybridHashJoin::nextSpilledJoinCursor() {
    tassert(11538806,
            "called nextSpilledJoinCursor() outside of kProcessSpilled phase",
            _phase == Phase::kProcessSpilled);
    if (!_isPartitioned) {
        return boost::none;
    }

    auto pIdx = findNextSpilledPartitionIdx();
    if (pIdx == kNumPartitions) {
        return boost::none;
    }

    // Clear previous partition's hash table
    _ht->clear();

    SpilledPartition& spill = *_partitionSpills[pIdx];

    // Optimization: if probe side is smaller than build side, swap them.
    // This reduces memory usage by loading the smaller side into the hash table.
    // The swapped flag is passed to the cursor to correctly attribute rows.
    bool swappedPartition = spill.swapIfProbeIsSmaller();
    _stats.numPartitionSwaps += swappedPartition;

    auto partitionSize = spill.buildSpill.size;

    if (partitionSize > _memLimit) {
        if (_recursionLevel == kMaxRecursionDepth) {
            // TODO SERVER-115389 Fall back to block nested loop join to guarantee completion.
            MONGO_UNIMPLEMENTED;
        }

        // Partition is still too large to fit in memory. Create a nested HybridHashJoin
        // to further subdivide the data. The higher level uses different hash bits to ensure
        // progress

        std::unique_ptr<HybridHashJoin> join =
            std::make_unique<HybridHashJoin>(_memLimit, _collator, _stats);
        join->_recursionLevel = _recursionLevel + 1;
        join->_fileStats = _fileStats;
        _stats.recursionDepthMax = std::max(_stats.recursionDepthMax, 1 + _recursionLevel);

        return JoinCursor(
            std::make_unique<RecursiveJoinJoinCursor>(std::move(join),
                                                      std::move(spill.buildSpill.iterator),
                                                      std::move(spill.probeSpill.iterator),
                                                      swappedPartition,
                                                      _stats));
    }

    // Update peakTrackedMemBytes to the partition size that will be loaded to memory.
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(partitionSize));

    return JoinCursor(
        std::make_unique<SpilledPartitionJoinCursor>(*_ht,
                                                     std::move(spill.buildSpill.iterator),
                                                     std::move(spill.probeSpill.iterator),
                                                     swappedPartition));
}

// ==================== Helpers ====================

int HybridHashJoin::getPartitionId(const value::MaterializedRow& key) const {
    size_t hash = value::MaterializedRowHasher{_collator}(key);

    static_assert((kMaxRecursionDepth + 1) * kNumBitsInPartitionMask <= sizeof(size_t) * 8);
    return (hash >> (_recursionLevel * kNumBitsInPartitionMask)) & kPartitionMask;
}

boost::filesystem::path HybridHashJoin::getTempDir() const {
    return boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp";
}

void HybridHashJoin::updateSpillingStats(uint64_t nRecords) {
    auto& spillingStats = _stats.spillingStats;
    auto spillToDiskBytes =
        _fileStats->bytesSpilledUncompressed() - spillingStats.getSpilledBytes();

    auto spilledDataStorageIncrease = spillingStats.updateSpillingStats(
        1,
        spillToDiskBytes,
        nRecords,
        _fileStats->bytesSpilled() - (int64_t)spillingStats.getSpilledDataStorageSize());

    hashJoinCounters.incrementPerSpilling(
        1, spillToDiskBytes, nRecords, spilledDataStorageIncrease);
}

// Return kNumPartitions if no more left
size_t HybridHashJoin::findNextSpilledPartitionIdx() {
    while (_currentSpilledPartitionIdx < kNumPartitions) {
        if (_isSpilled[_currentSpilledPartitionIdx]) {
            return _currentSpilledPartitionIdx++;
        }
        ++_currentSpilledPartitionIdx;
    }
    return _currentSpilledPartitionIdx;
}

/**
 * Resets the join to its initial state for reuse. Called between executions
 * when the stage is reopened.
 */
void HybridHashJoin::reset() {
    _ht->clear();

    _partitionBuffers = {};
    _partitionMemUsage = {};
    _isSpilled = {};
    _partitionSpills.clear();
    _partitionSpills.shrink_to_fit();

    _fileStats.reset();
    _memUsage = 0;
    _isPartitioned = false;

    _currentSpilledPartitionIdx = 0;
    _recordsAddedToWriter = 0;
    _htBucketCountBeforePartitioning = 0;
    _phase = Phase::kBuild;
}
}  // namespace sbe
}  // namespace mongo
