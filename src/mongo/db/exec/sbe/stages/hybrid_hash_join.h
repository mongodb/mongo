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

#pragma once

#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/bloom_filter.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * Stores a key row along with its precomputed hash value to avoid repeated hash computations.
 * Used as the key type in the hash table to enable efficient lookups without rehashing.
 */
struct HashedKey {
    size_t hash;
    value::MaterializedRow key;

    HashedKey(size_t h, value::MaterializedRow k) : hash(h), key(std::move(k)) {}
    HashedKey(HashedKey&&) noexcept = default;
    HashedKey& operator=(HashedKey&&) noexcept = default;
    HashedKey(const HashedKey&) = delete;
    HashedKey& operator=(const HashedKey&) = delete;
};

struct HashedKeyHasher {
    using is_transparent = void;

    CollatorInterface* collator = nullptr;

    size_t operator()(const HashedKey& hk) const {
        return hk.hash;
    }

    size_t operator()(const value::MaterializedRow& key) const {
        return value::MaterializedRowHasher{collator}(key);
    }
};

struct HashedKeyEq {
    using is_transparent = void;

    CollatorInterface* collator = nullptr;

    bool operator()(const HashedKey& a, const HashedKey& b) const {
        return value::MaterializedRowEq{collator}(a.key, b.key);
    }

    bool operator()(const HashedKey& a, const value::MaterializedRow& b) const {
        return value::MaterializedRowEq{collator}(a.key, b);
    }

    bool operator()(const value::MaterializedRow& a, const HashedKey& b) const {
        return value::MaterializedRowEq{collator}(a, b.key);
    }
};

using HHJTableType =
    std::unordered_multimap<HashedKey, value::MaterializedRow, HashedKeyHasher, HashedKeyEq>;

// Stores (key, project) pairs for in-memory partitions before hash table construction.
using BuildBuffer = std::vector<std::pair<HashedKey, value::MaterializedRow>>;

// Key type for file iterator (key row)
using FileKey = value::MaterializedRow;
// Value type for file iterator (project row)
using FileValue = value::MaterializedRow;
// Iterator type for reading from spill files
using SpillIterator = sorter::Iterator<FileKey, FileValue>;

enum class HashJoinPhase { kBuild, kProbe, kProcessSpilled };

// Represents a single match between a build row and probe row.
// Pointers are valid only until the next JoinCursor::next() call.
struct MatchResult {
    const value::MaterializedRow* buildKeyRow;
    const value::MaterializedRow* buildProjectRow;
    const value::MaterializedRow* probeKeyRow;
    const value::MaterializedRow* probeProjectRow;

    // swap build and probe side
    void swapRecords() {
        std::swap(buildKeyRow, probeKeyRow);
        std::swap(buildProjectRow, probeProjectRow);
    }
};

class JoinCursor {
public:
    class Impl;
    explicit JoinCursor(std::unique_ptr<Impl> impl);

    ~JoinCursor();

    JoinCursor(JoinCursor&&) noexcept;
    JoinCursor& operator=(JoinCursor&&) noexcept;

    static JoinCursor empty();

    [[nodiscard]] boost::optional<MatchResult> next();

    void saveState();

    bool tryReprobe();

    void reset();

private:
    std::unique_ptr<Impl> _impl;
};

// Encapsulates spill storage and writer for a single HHJ partition (build and probe).
class SpilledPartition {
public:
    static constexpr int64_t kWriteBufferSize = (int64_t)sorter::kSortedFileBufferSize;

    SpilledPartition(const boost::filesystem::path& tempDir, SorterFileStats* fileStats)
        : _storage(std::make_shared<SorterFile>(sorter::nextFileName(tempDir), fileStats),
                   /*dbName=*/boost::none,
                   sorter::kLatestChecksumVersion),
          _writer(_storage.makeWriter(SortOptions{}, /*settings=*/{})) {}

    void writeBuildRecord(const FileKey& key, const FileValue& value, int64_t memSize);
    void finishBuildWrite();

    void writeProbeRecord(const FileKey& key, const FileValue& value, int64_t memSize);
    void finishProbeWrite();

    bool swapIfProbeIsSmaller();
    std::shared_ptr<SpillIterator> getBuildIterator();
    std::shared_ptr<SpillIterator> getProbeIterator();

    int64_t buildMemSize() {
        return _buildMemSize;
    }

    void incrementBuildMemSize(int64_t memSize) {
        _buildMemSize += memSize;
    }

    int64_t probeMemSize() {
        return _probeMemSize;
    }

private:
    sorter::FileBasedSorterStorage<FileKey, FileValue> _storage;
    std::unique_ptr<SortedStorageWriter<FileKey, FileValue>> _writer;
    SorterRange _buildRange{};
    SorterRange _probeRange{};
    int64_t _buildMemSize{0};
    int64_t _probeMemSize{0};
    HashJoinPhase _joinPhase{HashJoinPhase::kBuild};
};

/**
 * Implements a hybrid hash join algorithm with spill-to-disk support for memory-bounded
 * execution. This class provides the core join logic used by HashJoinStage.
 *
 * The join operates in three phases:
 *
 * 1. BUILD PHASE: Rows from the build side are added via addBuild(). Initially, rows are
 *    stored directly in an in-memory hash table. If memory usage exceeds the limit, the
 *    algorithm switches to "hybrid" mode: rows are partitioned by hash value, and the
 *    largest partitions are spilled to disk to stay within the memory budget.
 *
 * 2. PROBE PHASE: After finishBuild(), probe rows are processed via probe(). For each
 *    probe row, if its partition is in memory, matches are returned immediately via a
 *    JoinCursor. If its partition was spilled, the probe row is written to a corresponding
 *    probe spill file for later processing.
 *
 * 3. SPILL PROCESSING PHASE: After finishProbe(), spilled partition pairs are processed
 *    one at a time via nextSpilledJoinCursor(). Each spilled build partition is loaded
 *    into memory, and matches are produced by scanning the corresponding probe spill file.
 *    If a spilled partition is still too large to fit in memory, recursive partitioning is
 *    applied (up to kMaxRecursionDepth levels).
 *
 * Usage:
 *   HybridHashJoin hhj(memLimit, collator);
 *   for (each build row)
 *       hhj.addBuild(key, project);
 *   hhj.finishBuild();
 *   for (each probe row)
 *       auto cursor = hhj.probe(key, project);
 *       // drain cursor
 *   hhj.finishProbe();
 *   while (auto cursor = hhj.nextSpilledJoinCursor()) {
 *       // drain cursor
 *   }
 */
class HybridHashJoin {
public:
    HybridHashJoin(int64_t memLimit,
                   CollatorInterface* collator,
                   boost::optional<size_t> estimatedBuildCardinality,
                   HashJoinStats& stats)
        : _memLimit(memLimit),
          _collator(collator),
          _estimatedBuildCardinality(estimatedBuildCardinality),
          _stats(stats) {
        const HashedKeyHasher hasher{_collator};
        const HashedKeyEq equator{_collator};
        _ht.emplace(0, hasher, equator);
    }

    HybridHashJoin(const HybridHashJoin&) = delete;
    HybridHashJoin& operator=(const HybridHashJoin&) = delete;

    // Build phase
    void addBuild(value::MaterializedRow key, value::MaterializedRow project);
    void finishBuild();

    // Probe phase
    // Updates the join cursor over matches for this probe row (may be empty; may spill the probe
    // row)
    void probe(value::MaterializedRow& key, value::MaterializedRow& project, JoinCursor& cursor);
    void finishProbe();

    // Spill processing phase
    // Processes spilled partition pairs one-by-one; caller pulls match streams
    [[nodiscard]] boost::optional<JoinCursor> nextSpilledJoinCursor();

    // Utilities
    int64_t getMemUsage() const {
        return _memUsage;
    };

    bool isPartitioned() const {
        return _isPartitioned;
    }

    void reset();

private:
    // Computes partition index using hash bits at the current recursion level.
    int getPartitionId(size_t hash) const;

    // Moves rows from in-memory partition buffers into the hash table after build phase.
    void buildHashTableFromInMemPartitions();

    // Returns the index of the largest non-spilled partition, or partition_size if none.
    int selectVictimPartition() const;

    // Spills the specified partition to disk and frees its in-memory buffer.
    void spillPartition(int pIdx);

    // Transitions from pure in-memory mode to hybrid mode with partitioning.
    void enablePartitioning();

    std::unique_ptr<SpilledPartition> createSpilledPartition();

    // Gets the temporary directory path for spilling
    boost::filesystem::path getTempDir() const;

    void updateSpillingStats(uint64_t nRecords);

    // Find the next spilled partition
    size_t findNextSpilledPartitionIdx();

    // File stats for tracking spill file operations
    std::shared_ptr<SorterFileStats> _fileStats;

    // Partition metadata. Uses Struct-of-Array pattern for better cache efficiency
    std::vector<BuildBuffer> _partitionBuffers;
    std::vector<int32_t> _partitionMemUsage;
    std::vector<bool> _isSpilled;
    std::vector<std::unique_ptr<SpilledPartition>> _partitionSpills;

    int64_t _memUsage{0};
    int64_t _memLimit;

    CollatorInterface* _collator;

    // In-memory hash table for the build side (used in both pure and hybrid modes).
    boost::optional<HHJTableType> _ht;

    // Estimated cardinality of the build side
    boost::optional<size_t> _estimatedBuildCardinality;

    // Bloom filter for filtering out keys that are not in the spilled partitions
    boost::optional<SplitBlockBloomFilter> _bloomFilter;

    // Current recursion depth for recursive partitioning. Higher levels use different
    // hash bits to avoid pathological re-partitioning of the same keys.
    int _recursionLevel = 0;

    // True once we've switched from pure in-memory to hybrid partitioned mode.
    bool _isPartitioned = false;

    HashJoinPhase _phase{HashJoinPhase::kBuild};
    size_t _currentSpilledPartitionIdx{0};

    uint64_t _recordsAddedToWriter = 0;  // track record for spillingStats

    size_t _htBucketCountBeforePartitioning{0};

    HashJoinStats& _stats;
};
}  // namespace mongo::sbe
