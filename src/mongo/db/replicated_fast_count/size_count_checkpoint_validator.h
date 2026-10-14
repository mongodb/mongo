// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * Type alias for the map of collection UUID to optional metadata. `CollectionReplicatedMetadata` is
 * optional because when a checkpoint contains a collection delete entry, there is no replicated
 * metadata to check.
 */
using CheckpointContents = absl::flat_hash_map<UUID, boost::optional<CollectionReplicatedMetadata>>;

enum class ReportingStrategy { kFassert, kLogError };

/**
 * Validates the set equality of the `standby` and `primary` replicated metadata checkpoints.
 *
 * If `reportingStrategy` is `Fassert`, the function terminates the process immediately on the first
 * inequality detected. If `reportingStrategy` is `LogError`, the function logs an error for each
 * inequality and continues.
 */
void validateCheckpoint(const CheckpointContents& standby,
                        const CheckpointContents& primary,
                        ReportingStrategy reportingStrategy);

/**
 * Returns true if the provided oplog entry is a replicated metadata checkpoint, and false
 * otherwise.
 */
bool isCheckpointApplyOpsEntry(const repl::OplogEntry& entry);

/**
 * Parses a replicated metadata checkpoint entry and returns the per-collection replicated metadata
 * it contains.
 */
// TODO(SERVER-136501): Handle chained applyOps entries.
CheckpointContents parseCheckpoint(const repl::OplogEntry& entry);

/**
 * Record of replicated metadata derived from an oplog entry during oplog application. Stored by
 * thread-local buffers which are merged during validation.
 */
struct ValidatorDeltaRecord {
    Timestamp ts;
    UUID uuid;
    ReplicatedMetadataDelta delta;
};

/**
 * Per-worker buffers of `ValidatorDeltaRecord`s, one entry per worker thread.
 */
using ValidatorBuffers [[MONGO_MOD_PUBLIC]] = std::vector<std::vector<ValidatorDeltaRecord>>;

/**
 * The replicated metadata deltas between the previous `ValidatorInterval` and `cutoff` (exclusive).
 * `cutoff` is the timestamp of a watermark.
 *
 * `ValidatorInterval`s are 1-1 with watermarks. During preprocessing, we allocate one
 * `ValidatorInterval` per watermark, and during validation, we evict `ValidatorIntervals` we have
 * validated.
 */
struct ValidatorInterval {
    Timestamp cutoff;
    ReplicatedMetadataDeltas deltas;
};

/**
 * The `SizeCountCheckpointValidator` performs per-batch validation on replicated collection
 * metadata during oplog application.
 *
 * Terminology:
 * - A "watermark" is an oplog entry whose timestamp T is the valid-as-of timestamp for the next
 *   checkpoint. See `flusher::writeWatermark()` for more information.
 * - A "checkpoint" is an oplog entry indicating the replicated metadata (size, count, and hash) for
 *   zero or more collections at some valid-as-of timestamp.
 *
 * For each batch, the validation algorithm is as follows:
 * 1. Before oplog application, record each watermark and checkpoint in the batch using
 *    `preprocessOplogEntry()`. There can be zero or more watermarks and zero or more checkpoints.
 * 2. During oplog application, record deltas to replicated metadata in thread-local buffers
 *    using `recordDelta()`.
 * 3. After oplog application, merge the thread-local buffers and validate the checkpoints in
 *    the batch, if any, against the replicated metadata deltas recorded during step 2.
 *
 * If validation fails while the `enableContinuousInternodePerCollectionValidation` server
 * parameter is enabled, the node crashes with an fassert.
 */
class [[MONGO_MOD_PUBLIC]] SizeCountCheckpointValidator {
public:
    /**
     * Parses the provided oplog entry for information relevant to validation.
     *
     * If the oplog entry is a watermark, its timestamp is stored and defines a `ValidatorInterval`.
     * If the oplog entry is a checkpoint, it is stored for validation later.
     */
    void preprocessOplogEntry(const repl::OplogEntry& entry);

    /**
     * Records the provided `uuid`, `ts,` and `delta` in a thread-local buffer. Lock-free.
     */
    static void recordDelta(UUID uuid, Timestamp ts, ReplicatedMetadataDelta delta);

    /**
     * RAII data type which manages the lifetime of the thread-local buffer.
     */
    class ScopedThreadBuffer {
    public:
        explicit ScopedThreadBuffer(std::vector<ValidatorDeltaRecord>* buffer);
        ScopedThreadBuffer(const ScopedThreadBuffer&) = delete;
        ScopedThreadBuffer& operator=(const ScopedThreadBuffer&) = delete;
        ~ScopedThreadBuffer();
    };

    /**
     * Merges the provided `threadRecords`. For each recorded checkpoint, computes the expected
     * `CheckpointContents` and then validates it.
     *
     * This function does not validate the primary's checkpoint entry for the oplog's replicated
     * metadata. The `oplogUuid` must be the same on the primary as the standby.
     */
    void mergeAndValidate(const ValidatorBuffers& threadRecords, UUID oplogUuid);

private:
    /**
     * Combines the provided `threadRecords` into one `ValidatorInterval` per recorded watermark. If
     * the timestamp of the record is greater than any recorded watermark, it is added to the
     * `_ongoing` map of deltas instead.
     */
    void merge(const ValidatorBuffers& threadRecords);

    /**
     * Computes the expected `CheckpointContents` for use in validation.
     *
     * The returned `CheckpointContents` is the sum of deltas in [Timestamp(0,0), validAsOf]
     * because the checkpoint in the oplog contains absolute size, count, and hash.
     */
    CheckpointContents computeExpectedCheckpoint(Timestamp validAsOf);

    /**
     * Increments the baseline absolute replicated metadata values by the provided `deltas`.
     */
    void advanceBaseline(const ReplicatedMetadataDeltas& deltas);

    /**
     * Records the provided `watermarkTs` in `_intervals` with any `_ongoing` deltas and resets
     * `_ongoing`. This is called during preprocessing, so oplog entries are scanned in order, and
     * any `_ongoing` deltas must have been from a previous batch.
     */
    void recordWatermark(Timestamp watermarkTs);

    /**
     * Copies the provided `entry` into `_checkpoints` for validation later.
     */
    void recordCheckpoint(const repl::OplogEntry& entry);

    /**
     * Absolute replicated metadata values since the last `mergeAndValidate()`. Checkpoints in the
     * oplog contain the absolute size, count, and hash for a given validAsOf timestamp, so the
     * expected replicated metadata for a collection is defined as `_baseline` + deltas < validAsOf.
     */
    absl::flat_hash_map<UUID, CollectionReplicatedMetadata> _baseline;

    /**
     * Queue of `ValidatorIntervals` which contain the replicated metadata deltas between
     * `_baseline` and the `ValidatorInterval::cutoff`.
     *
     * Sorted in increasing timestamp order because `ValidatorIntervals` are pushed during
     * preprocessing and popped during validation.
     */
    std::deque<ValidatorInterval> _intervals;

    /**
     * The set of deltas with timestamp > the latest watermark entry. These deltas are moved to a
     * `ValidatorInterval` when the next watermark is seen.
     */
    ReplicatedMetadataDeltas _ongoing;

    /**
     * List of checkpoints to be validated.
     */
    std::vector<repl::OplogEntry> _checkpoints;
};
}  // namespace mongo::replicated_fast_count
