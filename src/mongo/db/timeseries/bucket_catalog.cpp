/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/bucket_catalog.h"

#include <algorithm>
#include <boost/iterator/transform_iterator.hpp>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj);
void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj);

const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeWriteConflict);

uint8_t numDigits(uint32_t num) {
    uint8_t numDigits = 0;
    while (num) {
        num /= 10;
        ++numDigits;
    }
    return numDigits;
}

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj) {
    for (auto& arrayElem : obj) {
        if (arrayElem.type() == BSONType::Array) {
            BSONArrayBuilder subArray = builder->subarrayStart();
            normalizeArray(&subArray, arrayElem.Obj());
        } else if (arrayElem.type() == BSONType::Object) {
            BSONObjBuilder subObject = builder->subobjStart();
            normalizeObject(&subObject, arrayElem.Obj());
        } else {
            builder->append(arrayElem);
        }
    }
}

void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj) {
    // BSONObjIteratorSorted provides an abstraction similar to what this function does. However it
    // is using a lexical comparison that is slower than just doing a binary comparison of the field
    // names. That is all we need here as we are looking to create something that is binary
    // comparable no matter of field order provided by the user.

    // Helper that extracts the necessary data from a BSONElement that we can sort and re-construct
    // the same BSONElement from.
    struct Field {
        BSONElement element() const {
            return BSONElement(fieldName.rawData() - 1,  // Include type byte before field name
                               fieldName.size() + 1,     // Include null terminator after field name
                               totalSize,
                               BSONElement::CachedSizeTag{});
        }
        bool operator<(const Field& rhs) const {
            return fieldName < rhs.fieldName;
        }
        StringData fieldName;
        int totalSize;
    };

    // Put all elements in a buffer, sort it and then continue normalize in sorted order
    auto num = obj.nFields();
    static constexpr std::size_t kNumStaticFields = 16;
    boost::container::small_vector<Field, kNumStaticFields> fields;
    fields.resize(num);
    BSONObjIterator bsonIt(obj);
    int i = 0;
    while (bsonIt.more()) {
        auto elem = bsonIt.next();
        fields[i++] = {elem.fieldNameStringData(), elem.size()};
    }
    auto it = fields.begin();
    auto end = fields.end();
    std::sort(it, end);
    for (; it != end; ++it) {
        auto elem = it->element();
        if (elem.type() == BSONType::Array) {
            BSONArrayBuilder subArray(builder->subarrayStart(elem.fieldNameStringData()));
            normalizeArray(&subArray, elem.Obj());
        } else if (elem.type() == BSONType::Object) {
            BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
            normalizeObject(&subObject, elem.Obj());
        } else {
            builder->append(elem);
        }
    }
}

void normalizeTopLevel(BSONObjBuilder* builder, const BSONElement& elem) {
    if (elem.type() == BSONType::Array) {
        BSONArrayBuilder subArray(builder->subarrayStart(elem.fieldNameStringData()));
        normalizeArray(&subArray, elem.Obj());
    } else if (elem.type() == BSONType::Object) {
        BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
        normalizeObject(&subObject, elem.Obj());
    } else {
        builder->append(elem);
    }
}

OperationId getOpId(OperationContext* opCtx,
                    BucketCatalog::CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case BucketCatalog::CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
}

BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime) {
    BSONObjBuilder builder;
    builder.append(timeField, roundedTime);
    return builder.obj();
}
}  // namespace

const std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::kEmptyStats{
    std::make_shared<BucketCatalog::ExecutionStats>()};

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

BSONObj BucketCatalog::getMetadata(const OID& bucketId) const {
    BucketAccess bucket{const_cast<BucketCatalog*>(this), bucketId};
    if (!bucket) {
        return {};
    }

    return bucket->_metadata.toBSON();
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine) {

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = doc[*metaFieldName];
    }
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};

    auto stats = _getExecutionStats(ns);
    invariant(stats);

    auto timeElem = doc[options.getTimeField()];
    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << options.getTimeField() << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }

    auto time = timeElem.Date();

    ClosedBuckets closedBuckets;
    BucketAccess bucket{this, key, options, stats.get(), &closedBuckets, time};
    invariant(bucket);

    NewFieldNames newFieldNamesToBeInserted;
    uint32_t newFieldNamesSize = 0;
    uint32_t sizeToBeAdded = 0;
    bucket->_calculateBucketFieldsAndSizeChange(doc,
                                                options.getMetaField(),
                                                &newFieldNamesToBeInserted,
                                                &newFieldNamesSize,
                                                &sizeToBeAdded);

    auto isBucketFull = [&](BucketAccess* bucket) -> bool {
        if ((*bucket)->_numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
            stats->numBucketsClosedDueToCount.fetchAndAddRelaxed(1);
            return true;
        }
        if ((*bucket)->_size + sizeToBeAdded >
            static_cast<std::uint64_t>(gTimeseriesBucketMaxSize)) {
            stats->numBucketsClosedDueToSize.fetchAndAddRelaxed(1);
            return true;
        }
        auto bucketTime = (*bucket).getTime();
        if (time - bucketTime >= Seconds(*options.getBucketMaxSpanSeconds())) {
            stats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(1);
            return true;
        }
        if (time < bucketTime) {
            stats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(1);
            return true;
        }
        return false;
    };

    if (!bucket->_ns.isEmpty() && isBucketFull(&bucket)) {
        bucket.rollover(isBucketFull, &closedBuckets);

        bucket->_calculateBucketFieldsAndSizeChange(doc,
                                                    options.getMetaField(),
                                                    &newFieldNamesToBeInserted,
                                                    &newFieldNamesSize,
                                                    &sizeToBeAdded);
    }

    auto batch = bucket->_activeBatch(getOpId(opCtx, combine), stats);
    batch->_addMeasurement(doc);
    batch->_recordNewFields(std::move(newFieldNamesToBeInserted));

    bucket->_numMeasurements++;
    bucket->_size += sizeToBeAdded;
    if (time > bucket->_latestTime) {
        bucket->_latestTime = time;
    }
    if (bucket->_ns.isEmpty()) {
        // The namespace and metadata only need to be set if this bucket was newly created.
        bucket->_ns = ns;
        key.metadata.normalize();
        bucket->_metadata = key.metadata;

        // The namespace is stored two times: the bucket itself and _openBuckets.
        // The metadata is stored two times, normalized and un-normalized. A unique pointer to the
        // bucket is stored once: _allBuckets. A raw pointer to the bucket is stored at most twice:
        // _openBuckets, _idleBuckets.
        bucket->_memoryUsage += (ns.size() * 2) + (bucket->_metadata.toBSON().objsize() * 2) +
            sizeof(Bucket) + sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage);

    return InsertResult{batch, closedBuckets};
}

bool BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    if (batch->finished()) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return false;
    }

    _waitToCommitBatch(batch);

    BucketAccess bucket(this, batch->bucketId(), BucketState::kPrepared);
    if (batch->finished()) {
        // Someone may have aborted it while we were waiting.
        return false;
    } else if (!bucket) {
        abort(batch);
        return false;
    }

    auto prevMemoryUsage = bucket->_memoryUsage;
    batch->_prepareCommit(bucket);
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage - prevMemoryUsage);

    return true;
}

boost::optional<BucketCatalog::ClosedBucket> BucketCatalog::finish(
    std::shared_ptr<WriteBatch> batch, const CommitInfo& info) {
    invariant(!batch->finished());
    invariant(!batch->active());

    boost::optional<ClosedBucket> closedBucket;

    batch->_finish(info);

    BucketAccess bucket(this, batch->bucketId(), BucketState::kNormal);
    if (bucket) {
        bucket->_preparedBatch.reset();
    }

    auto& stats = batch->_stats;
    stats->numCommits.fetchAndAddRelaxed(1);
    if (batch->numPreviouslyCommittedMeasurements() == 0) {
        stats->numBucketInserts.fetchAndAddRelaxed(1);
    } else {
        stats->numBucketUpdates.fetchAndAddRelaxed(1);
    }

    stats->numMeasurementsCommitted.fetchAndAddRelaxed(batch->measurements().size());
    if (bucket) {
        bucket->_numCommittedMeasurements += batch->measurements().size();
    }

    if (!bucket) {
        // It's possible that we cleared the bucket in between preparing the commit and finishing
        // here. In this case, we should abort any other ongoing batches and clear the bucket from
        // the catalog so it's not hanging around idle.
        auto lk = _lockExclusive();
        auto it = _allBuckets.find(batch->bucketId());
        if (it != _allBuckets.end()) {
            auto bucket = it->second.get();
            stdx::unique_lock blk{bucket->_mutex};
            bucket->_preparedBatch.reset();
            _abort(blk, bucket, nullptr, boost::none);
        }
    } else if (bucket->allCommitted()) {
        if (bucket->_full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);

            bucket.release();
            auto lk = _lockExclusive();

            auto it = _allBuckets.find(batch->bucketId());
            if (it != _allBuckets.end()) {
                Bucket* ptr = it->second.get();
                _verifyBucketIsUnused(ptr);

                closedBucket = ClosedBucket{
                    batch->bucketId(), ptr->getTimeField().toString(), ptr->numMeasurements()};

                // Only remove from _allBuckets and _idleBuckets. If it was marked full, we know
                // that happened in BucketAccess::rollover, and that there is already a new open
                // bucket for this metadata.
                _markBucketNotIdle(ptr, false /* locked */);
                {
                    stdx::lock_guard statesLk{_statesMutex};
                    _bucketStates.erase(batch->bucketId());
                }

                _allBuckets.erase(batch->bucketId());
            }
        } else {
            _markBucketIdle(bucket);
        }
    }
    return closedBucket;
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch,
                          const boost::optional<Status>& status) {
    invariant(batch);
    invariant(batch->_commitRights.load());

    if (batch->finished()) {
        auto batchStatus = batch->getResult().getStatus();
        tassert(5916403,
                str::stream() << "Unexpected error when aborting time-series batch: "
                              << batchStatus,
                batchStatus == ErrorCodes::TimeseriesBucketCleared ||
                    batchStatus.isA<ErrorCategory::Interruption>() ||
                    batchStatus.isA<ErrorCategory::StaleShardVersionError>());
        return;
    }

    // Before we access the bucket, make sure it's still there.
    auto lk = _lockExclusive();
    auto it = _allBuckets.find(batch->bucketId());
    if (it == _allBuckets.end()) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        batch->_abort(status, nullptr);
        return;
    }

    Bucket* bucket = it->second.get();
    stdx::unique_lock blk{bucket->_mutex};
    _abort(blk, bucket, batch, status);
}

void BucketCatalog::clear(const OID& oid) {
    auto result = _setBucketState(oid, BucketState::kCleared);
    if (result && *result == BucketState::kPreparedAndCleared) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throw WriteConflictException();
    }
}

void BucketCatalog::clear(const std::function<bool(const NamespaceString&)>& shouldClear) {
    auto lk = _lockExclusive();
    auto statsLk = _statsMutex.lockExclusive();

    for (auto it = _allBuckets.begin(); it != _allBuckets.end();) {
        auto nextIt = std::next(it);

        const auto& bucket = it->second;
        stdx::unique_lock blk{bucket->_mutex};
        if (shouldClear(bucket->_ns)) {
            _executionStats.erase(bucket->_ns);
            _abort(blk, bucket.get(), nullptr, boost::none);
        }

        it = nextIt;
    }
}

void BucketCatalog::clear(const NamespaceString& ns) {
    clear([&ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
}

void BucketCatalog::clear(StringData dbName) {
    clear([&dbName](const NamespaceString& bucketNs) { return bucketNs.db() == dbName; });
}

void BucketCatalog::appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const {
    const auto stats = _getExecutionStats(ns);

    builder->appendNumber("numBucketInserts", stats->numBucketInserts.load());
    builder->appendNumber("numBucketUpdates", stats->numBucketUpdates.load());
    builder->appendNumber("numBucketsOpenedDueToMetadata",
                          stats->numBucketsOpenedDueToMetadata.load());
    builder->appendNumber("numBucketsClosedDueToCount", stats->numBucketsClosedDueToCount.load());
    builder->appendNumber("numBucketsClosedDueToSize", stats->numBucketsClosedDueToSize.load());
    builder->appendNumber("numBucketsClosedDueToTimeForward",
                          stats->numBucketsClosedDueToTimeForward.load());
    builder->appendNumber("numBucketsClosedDueToTimeBackward",
                          stats->numBucketsClosedDueToTimeBackward.load());
    builder->appendNumber("numBucketsClosedDueToMemoryThreshold",
                          stats->numBucketsClosedDueToMemoryThreshold.load());
    auto commits = stats->numCommits.load();
    builder->appendNumber("numCommits", commits);
    builder->appendNumber("numWaits", stats->numWaits.load());
    auto measurementsCommitted = stats->numMeasurementsCommitted.load();
    builder->appendNumber("numMeasurementsCommitted", measurementsCommitted);
    if (commits) {
        builder->appendNumber("avgNumMeasurementsPerCommit", measurementsCommitted / commits);
    }
}

BucketCatalog::StripedMutex::ExclusiveLock::ExclusiveLock(const StripedMutex& sm) {
    invariant(sm._mutexes.size() == _locks.size());
    for (std::size_t i = 0; i < sm._mutexes.size(); ++i) {
        _locks[i] = stdx::unique_lock<Mutex>(sm._mutexes[i]);
    }
}

BucketCatalog::StripedMutex::SharedLock BucketCatalog::StripedMutex::lockShared() const {
    static const std::hash<stdx::thread::id> hasher;
    return SharedLock{_mutexes[hasher(stdx::this_thread::get_id()) % kNumStripes]};
}

BucketCatalog::StripedMutex::ExclusiveLock BucketCatalog::StripedMutex::lockExclusive() const {
    return ExclusiveLock{*this};
}

BucketCatalog::StripedMutex::SharedLock BucketCatalog::_lockShared() const {
    return _bucketMutex.lockShared();
}

BucketCatalog::StripedMutex::ExclusiveLock BucketCatalog::_lockExclusive() const {
    return _bucketMutex.lockExclusive();
}

void BucketCatalog::_waitToCommitBatch(const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        BucketAccess bucket{this, batch->bucketId()};
        if (!bucket || batch->finished()) {
            return;
        }

        auto current = bucket->_preparedBatch;
        if (!current) {
            // No other batches for this bucket are currently committing, so we can proceed.
            bucket->_preparedBatch = batch;
            bucket->_batches.erase(batch->_opId);
            break;
        }

        // We have to wait for someone else to finish.
        bucket.release();
        current->getResult().getStatus().ignore();  // We don't care about the result.
    }
}

bool BucketCatalog::_removeBucket(Bucket* bucket, bool expiringBuckets) {
    auto it = _allBuckets.find(bucket->id());
    if (it == _allBuckets.end()) {
        return false;
    }

    invariant(bucket->_batches.empty());
    invariant(!bucket->_preparedBatch);

    _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    _markBucketNotIdle(bucket, expiringBuckets /* locked */);
    _removeNonNormalizedKeysForBucket(bucket);
    _openBuckets.erase({bucket->_ns, bucket->_metadata});
    {
        stdx::lock_guard statesLk{_statesMutex};
        _bucketStates.erase(bucket->id());
    }
    _allBuckets.erase(it);

    return true;
}

void BucketCatalog::_removeNonNormalizedKeysForBucket(Bucket* bucket) {
    auto comparator = bucket->_metadata.getComparator();
    for (auto&& metadata : bucket->_nonNormalizedKeyMetadatas) {
        _openBuckets.erase({bucket->_ns, {metadata.firstElement(), metadata, comparator}});
    }
}

void BucketCatalog::_abort(stdx::unique_lock<Mutex>& lk,
                           Bucket* bucket,
                           std::shared_ptr<WriteBatch> batch,
                           const boost::optional<Status>& status) {
    // For any uncommitted batches that we need to abort, see if we already have the rights,
    // otherwise try to claim the rights and abort it. If we don't get the rights, then wait
    // for the other writer to resolve the batch.
    for (const auto& [_, current] : bucket->_batches) {
        current->_abort(status, bucket);
    }
    bucket->_batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the on we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket->_preparedBatch) {
        if (prepared == batch) {
            prepared->_abort(status, bucket);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    lk.unlock();
    if (doRemove) {
        [[maybe_unused]] bool removed = _removeBucket(bucket, false /* expiringBuckets */);
    }
}

void BucketCatalog::_markBucketIdle(Bucket* bucket) {
    invariant(bucket);
    stdx::lock_guard lk{_idleMutex};
    _idleBuckets.push_front(bucket);
    bucket->_idleListEntry = _idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(Bucket* bucket, bool locked) {
    invariant(bucket);
    if (bucket->_idleListEntry) {
        stdx::unique_lock<Mutex> guard;
        if (!locked) {
            guard = stdx::unique_lock{_idleMutex};
        }
        _idleBuckets.erase(*bucket->_idleListEntry);
        bucket->_idleListEntry = boost::none;
    }
}

void BucketCatalog::_verifyBucketIsUnused(Bucket* bucket) const {
    // Take a lock on the bucket so we guarantee no one else is accessing it. We can release it
    // right away since no one else can take it again without taking the catalog lock, which we
    // also hold outside this method.
    stdx::lock_guard<Mutex> lk{bucket->_mutex};
}

void BucketCatalog::_expireIdleBuckets(ExecutionStats* stats,
                                       std::vector<BucketCatalog::ClosedBucket>* closedBuckets) {
    // Must hold an exclusive lock on _bucketMutex from outside.
    stdx::unique_lock lk{_idleMutex};

    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numClosed = 0;
    while (!_idleBuckets.empty() &&
           _memoryUsage.load() >
               static_cast<std::uint64_t>(gTimeseriesIdleBucketExpiryMemoryUsageThreshold.load()) &&
           numClosed <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = _idleBuckets.back();

        lk.unlock();
        _verifyBucketIsUnused(bucket);
        lk.lock();
        if (!bucket->_idleListEntry) {
            // The bucket may have become non-idle between when we unlocked _idleMutex and locked
            // the bucket's mutex.
            continue;
        }

        ClosedBucket closed{
            bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()};
        if (_removeBucket(bucket, true /* expiringBuckets */)) {
            stats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(1);
            closedBuckets->push_back(closed);
            ++numClosed;
        }
    }
}

std::size_t BucketCatalog::_numberOfIdleBuckets() const {
    stdx::lock_guard lk{_idleMutex};
    return _idleBuckets.size();
}

BucketCatalog::Bucket* BucketCatalog::_allocateBucket(const BucketKey& key,
                                                      const Date_t& time,
                                                      const TimeseriesOptions& options,
                                                      ExecutionStats* stats,
                                                      ClosedBuckets* closedBuckets,
                                                      bool openedDuetoMetadata) {
    _expireIdleBuckets(stats, closedBuckets);

    OID bucketId = OID::gen();

    auto roundedTime = timeseries::roundTimestampToGranularity(time, options.getGranularity());
    auto const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    bucketId.setTimestamp(roundedSeconds);

    auto [it, inserted] = _allBuckets.try_emplace(bucketId, std::make_unique<Bucket>(bucketId));
    tassert(6130900, "Expected bucket to be inserted", inserted);
    Bucket* bucket = it->second.get();
    _openBuckets[key] = bucket;
    {
        stdx::lock_guard statesLk{_statesMutex};
        _bucketStates.emplace(bucketId, BucketState::kNormal);
    }

    if (openedDuetoMetadata) {
        stats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(1);
    }

    bucket->_timeField = options.getTimeField().toString();

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(options.getTimeField(), roundedTime);
    bucket->_minmax.update(
        controlDoc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());

    return bucket;
}

std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) {
    {
        auto lock = _statsMutex.lockShared();
        auto it = _executionStats.find(ns);
        if (it != _executionStats.end()) {
            return it->second;
        }
    }

    auto lock = _statsMutex.lockExclusive();
    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return res.first->second;
}

const std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) const {
    auto lock = _statsMutex.lockShared();

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::_setBucketState(const OID& id,
                                                                           BucketState target) {
    stdx::lock_guard statesLk{_statesMutex};
    auto it = _bucketStates.find(id);
    if (it == _bucketStates.end()) {
        return boost::none;
    }

    auto& [_, state] = *it;
    switch (target) {
        case BucketState::kNormal: {
            if (state == BucketState::kPrepared) {
                state = BucketState::kNormal;
            } else if (state == BucketState::kPreparedAndCleared) {
                state = BucketState::kCleared;
            }
            break;
        }
        case BucketState::kPrepared: {
            if (state == BucketState::kNormal) {
                state = BucketState::kPrepared;
            }
            break;
        }
        case BucketState::kCleared: {
            if (state == BucketState::kNormal) {
                state = BucketState::kCleared;
            } else if (state == BucketState::kPrepared) {
                state = BucketState::kPreparedAndCleared;
            }
            break;
        }
        case BucketState::kPreparedAndCleared: {
            invariant(target != BucketState::kPreparedAndCleared);
        }
    }

    return state;
}

BucketCatalog::BucketMetadata::BucketMetadata(BSONElement elem,
                                              const StringData::ComparatorInterface* comparator)
    : _metadataElement(elem), _comparator(comparator) {}

BucketCatalog::BucketMetadata::BucketMetadata(BSONElement elem,
                                              BSONObj obj,
                                              const StringData::ComparatorInterface* comparator,
                                              bool normalized,
                                              bool copied)
    : _metadataElement(elem),
      _metadata(obj),
      _comparator(comparator),
      _normalized(normalized),
      _copied(copied) {}


void BucketCatalog::BucketMetadata::normalize() {
    if (!_normalized) {
        if (_metadataElement) {
            BSONObjBuilder objBuilder;
            // We will get an object of equal size, just with reordered fields.
            objBuilder.bb().reserveBytes(_metadataElement.size());
            normalizeTopLevel(&objBuilder, _metadataElement);
            _metadata = objBuilder.obj();
        }
        // Updates the BSONElement to refer to the copied BSONObj.
        _metadataElement = _metadata.firstElement();
        _normalized = true;
        _copied = true;
    }
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    // Should only be called after the metadata is owned.
    invariant(_copied);
    return _metadata;
}

const BSONElement BucketCatalog::BucketMetadata::getMetaElement() const {
    return _metadataElement;
}

StringData BucketCatalog::BucketMetadata::getMetaField() const {
    return StringData(_metadataElement.fieldName());
}

const StringData::ComparatorInterface* BucketCatalog::BucketMetadata::getComparator() const {
    return _comparator;
}

BucketCatalog::Bucket::Bucket(const OID& id) : _id(id) {}

const OID& BucketCatalog::Bucket::id() const {
    return _id;
}

StringData BucketCatalog::Bucket::getTimeField() {
    return _timeField;
}

void BucketCatalog::Bucket::_calculateBucketFieldsAndSizeChange(
    const BSONObj& doc,
    boost::optional<StringData> metaField,
    NewFieldNames* newFieldNamesToBeInserted,
    uint32_t* newFieldNamesSize,
    uint32_t* sizeToBeAdded) const {
    // BSON size for an object with an empty object field where field name is empty string.
    // We can use this as an offset to know the size when we have real field names.
    static constexpr int emptyObjSize = 12;
    // Validate in debug builds that this size is correct
    dassert(emptyObjSize == BSON("" << BSONObj()).objsize());

    newFieldNamesToBeInserted->clear();
    *newFieldNamesSize = 0;
    *sizeToBeAdded = 0;
    auto numMeasurementsFieldLength = numDigits(_numMeasurements);
    for (const auto& elem : doc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == metaField) {
            // Ignore the metadata field since it will not be inserted.
            continue;
        }

        // If the field name is new, add the size of an empty object with that field name.
        auto hashedKey = StringSet::hasher().hashed_key(fieldName);
        if (!_fieldNames.contains(hashedKey)) {
            newFieldNamesToBeInserted->push_back(hashedKey);
            *newFieldNamesSize += elem.fieldNameSize();
            *sizeToBeAdded += emptyObjSize + fieldName.size();
        }

        // Add the element size, taking into account that the name will be changed to its
        // positional number. Add 1 to the calculation since the element's field name size
        // accounts for a null terminator whereas the stringified position does not.
        *sizeToBeAdded += elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;
    }
}

bool BucketCatalog::Bucket::_hasBeenCommitted() const {
    return _numCommittedMeasurements != 0 || _preparedBatch;
}

bool BucketCatalog::Bucket::allCommitted() const {
    return _batches.empty() && !_preparedBatch;
}

uint32_t BucketCatalog::Bucket::numMeasurements() const {
    return _numMeasurements;
}

std::shared_ptr<BucketCatalog::WriteBatch> BucketCatalog::Bucket::_activeBatch(
    OperationId opId, const std::shared_ptr<ExecutionStats>& stats) {
    auto it = _batches.find(opId);
    if (it == _batches.end()) {
        it = _batches.try_emplace(opId, std::make_shared<WriteBatch>(_id, opId, stats)).first;
    }
    return it->second;
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog,
                                          BucketKey& key,
                                          const TimeseriesOptions& options,
                                          ExecutionStats* stats,
                                          ClosedBuckets* closedBuckets,
                                          const Date_t& time)
    : _catalog(catalog), _key(&key), _options(&options), _stats(stats), _time(&time) {

    auto bucketFound = [](BucketState bucketState) {
        return bucketState == BucketState::kNormal || bucketState == BucketState::kPrepared;
    };

    // First we try to find the bucket without normalizing the key as the normalization is an
    // expensive operation.
    auto hashedKey = BucketHasher{}.hashed_key(key);
    if (bucketFound(_findOpenBucketThenLock(hashedKey))) {
        return;
    }

    // If not found, we normalize the metadata object and try to find it again.
    // Save a copy of the non-normalized metadata before normalizing so we can add this key if the
    // bucket was found for the normalized metadata. The BSON element is still refering to that of
    // the document in current scope at this point. We will only make a copy of it when we decide to
    // store it.
    BSONElement nonNormalizedMetadata = key.metadata.getMetaElement();
    key.metadata.normalize();
    HashedBucketKey hashedNormalizedKey = BucketHasher{}.hashed_key(key);

    if (bucketFound(_findOpenBucketThenLock(hashedNormalizedKey))) {
        // Bucket found, check if we have available slots to store the non-normalized key
        if (_bucket->_nonNormalizedKeyMetadatas.size() ==
            _bucket->_nonNormalizedKeyMetadatas.capacity()) {
            return;
        }

        // Release the bucket as we need to acquire the exclusive lock for the catalog.
        release();

        // Re-construct the key as it were before normalization.
        auto originalBucketKey = nonNormalizedMetadata
            ? key.withCopiedMetadata(nonNormalizedMetadata.wrap())
            : key.withCopiedMetadata(BSONObj());
        hashedKey.key = &originalBucketKey;

        // Find the bucket under a catalog exclusive lock for the catalog. It may have been modified
        // since we released our locks. If found we store the key to avoid the need to normalize for
        // future lookups with this incoming field order.
        BSONObj nonNormalizedMetadataObj =
            nonNormalizedMetadata ? nonNormalizedMetadata.wrap() : BSONObj();
        if (bucketFound(_findOpenBucketThenLockAndStoreKey(
                hashedNormalizedKey, hashedKey, nonNormalizedMetadataObj))) {
            return;
        }
    }

    // Bucket not found, grab exclusive lock and create bucket with the key before normalization.
    auto originalBucketKey = nonNormalizedMetadata
        ? key.withCopiedMetadata(nonNormalizedMetadata.wrap())
        : key.withCopiedMetadata(BSONObj());
    hashedKey.key = &originalBucketKey;
    auto lk = _catalog->_lockExclusive();
    _findOrCreateOpenBucketThenLock(hashedNormalizedKey, hashedKey, closedBuckets);
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog,
                                          const OID& bucketId,
                                          boost::optional<BucketState> targetState)
    : _catalog(catalog) {
    invariant(!targetState || targetState == BucketState::kNormal ||
              targetState == BucketState::kPrepared);

    {
        auto lk = _catalog->_lockShared();
        auto bucketIt = _catalog->_allBuckets.find(bucketId);
        if (bucketIt == _catalog->_allBuckets.end()) {
            return;
        }

        _bucket = bucketIt->second.get();
        _acquire();
    }

    auto state =
        targetState ? _catalog->_setBucketState(_bucket->_id, *targetState) : _getBucketState();
    if (!state || state == BucketState::kCleared || state == BucketState::kPreparedAndCleared) {
        release();
    }
}

BucketCatalog::BucketAccess::~BucketAccess() {
    if (isLocked()) {
        release();
    }
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::BucketAccess::_getBucketState() const {
    stdx::lock_guard lk{_catalog->_statesMutex};
    auto it = _catalog->_bucketStates.find(_bucket->_id);
    return it != _catalog->_bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

BucketCatalog::BucketState BucketCatalog::BucketAccess::_findOpenBucketThenLock(
    const HashedBucketKey& key) {
    {
        auto lk = _catalog->_lockShared();
        auto it = _catalog->_openBuckets.find(key);
        if (it == _catalog->_openBuckets.end()) {
            // Bucket does not exist.
            return BucketState::kCleared;
        }

        _bucket = it->second;
        _acquire();
    }

    return _confirmStateForAcquiredBucket();
}

BucketCatalog::BucketState BucketCatalog::BucketAccess::_findOpenBucketThenLockAndStoreKey(
    const HashedBucketKey& normalizedKey,
    const HashedBucketKey& nonNormalizedKey,
    BSONObj nonNormalizedMetadata) {
    invariant(!isLocked());
    {
        auto lk = _catalog->_lockExclusive();
        auto it = _catalog->_openBuckets.find(normalizedKey);
        if (it == _catalog->_openBuckets.end()) {
            // Bucket does not exist.
            return BucketState::kCleared;
        }

        _bucket = it->second;
        _acquire();

        // Store the non-normalized key if we still have free slots
        if (_bucket->_nonNormalizedKeyMetadatas.size() <
            _bucket->_nonNormalizedKeyMetadatas.capacity()) {
            auto [_, inserted] =
                _catalog->_openBuckets.insert(std::make_pair(nonNormalizedKey, _bucket));
            if (inserted) {
                _bucket->_nonNormalizedKeyMetadatas.push_back(nonNormalizedMetadata);
                // Increment the memory usage to store this key and value in _openBuckets
                _bucket->_memoryUsage += nonNormalizedKey.key->ns.size() +
                    nonNormalizedMetadata.objsize() + sizeof(_bucket);
            }
        }
    }

    return _confirmStateForAcquiredBucket();
}

BucketCatalog::BucketState BucketCatalog::BucketAccess::_confirmStateForAcquiredBucket() {
    auto state = *_getBucketState();
    if (state == BucketState::kCleared || state == BucketState::kPreparedAndCleared) {
        release();
    } else {
        _catalog->_markBucketNotIdle(_bucket, false /* locked */);
    }

    return state;
}

void BucketCatalog::BucketAccess::_findOrCreateOpenBucketThenLock(
    const HashedBucketKey& normalizedKey,
    const HashedBucketKey& nonNormalizedKey,
    ClosedBuckets* closedBuckets) {
    auto it = _catalog->_openBuckets.find(normalizedKey);
    if (it == _catalog->_openBuckets.end()) {
        // No open bucket for this metadata.
        _create(normalizedKey, nonNormalizedKey, closedBuckets);
        return;
    }

    _bucket = it->second;
    _acquire();

    auto state = *_getBucketState();
    if (state == BucketState::kNormal || state == BucketState::kPrepared) {
        _catalog->_markBucketNotIdle(_bucket, false /* locked */);
        return;
    }

    _catalog->_abort(_guard, _bucket, nullptr, boost::none);
    _create(normalizedKey, nonNormalizedKey, closedBuckets);
}

void BucketCatalog::BucketAccess::_acquire() {
    invariant(_bucket);
    _guard = stdx::unique_lock<Mutex>(_bucket->_mutex);
}

void BucketCatalog::BucketAccess::_create(const HashedBucketKey& normalizedKey,
                                          const HashedBucketKey& nonNormalizedKey,
                                          ClosedBuckets* closedBuckets,
                                          bool openedDuetoMetadata) {
    invariant(_options);
    _bucket = _catalog->_allocateBucket(
        normalizedKey, *_time, *_options, _stats, closedBuckets, openedDuetoMetadata);
    _catalog->_openBuckets[nonNormalizedKey] = _bucket;
    _bucket->_nonNormalizedKeyMetadatas.push_back(nonNormalizedKey.key->metadata.toBSON());
    _acquire();
}

void BucketCatalog::BucketAccess::release() {
    invariant(_guard.owns_lock());
    _guard.unlock();
    _bucket = nullptr;
}

bool BucketCatalog::BucketAccess::isLocked() const {
    return _bucket && _guard.owns_lock();
}

BucketCatalog::Bucket* BucketCatalog::BucketAccess::operator->() {
    invariant(isLocked());
    return _bucket;
}

BucketCatalog::BucketAccess::operator bool() const {
    return isLocked();
}

BucketCatalog::BucketAccess::operator BucketCatalog::Bucket*() const {
    return _bucket;
}

void BucketCatalog::BucketAccess::rollover(const std::function<bool(BucketAccess*)>& isBucketFull,
                                           ClosedBuckets* closedBuckets) {
    invariant(isLocked());
    invariant(_key);
    invariant(_time);

    auto oldId = _bucket->id();
    release();

    // Precompute the hash outside the lock, since it's expensive.
    auto prevMetadata = _key->metadata.getMetaElement();
    _key->metadata.normalize();
    auto hashedNormalizedKey = BucketHasher{}.hashed_key(*_key);
    auto prevBucketKey = prevMetadata ? _key->withCopiedMetadata(prevMetadata.wrap())
                                      : _key->withCopiedMetadata(BSONObj());
    auto hashedKey = BucketHasher{}.hashed_key(prevBucketKey);

    auto lk = _catalog->_lockExclusive();
    _findOrCreateOpenBucketThenLock(hashedNormalizedKey, hashedKey, closedBuckets);

    // Recheck if still full now that we've reacquired the bucket.
    bool sameBucket =
        oldId == _bucket->id();  // Only record stats if bucket has changed, don't double-count.
    if (sameBucket || isBucketFull(this)) {
        // The bucket is indeed full, so create a new one.
        if (_bucket->allCommitted()) {
            // The bucket does not contain any measurements that are yet to be committed, so we can
            // remove it now. Otherwise, we must keep the bucket around until it is committed.
            closedBuckets->push_back(ClosedBucket{
                _bucket->id(), _bucket->getTimeField().toString(), _bucket->numMeasurements()});

            Bucket* oldBucket = _bucket;
            release();
            bool removed = _catalog->_removeBucket(oldBucket, false /* expiringBuckets */);
            invariant(removed);
        } else {
            _bucket->_full = true;

            // We will recreate a new bucket for the same key below. We also need to cleanup all
            // extra metadata keys added for the old bucket instance.
            _catalog->_removeNonNormalizedKeysForBucket(_bucket);
            release();
        }

        _create(hashedNormalizedKey, hashedKey, closedBuckets, false /* openedDueToMetadata */);
    }
}

Date_t BucketCatalog::BucketAccess::getTime() const {
    return _bucket->id().asDateT();
}

BucketCatalog::WriteBatch::WriteBatch(const OID& bucketId,
                                      OperationId opId,
                                      const std::shared_ptr<ExecutionStats>& stats)
    : _bucketId{bucketId}, _opId(opId), _stats{stats} {}

bool BucketCatalog::WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<BucketCatalog::CommitInfo> BucketCatalog::WriteBatch::getResult() const {
    if (!_promise.getFuture().isReady()) {
        _stats->numWaits.fetchAndAddRelaxed(1);
    }
    return _promise.getFuture().getNoThrow();
}

const OID& BucketCatalog::WriteBatch::bucketId() const {
    return _bucketId;
}

const std::vector<BSONObj>& BucketCatalog::WriteBatch::measurements() const {
    invariant(!_active);
    return _measurements;
}

const BSONObj& BucketCatalog::WriteBatch::min() const {
    invariant(!_active);
    return _min;
}

const BSONObj& BucketCatalog::WriteBatch::max() const {
    invariant(!_active);
    return _max;
}

const StringMap<std::size_t>& BucketCatalog::WriteBatch::newFieldNamesToBeInserted() const {
    invariant(!_active);
    return _newFieldNamesToBeInserted;
}

uint32_t BucketCatalog::WriteBatch::numPreviouslyCommittedMeasurements() const {
    invariant(!_active);
    return _numPreviouslyCommittedMeasurements;
}

bool BucketCatalog::WriteBatch::active() const {
    return _active;
}

bool BucketCatalog::WriteBatch::finished() const {
    return _promise.getFuture().isReady();
}

BSONObj BucketCatalog::WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) { return nameHashPair.first; };
    return BSON("docs" << _measurements << "bucketMin" << _min << "bucketMax" << _max
                       << "numCommittedMeasurements" << int(_numPreviouslyCommittedMeasurements)
                       << "newFieldNamesToBeInserted"
                       << std::set<std::string>(
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.begin(),
                                                             toFieldName),
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.end(),
                                                             toFieldName)));
}

void BucketCatalog::WriteBatch::_addMeasurement(const BSONObj& doc) {
    invariant(_active);
    _measurements.push_back(doc);
}

void BucketCatalog::WriteBatch::_recordNewFields(NewFieldNames&& fields) {
    invariant(_active);
    for (auto&& field : fields) {
        _newFieldNamesToBeInserted[field] = field.hash();
    }
}

void BucketCatalog::WriteBatch::_prepareCommit(Bucket* bucket) {
    invariant(_commitRights.load());
    invariant(_active);
    _active = false;
    _numPreviouslyCommittedMeasurements = bucket->_numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = _newFieldNamesToBeInserted.begin(); it != _newFieldNamesToBeInserted.end();) {
        StringMapHashedKey fieldName(it->first, it->second);
        if (bucket->_fieldNames.contains(fieldName)) {
            _newFieldNamesToBeInserted.erase(it++);
            continue;
        }

        bucket->_fieldNames.emplace(fieldName);
        ++it;
    }

    for (const auto& doc : _measurements) {
        bucket->_minmax.update(
            doc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());
    }

    const bool isUpdate = _numPreviouslyCommittedMeasurements > 0;
    if (isUpdate) {
        _min = bucket->_minmax.minUpdates();
        _max = bucket->_minmax.maxUpdates();
    } else {
        _min = bucket->_minmax.min();
        _max = bucket->_minmax.max();

        // Approximate minmax memory usage by taking sizes of initial commit. Subsequent updates may
        // add fields but are most likely just to update values.
        bucket->_memoryUsage += _min.objsize();
        bucket->_memoryUsage += _max.objsize();
    }
}

void BucketCatalog::WriteBatch::_finish(const CommitInfo& info) {
    invariant(_commitRights.load());
    invariant(!_active);
    _promise.emplaceValue(info);
}

void BucketCatalog::WriteBatch::_abort(const boost::optional<Status>& status,
                                       const Bucket* bucket) {
    if (finished()) {
        return;
    }

    _active = false;
    std::string nsIdentification;
    if (bucket) {
        nsIdentification.append(str::stream() << " for namespace " << bucket->_ns);
    }
    _promise.setError(status.value_or(Status{ErrorCodes::TimeseriesBucketCleared,
                                             str::stream() << "Time-series bucket " << _bucketId
                                                           << nsIdentification << " was cleared"}));
}

class BucketCatalog::ServerStatus : public ServerStatusSection {
public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            auto statsLk = bucketCatalog._statsMutex.lockShared();
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto lk = bucketCatalog._lockShared();
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets",
                             static_cast<long long>(bucketCatalog._allBuckets.size()));
        builder.appendNumber("numOpenBuckets",
                             static_cast<long long>(bucketCatalog._openBuckets.size()));
        builder.appendNumber("numIdleBuckets",
                             static_cast<long long>(bucketCatalog._numberOfIdleBuckets()));
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));
        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo
