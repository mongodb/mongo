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

constexpr int kMaxRecursionDepth = 1;
constexpr uint32_t kPartitionMask = (kNumPartitions - 1);
constexpr int kNumBitsInPartitionMask = std::countr_one(kPartitionMask);
constexpr size_t kMaxBloomFilterSize = 16 * 1024 * 1024;  // 16 MB
constexpr size_t kMinBloomFilterSize = 32 * 1024;         // 32 KB

// ==================== JoinCursor ====================

class JoinCursor::Impl {
public:
    virtual ~Impl() = default;
    virtual boost::optional<MatchResult> next() = 0;
    virtual void saveState() {}
    virtual bool tryReprobe() {
        return false;
    }
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

bool JoinCursor::tryReprobe() {
    return _impl && _impl->tryReprobe();
}

JoinCursor JoinCursor::empty() {
    return JoinCursor{nullptr};
}

// Iterates over matches from the in-memory hash table for a single probe key.
class InMemoryJoinCursor final : public JoinCursor::Impl {
public:
    InMemoryJoinCursor(HHJTableType& ht,
                       value::MaterializedRow& probeKey,
                       value::MaterializedRow& probeProject)
        : _ht(ht), _probeKey(probeKey), _probeProject(probeProject) {
        // Probe the hash table for matches into the cursor to stream them.
        std::tie(_htIt, _htItEnd) = _ht.equal_range(_probeKey);
    }

    boost::optional<MatchResult> next() override;

    // Reprobe the hash table for matches into the cursor to stream them.
    bool tryReprobe() override {
        std::tie(_htIt, _htItEnd) = _ht.equal_range(_probeKey);
        return true;
    }

    void saveState() override {
        _probeKey.makeOwned();
        _probeProject.makeOwned();
    }

private:
    HHJTableType& _ht;
    value::MaterializedRow& _probeKey;
    value::MaterializedRow& _probeProject;
    HHJTableType::iterator _htIt{};
    HHJTableType::iterator _htItEnd{};
};

boost::optional<MatchResult> InMemoryJoinCursor::next() {
    if (_htIt == _htItEnd) {
        return boost::none;
    }

    MatchResult result;
    result.buildKeyRow = &_htIt->first.key;  // Access key from HashedKey
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
                               std::unique_ptr<SpilledPartition> spilledPartition,
                               bool swapped,
                               CollatorInterface* collator)
        : _ht(ht),
          _spilledPartition(std::move(spilledPartition)),
          _probeIterator(_spilledPartition->getProbeIterator()),
          _swapped(swapped),
          _collator(collator) {
        auto buildIterator = _spilledPartition->getBuildIterator();
        // Load the build side into the hash table.
        while (buildIterator->more()) {
            auto [keyRow, projectRow] = buildIterator->next();
            // Compute hash when loading from spill file and store with key.
            size_t hash = value::MaterializedRowHasher{_collator}(keyRow);
            _ht.emplace(HashedKey{hash, std::move(keyRow)}, std::move(projectRow));
        }
    }

    boost::optional<MatchResult> next() override;

private:
    HHJTableType& _ht;
    std::unique_ptr<SpilledPartition> _spilledPartition;
    std::shared_ptr<SpillIterator> _probeIterator;
    bool _swapped;
    CollatorInterface* _collator;
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
            result.buildKeyRow = &_htIt->first.key;  // Access key from HashedKey
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

        // Compute hash once and use for hash table lookup.
        size_t hash = value::MaterializedRowHasher{_collator}(record.first);
        HashedKey hashedKey{hash, std::move(record.first)};
        std::tie(_htIt, _htItEnd) = _ht.equal_range(hashedKey);

        // Move the key back since we need it for the MatchResult.
        _probeKey = std::move(hashedKey.key);
        _probeProject = std::move(record.second);
    }
}

// Handles recursive partitioning when a spilled partition exceeds memory limit.
// Creates a nested HybridHashJoin to process the oversized partition.
class RecursiveJoinJoinCursor final : public JoinCursor::Impl {
public:
    RecursiveJoinJoinCursor(std::unique_ptr<HybridHashJoin> join,
                            std::unique_ptr<SpilledPartition> spilledPartition,
                            bool swapped,
                            HashJoinStats& stats)
        : _join(std::move(join)),
          _spilledPartition(std::move(spilledPartition)),
          _probeIterator(_spilledPartition->getProbeIterator()),
          _swapped(swapped),
          _stats(stats) {
        auto buildIterator = _spilledPartition->getBuildIterator();
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
    std::unique_ptr<SpilledPartition> _spilledPartition;
    std::shared_ptr<SpillIterator> _probeIterator;
    value::MaterializedRow _probeKey;
    value::MaterializedRow _probeProject;
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
                    _probeKey = std::move(keyRow);
                    _probeProject = std::move(projectRow);
                    _join->probe(_probeKey, _probeProject, _cursor);
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

// Performs block nested loop join
class BlockNestedLoopJoinCursor final : public JoinCursor::Impl {
public:
    BlockNestedLoopJoinCursor(std::unique_ptr<SpilledPartition> spilledPartition,
                              uint64_t memLimit,
                              CollatorInterface* collator,
                              bool swapped,
                              HashJoinStats& stats)
        : _spilledPartition(std::move(spilledPartition)),
          _memLimit(memLimit),
          _collator(collator),
          _swapped(swapped),
          _stats(stats) {
        if (loadNextBuildChunk()) {
            _probeIterator = _spilledPartition->getProbeIterator();
            _hasProbeRow = advanceProbeIterator();
        }
    }

    boost::optional<MatchResult> next() override;

private:
    // Loads the next chunk of build data into _buildBuffer until memory limit is reached.
    // Returns true if any data was loaded, false if build side is exhausted.
    bool loadNextBuildChunk();

    // Advances the probe iterator to the next row and sets _probeKey/_probeProject to the new row.
    // Returns true if a new row was found, false if the probe side is exhausted.
    bool advanceProbeIterator();

    std::unique_ptr<SpilledPartition> _spilledPartition;
    uint64_t _memLimit;
    CollatorInterface* _collator;
    bool _swapped;
    HashJoinStats& _stats;

    std::shared_ptr<SpillIterator> _buildIterator;
    std::shared_ptr<SpillIterator> _probeIterator;
    std::vector<std::pair<value::MaterializedRow, value::MaterializedRow>> _buildBuffer;
    uint64_t _buildBufferMemUsage{0};

    size_t _buildBufferIdx{0};  // Current position in _buildBuffer
    value::MaterializedRow _probeKey;
    value::MaterializedRow _probeProject;
    bool _hasProbeRow{false};  // True if _probeKey/_probeProject are valid
};

/**
 * Loads the next chunk of build data into _buildBuffer until memory limit is reached.
 * Returns true if any data was loaded, false if build side is exhausted.
 */
bool BlockNestedLoopJoinCursor::loadNextBuildChunk() {
    _buildBuffer.clear();
    _buildBufferMemUsage = 0;
    _buildBufferIdx = 0;

    if (!_buildIterator) {
        _buildIterator = _spilledPartition->getBuildIterator();
    }

    while (_buildIterator->more()) {
        auto record = _buildIterator->next();

        auto keyRow = std::move(record.first);
        auto projectRow = std::move(record.second);

        auto memUsage = keyRow.memUsageForSorter() + projectRow.memUsageForSorter();
        _buildBuffer.emplace_back(std::move(keyRow), std::move(projectRow));
        _buildBufferMemUsage += memUsage;

        if (_buildBufferMemUsage >= _memLimit) {
            break;
        }
    }

    _stats.peakTrackedMemBytes = std::max(_stats.peakTrackedMemBytes, _buildBufferMemUsage);
    return !_buildBuffer.empty();
}

bool BlockNestedLoopJoinCursor::advanceProbeIterator() {
    if (_probeIterator && _probeIterator->more()) {
        auto record = _probeIterator->next();
        _probeKey = std::move(record.first);
        _probeProject = std::move(record.second);
        return true;
    }
    return false;
}

/**
 * Implements block nested loop join algorithm:
 * 1. Load a chunk of build-side records into memory (up to memory limit)
 * 2. For each probe record, compare against all build records in memory
 * 3. When all probe records are exhausted, load next build chunk and rewind probe
 * 4. Repeat until all build chunks are processed
 */
boost::optional<MatchResult> BlockNestedLoopJoinCursor::next() {
    const value::MaterializedRowEq keyEq(_collator);

    while (_hasProbeRow) {
        // Scan remaining build rows for the current probe row.
        while (_buildBufferIdx < _buildBuffer.size()) {
            auto& [buildKey, buildProject] = _buildBuffer[_buildBufferIdx++];

            // Check if keys match
            if (keyEq(buildKey, _probeKey)) {
                MatchResult result;
                result.buildKeyRow = &buildKey;
                result.buildProjectRow = &buildProject;
                result.probeKeyRow = &_probeKey;
                result.probeProjectRow = &_probeProject;

                if (_swapped) {
                    result.swapRecords();
                }
                return result;
            }
        }

        // Build buffer exhausted for this probe row. Try next probe row.
        _buildBufferIdx = 0;
        _hasProbeRow = advanceProbeIterator();
        if (_hasProbeRow) {
            continue;
        }

        // Probe exhausted for current build chunk. Load next chunk and rewind probe.
        if (!loadNextBuildChunk()) {
            break;
        }
        _probeIterator = _spilledPartition->getProbeIterator();
        _hasProbeRow = advanceProbeIterator();
    }

    return boost::none;
}

// ==================== SpilledPartition ====================

void SpilledPartition::writeBuildRecord(const FileKey& key,
                                        const FileValue& value,
                                        int64_t memSize) {
    tassert(11751700, "expected to be in kBuild phase", _joinPhase == HashJoinPhase::kBuild);
    _writer->addAlreadySorted(key, value);
    _buildMemSize += memSize;
}

void SpilledPartition::finishBuildWrite() {
    tassert(11751702, "expected to be in kBuild phase", _joinPhase == HashJoinPhase::kBuild);
    _buildRange = _writer->done()->getRange();

    // probe writer
    _writer = _storage.makeWriter(SortOptions{}, /*settings=*/{});
    _joinPhase = HashJoinPhase::kProbe;
}

void SpilledPartition::writeProbeRecord(const FileKey& key,
                                        const FileValue& value,
                                        int64_t memSize) {
    tassert(11751703, "expected to be in kProbe phase", _joinPhase == HashJoinPhase::kProbe);
    _writer->addAlreadySorted(key, value);
    _probeMemSize += memSize;
}

void SpilledPartition::finishProbeWrite() {
    tassert(11751704, "expected to be in kProbe phase", _joinPhase == HashJoinPhase::kProbe);
    _probeRange = _writer->done()->getRange();
    _writer.reset();
    _joinPhase = HashJoinPhase::kProcessSpilled;
}

std::shared_ptr<SpillIterator> SpilledPartition::getBuildIterator() {
    tassert(11751705,
            "expected to be in kProcessSpilled phase",
            _joinPhase == HashJoinPhase::kProcessSpilled);
    return _storage.getSortedIterator(_buildRange, {});
}

std::shared_ptr<SpillIterator> SpilledPartition::getProbeIterator() {
    tassert(11751706,
            "expected to be in kProcessSpilled phase",
            _joinPhase == HashJoinPhase::kProcessSpilled);
    return _storage.getSortedIterator(_probeRange, {});
}

bool SpilledPartition::swapIfProbeIsSmaller() {
    tassert(11751707,
            "expected to be in kProcessSpilled phase",
            _joinPhase == HashJoinPhase::kProcessSpilled);
    if (_buildMemSize > _probeMemSize) {
        std::swap(_buildRange, _probeRange);
        std::swap(_buildMemSize, _probeMemSize);
        return true;
    }
    return false;
}

// ==================== Build HashJoinPhase ====================

void HybridHashJoin::addBuild(value::MaterializedRow key, value::MaterializedRow project) {
    tassert(11538800, "called addBuild() outside of kBuild phase", _phase == HashJoinPhase::kBuild);

    int64_t memUsage = sizeof(size_t) + key.memUsageForSorter() + project.memUsageForSorter();

    // Compute hash once and reuse for both partitioning and hash table insertion.
    size_t hash = value::MaterializedRowHasher{_collator}(key);

    if (!_isPartitioned) {
        // Pure in-memory mode: insert directly into hash table.
        // Switch to hybrid mode if memory limit exceeded.
        _memUsage += memUsage;
        _stats.peakTrackedMemBytes =
            std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));
        _ht->emplace(HashedKey{hash, std::move(key)}, std::move(project));
        if (_memUsage > _memLimit) {
            enablePartitioning();
        }
        return;
    }

    int pIdx = getPartitionId(hash);

    if (_isSpilled[pIdx]) {
        // This partition is spilled; write to file using SortedFileWriter.

        // Add to bloom filter for this spilled partition
        _bloomFilter->insert(hash);

        auto& spilled = *_partitionSpills[pIdx];
        spilled.writeBuildRecord(key, project, memUsage - (int64_t)sizeof(size_t));
        _recordsAddedToWriter++;
        return;
    }

    _memUsage += memUsage;
    _partitionMemUsage[pIdx] += memUsage;
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));
    _partitionBuffers[pIdx].emplace_back(HashedKey{hash, std::move(key)}, std::move(project));
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
    // Initialize the partition buffers and metadata and bloom filter
    _partitionBuffers.resize(kNumPartitions);
    _partitionMemUsage.resize(kNumPartitions, 0);
    _isSpilled.resize(kNumPartitions, false);
    _partitionSpills.resize(kNumPartitions);

    size_t optimalBloomFilterSize =
        SplitBlockBloomFilter::optimalNumBytes(_estimatedBuildCardinality.value_or(0), 0.1);
    size_t bloomFilterSize =
        std::clamp(optimalBloomFilterSize, kMinBloomFilterSize, kMaxBloomFilterSize);
    _bloomFilter.emplace(bloomFilterSize);
    _memUsage += _bloomFilter->memoryUsage();

    _htBucketCountBeforePartitioning = _ht->bucket_count();

    // Partition all hash table records into partition buffers.
    // The hash is already stored in HashedKey, so we can reuse it for partitioning.
    while (!_ht->empty()) {
        auto node = _ht->extract(_ht->begin());
        int pIdx = getPartitionId(node.key().hash);

        auto rowSize =
            sizeof(size_t) + node.key().key.memUsageForSorter() + node.mapped().memUsageForSorter();
        _partitionBuffers[pIdx].emplace_back(std::move(node.key()), std::move(node.mapped()));
        _partitionMemUsage[pIdx] += rowSize;
    }

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

    // Write (key, project) to disk; hash is not stored and will be recomputed when reading.
    for (auto&& [hashedKey, project] : partitionBuffer) {
        // Add to bloom filter
        _bloomFilter->insert(hashedKey.hash);
        spilledPartition->writeBuildRecord(hashedKey.key, project, 0);
    }
    auto prevMemUsage = _partitionMemUsage[pIdx];
    spilledPartition->incrementBuildMemSize(prevMemUsage);
    _partitionSpills[pIdx] = std::move(spilledPartition);

    _stats.numPartitionsSpilled += 1;
    updateSpillingStats(partitionBuffer.size());

    _memUsage -= prevMemUsage;
    _memUsage += SpilledPartition::kWriteBufferSize;
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(_memUsage));

    _isSpilled[pIdx] = true;

    // reset partition metadata
    _partitionMemUsage[pIdx] = 0;
    partitionBuffer.clear();
    partitionBuffer.shrink_to_fit();
}

std::unique_ptr<SpilledPartition> HybridHashJoin::createSpilledPartition() {
    if (!_fileStats) {
        _fileStats = std::make_shared<SorterFileStats>(nullptr);
    }

    return std::make_unique<SpilledPartition>(getTempDir(), _fileStats.get());
}

void HybridHashJoin::finishBuild() {
    tassert(11538803,
            "called finishBuild() outside of kBuild() phase",
            _phase == HashJoinPhase::kBuild);

    if (_isPartitioned) {
        // Flush any pending writes for spilled partitions
        for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
            if (_isSpilled[pIdx]) {
                _partitionSpills[pIdx]->finishBuildWrite();
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
    _phase = HashJoinPhase::kProbe;
}


// ==================== Probe HashJoinPhase ====================

void HybridHashJoin::probe(value::MaterializedRow& key,
                           value::MaterializedRow& project,
                           JoinCursor& cursor) {
    tassert(11538804, "called probe() outside of kProbe phase", _phase == HashJoinPhase::kProbe);

    if (_isPartitioned) {
        size_t hash = value::MaterializedRowHasher{_collator}(key);
        int pIdx = getPartitionId(hash);

        if (_isSpilled[pIdx]) {
            // Check bloom filter first - if key is definitely not in build side, skip
            if (!_bloomFilter->maybeContains(hash)) {
                // Key definitely not in build side, no need to spill this probe row
                _stats.numProbeRecordsDiscarded += 1;
                return;
            }

            // This partition is spilled; write probe row to disk.
            auto memUsage = key.memUsageForSorter() + project.memUsageForSorter();

            auto& spilled = *_partitionSpills[pIdx];
            spilled.writeProbeRecord(key, project, memUsage);

            _recordsAddedToWriter++;
            return;
        }
    }

    // Creates the InMemoryJoinCursor only the first time. After that tryReprobe() will reuse
    // the already created cursor.
    if (!cursor.tryReprobe()) {
        cursor = JoinCursor(std::make_unique<InMemoryJoinCursor>(*_ht, key, project));
    }
}

void HybridHashJoin::finishProbe() {
    tassert(
        11538805, "called finishProbe() outside of kProbe phase", _phase == HashJoinPhase::kProbe);

    // Flush any pending writes for spilled partitions
    if (_isPartitioned) {
        for (size_t pIdx = 0; pIdx < kNumPartitions; ++pIdx) {
            if (_isSpilled[pIdx]) {
                _partitionSpills[pIdx]->finishProbeWrite();
                _memUsage -= SpilledPartition::kWriteBufferSize;
            }
        }
        if (_recordsAddedToWriter > 0) {
            updateSpillingStats(_recordsAddedToWriter);
            _recordsAddedToWriter = 0;
        }

        // clear bloom filter
        _bloomFilter.reset();
        // We are setting _memUsage to 0 in the next step, so no need to subtract the bloom filter
        // size here.
    }

    // Clear the in-memory hash table to free memory for spill processing
    _ht->clear();
    _ht->rehash(0);
    _memUsage = 0;

    // Reset iterator for spilled partition processing
    _currentSpilledPartitionIdx = 0;

    // Transition to spill processing phase
    _phase = HashJoinPhase::kProcessSpilled;
}

// ==================== Spill Processing HashJoinPhase ====================

boost::optional<JoinCursor> HybridHashJoin::nextSpilledJoinCursor() {
    tassert(11538806,
            "called nextSpilledJoinCursor() outside of kProcessSpilled phase",
            _phase == HashJoinPhase::kProcessSpilled);
    if (!_isPartitioned) {
        return boost::none;
    }

    auto pIdx = findNextSpilledPartitionIdx();
    if (pIdx == kNumPartitions) {
        return boost::none;
    }

    // Clear previous partition's hash table
    _ht->clear();

    // Move the ownership of spilledPartition to the JoinCursor.
    // _partitionSpills[pIdx] is not needed again after this.
    std::unique_ptr<SpilledPartition> spilledPartition = std::move(_partitionSpills[pIdx]);

    // Optimization: if probe side is smaller than build side, swap them.
    // This reduces memory usage by loading the smaller side into the hash table.
    // The swapped flag is passed to the cursor to correctly attribute rows.
    bool swappedPartition = spilledPartition->swapIfProbeIsSmaller();
    _stats.numPartitionSwaps += swappedPartition;

    auto partitionBuildSize = spilledPartition->buildMemSize();

    if (partitionBuildSize > _memLimit) {
        if (_recursionLevel == kMaxRecursionDepth) {
            // Fall back to block nested loop join to guarantee completion.
            _stats.numFallbacksToBlockNestedLoopJoin += 1;
            return JoinCursor(std::make_unique<BlockNestedLoopJoinCursor>(
                std::move(spilledPartition), _memLimit, _collator, swappedPartition, _stats));
        }

        // Partition is still too large to fit in memory. Create a nested HybridHashJoin
        // to further subdivide the data. The higher level uses different hash bits to ensure
        // progress

        std::unique_ptr<HybridHashJoin> join =
            std::make_unique<HybridHashJoin>(_memLimit, _collator, boost::none, _stats);
        join->_recursionLevel = _recursionLevel + 1;
        join->_fileStats = _fileStats;
        _stats.recursionDepthMax = std::max(_stats.recursionDepthMax, 1 + _recursionLevel);

        return JoinCursor(std::make_unique<RecursiveJoinJoinCursor>(
            std::move(join), std::move(spilledPartition), swappedPartition, _stats));
    }

    // Update peakTrackedMemBytes to the partition size that will be loaded to memory.
    _stats.peakTrackedMemBytes =
        std::max(_stats.peakTrackedMemBytes, static_cast<uint64_t>(partitionBuildSize));

    return JoinCursor(std::make_unique<SpilledPartitionJoinCursor>(
        *_ht, std::move(spilledPartition), swappedPartition, _collator));
}

// ==================== Helpers ====================

int HybridHashJoin::getPartitionId(size_t hash) const {
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
    _ht->rehash(0);

    _partitionBuffers.clear();
    _partitionBuffers.shrink_to_fit();
    _partitionMemUsage = {};
    _isSpilled = {};
    _partitionSpills.clear();
    _partitionSpills.shrink_to_fit();
    _bloomFilter.reset();

    _fileStats.reset();
    _memUsage = 0;
    _isPartitioned = false;

    _currentSpilledPartitionIdx = 0;
    _recordsAddedToWriter = 0;
    _htBucketCountBeforePartitioning = 0;
    _phase = HashJoinPhase::kBuild;
}
}  // namespace sbe
}  // namespace mongo
