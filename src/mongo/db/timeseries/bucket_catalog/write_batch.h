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

#pragma once

#include <boost/container/small_vector.hpp>

#include "mongo/bson/oid.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/future.h"

namespace mongo::timeseries::bucket_catalog {

class Bucket;

struct CommitInfo {
    boost::optional<repl::OpTime> opTime;
    boost::optional<OID> electionId;
};

// Number of new field names we can hold in NewFieldNames without needing to allocate memory.
static constexpr std::size_t kNumStaticNewFields = 10;
using NewFieldNames = boost::container::small_vector<StringMapHashedKey, kNumStaticNewFields>;

/**
 * The basic unit of work for a bucket. Each insert will return a shared_ptr to a WriteBatch.
 * When a writer is finished with all their insertions, they should then take steps to ensure
 * each batch they wrote into is committed. To ensure a batch is committed, a writer should
 * first attempt to claimCommitRights(). If successful, the writer can proceed to commit (or
 * abort) the batch via BucketCatalog::prepareCommit and BucketCatalog::finish. If unsuccessful,
 * it means another writer is in the process of committing. The writer can proceed to do other
 * work (like commit another batch), and when they have no other work to do, they can wait for
 * this batch to be committed by executing the blocking operation getResult().
 */
class WriteBatch {
    friend class BucketCatalog;

    // Number of measurements we can hold in a batch without needing to allocate memory.
    static constexpr std::size_t kNumStaticBatchMeasurements = 10;
    using BatchMeasurements = boost::container::small_vector<BSONObj, kNumStaticBatchMeasurements>;

public:
    WriteBatch() = delete;

    WriteBatch(const BucketHandle& bucketId, OperationId opId, ExecutionStatsController& stats);

    /**
     * Attempts to claim the right to commit a batch. If it returns true, rights are
     * granted. If it returns false, rights are revoked, and the caller should get the result
     * of the batch with getResult(). Non-blocking.
     */
    bool claimCommitRights();

    /**
     * Retrieves the result of the write batch commit. Should be called by any interested party
     * that does not have commit rights. Blocking.
     */
    StatusWith<CommitInfo> getResult();

    /**
     * Returns a handle which can be used by the BucketCatalog internally to locate its record
     * for this bucket.
     */
    const BucketHandle& bucket() const;

    const BatchMeasurements& measurements() const;
    const BSONObj& min() const;
    const BSONObj& max() const;
    const StringMap<std::size_t>& newFieldNamesToBeInserted() const;
    uint32_t numPreviouslyCommittedMeasurements() const;
    bool needToDecompressBucketBeforeInserting() const;
    const DecompressionResult& decompressed() const;

    /**
     * Returns whether the batch has already been committed or aborted.
     */
    bool finished() const;

    BSONObj toBSON() const;

private:
    /**
     * Adds a measurement. Active batches only.
     */
    void _addMeasurement(const BSONObj& doc);

    /**
     * Records a set of new-to-the-bucket fields. Active batches only.
     */
    void _recordNewFields(Bucket* bucket, NewFieldNames&& fields);

    /**
     * Prepares the batch for commit. Sets min/max appropriately, records the number of
     * documents that have previously been committed to the bucket, and renders the batch
     * inactive. Must have commit rights.
     */
    void _prepareCommit(Bucket* bucket);

    /**
     * Reports the result and status of a commit, and notifies anyone waiting on getResult().
     * Must have commit rights. Inactive batches only.
     */
    void _finish(const CommitInfo& info);

    /**
     * Abandons the write batch and notifies any waiters that the bucket has been cleared.
     */
    void _abort(const Status& status);

    const BucketHandle _bucket;
    OperationId _opId;
    ExecutionStatsController _stats;

    BatchMeasurements _measurements;
    BSONObj _min;  // Batch-local min; full if first batch, updates otherwise.
    BSONObj _max;  // Batch-local max; full if first batch, updates otherwise.
    uint32_t _numPreviouslyCommittedMeasurements = 0;
    StringMap<std::size_t> _newFieldNamesToBeInserted;   // Value is hash of string key
    boost::optional<DecompressionResult> _decompressed;  // If set, bucket is compressed on-disk.

    AtomicWord<bool> _commitRights{false};
    SharedPromise<CommitInfo> _promise;
};

}  // namespace mongo::timeseries::bucket_catalog
