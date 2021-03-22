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

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/thread.h"

namespace mongo {
namespace {
const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();

uint8_t numDigits(uint32_t num) {
    uint8_t numDigits = 0;
    while (num) {
        num /= 10;
        ++numDigits;
    }
    return numDigits;
}

void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj) {
    BSONObjIteratorSorted iter(obj);
    while (iter.more()) {
        auto elem = iter.next();
        if (elem.type() != BSONType::Object) {
            builder->append(elem);
        } else {
            BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
            normalizeObject(&subObject, elem.Obj());
        }
    }
}

UUID getLsid(OperationContext* opCtx, BucketCatalog::CombineWithInsertsFromOtherClients combine) {
    static const UUID common{UUID::gen()};
    switch (combine) {
        case BucketCatalog::CombineWithInsertsFromOtherClients::kAllow:
            return common;
        case BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow:
            return opCtx->getLogicalSessionId()->getId();
    }
    MONGO_UNREACHABLE;
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

BSONObj BucketCatalog::getMetadata(const BucketId& bucketId) const {
    BucketAccess bucket{const_cast<BucketCatalog*>(this), bucketId};
    if (!bucket) {
        return {};
    }

    return bucket->metadata.toBSON();
}

StatusWith<std::shared_ptr<BucketCatalog::WriteBatch>> BucketCatalog::insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine) {
    auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, ns.db());
    invariant(viewCatalog);
    auto viewDef = viewCatalog->lookup(opCtx, ns.ns());
    invariant(viewDef);
    const auto& options = *viewDef->timeseries();

    BSONObjBuilder metadata;
    if (auto metaField = options.getMetaField()) {
        if (auto elem = doc[*metaField]) {
            metadata.appendAs(elem, *metaField);
        } else {
            metadata.appendNull(*metaField);
        }
    }
    auto key = std::make_tuple(ns, BucketMetadata{metadata.obj(), viewDef});

    auto stats = _getExecutionStats(ns);
    invariant(stats);

    auto timeElem = doc[options.getTimeField()];
    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << options.getTimeField() << "' must be present an contain a "
                              << "valid BSON UTC datetime value"};
    }

    auto time = timeElem.Date();

    BucketAccess bucket{this, key, stats.get(), time};

    StringSet newFieldNamesToBeInserted;
    uint32_t newFieldNamesSize = 0;
    uint32_t sizeToBeAdded = 0;
    bucket->calculateBucketFieldsAndSizeChange(doc,
                                               options.getMetaField(),
                                               &newFieldNamesToBeInserted,
                                               &newFieldNamesSize,
                                               &sizeToBeAdded);

    auto isBucketFull = [&](BucketAccess* bucket) -> bool {
        if ((*bucket)->numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
            stats->numBucketsClosedDueToCount.fetchAndAddRelaxed(1);
            return true;
        }
        if ((*bucket)->size + sizeToBeAdded >
            static_cast<std::uint64_t>(gTimeseriesBucketMaxSize)) {
            stats->numBucketsClosedDueToSize.fetchAndAddRelaxed(1);
            return true;
        }
        auto bucketTime = (*bucket).getTime();
        if (time - bucketTime >= Seconds(options.getBucketMaxSpanSeconds())) {
            stats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(1);
            return true;
        }
        if (time < bucketTime) {
            if (!(*bucket)->hasBeenCommitted() &&
                (*bucket)->latestTime - time < Seconds(options.getBucketMaxSpanSeconds())) {
                (*bucket).setTime();
            } else {
                stats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(1);
                return true;
            }
        }
        return false;
    };

    if (!bucket->ns.isEmpty() && isBucketFull(&bucket)) {
        bucket.rollover(isBucketFull);
        bucket->calculateBucketFieldsAndSizeChange(doc,
                                                   options.getMetaField(),
                                                   &newFieldNamesToBeInserted,
                                                   &newFieldNamesSize,
                                                   &sizeToBeAdded);
    }

    auto batch = bucket->activeBatch(getLsid(opCtx, combine), stats);
    batch->_addMeasurement(doc);
    batch->_recordNewFields(std::move(newFieldNamesToBeInserted));

    bucket->numMeasurements++;
    bucket->size += sizeToBeAdded;
    if (time > bucket->latestTime) {
        bucket->latestTime = time;
    }
    if (bucket->ns.isEmpty()) {
        // The namespace and metadata only need to be set if this bucket was newly created.
        bucket->ns = ns;
        bucket->metadata = std::get<BucketMetadata>(key);

        // The namespace is stored two times: the bucket itself and _openBuckets.
        // The metadata is stored two times: the bucket itself and _openBuckets.
        // The bucketId is stored one time: the bucket itself.
        // A shared pointer to the bucket is stored two times: _allBuckets and _openBuckets.
        // A raw pointer to the bucket is stored at most once: _idleBuckets.
        bucket->memoryUsage += (ns.size() * 2) + (bucket->metadata.toBSON().objsize() * 2) +
            ((sizeof(BucketId) + sizeof(OID))) + (sizeof(std::shared_ptr<Bucket>) * 2) +
            sizeof(Bucket*);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->memoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->memoryUsage);

    return batch;
}

void BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    invariant(!batch->finished());

    _waitToCommitBatch(batch);

    BucketAccess bucket(this, batch->bucket());

    auto prevMemoryUsage = bucket->memoryUsage;
    batch->_prepareCommit();
    _memoryUsage.fetchAndAdd(bucket->memoryUsage - prevMemoryUsage);

    bucket->batches.erase(batch->_lsid);
}

void BucketCatalog::finish(std::shared_ptr<WriteBatch> batch, const CommitInfo& info) {
    invariant(!batch->finished());
    invariant(!batch->active());

    auto stats = _getExecutionStats(batch->bucket()->ns);

    BucketAccess bucket(this, batch->bucket());

    batch->_finish(info);
    bucket->preparedBatch.reset();

    if (info.result.isOK()) {
        stats->numCommits.fetchAndAddRelaxed(1);
        if (bucket->numCommittedMeasurements == 0) {
            stats->numBucketInserts.fetchAndAddRelaxed(1);
        } else {
            stats->numBucketUpdates.fetchAndAddRelaxed(1);
        }

        stats->numMeasurementsCommitted.fetchAndAddRelaxed(batch->measurements().size());
        bucket->numCommittedMeasurements += batch->measurements().size();
    }

    if (bucket->allCommitted()) {
        if (bucket->full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _memoryUsage.fetchAndSubtract(bucket->memoryUsage);

            std::shared_ptr<Bucket> ptr(bucket);
            BucketId bucketId = bucket->id;
            bucket.release();
            auto lk = _lockExclusive();

            // Only remove from _allBuckets and _idleBuckets. If it was marked full, we know that
            // happened in BucketAccess::rollover, and that there is already a new open bucket for
            // this metadata.
            _markBucketNotIdle(ptr);
            _allBuckets.erase(bucketId);
        } else {
            _markBucketIdle(bucket);
        }
    }
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch) {
    invariant(batch);
    auto bucket = batch->_bucket;

    while (true) {
        std::vector<std::shared_ptr<WriteBatch>> batchesToWaitOn;

        {
            auto lk = _lockExclusive();
            stdx::unique_lock blk{bucket->mutex};
            if (bucket->batches.empty() && !bucket->preparedBatch) {
                // No uncommitted batches left to abort, go ahead and remove the bucket.
                blk.unlock();
                _removeBucket(bucket->id, true /* bucketIsUnused */);
                break;
            }

            // For any uncommitted batches that we need to abort, see if we already have the rights,
            // otherwise try to claim the rights and abort it. If we don't get the rights, then wait
            // for the other writer to resolve the batch.
            for (const auto& [_, active] : bucket->batches) {
                if (active == batch || active->claimCommitRights()) {
                    active->_abort();
                } else {
                    batchesToWaitOn.push_back(active);
                }
            }
            bucket->batches.clear();

            if (auto& committing = bucket->preparedBatch) {
                if (committing == batch) {
                    committing->_abort();
                } else {
                    batchesToWaitOn.push_back(committing);
                }
                committing.reset();
            }
        }

        for (const auto& batchToWaitOn : batchesToWaitOn) {
            batchToWaitOn->getResult().getStatus().ignore();
        }
    }
}

void BucketCatalog::clear(const NamespaceString& ns) {
    auto lk = _lockExclusive();
    auto statsLk = _statsMutex.lockExclusive();

    auto shouldClear = [&ns](const NamespaceString& bucketNs) {
        return ns.coll().empty() ? ns.db() == bucketNs.db() : ns == bucketNs;
    };

    for (auto it = _allBuckets.begin(); it != _allBuckets.end();) {
        auto nextIt = std::next(it);

        const auto [id, bucket] = *it;
        _verifyBucketIsUnused(bucket.get());
        if (shouldClear(bucket->ns)) {
            _executionStats.erase(bucket->ns);
            _removeBucket(id, true /* bucketIsUnused */);
        }

        it = nextIt;
    }
}

void BucketCatalog::clear(StringData dbName) {
    clear(NamespaceString(dbName, ""));
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
        BucketAccess bucket{this, batch->bucket()};
        auto current = bucket->preparedBatch;
        if (!current) {
            // No other batches for this bucket are currently committing, so we can proceed.
            bucket->preparedBatch = batch;
            break;
        }

        // We have to wait for someone else to finish.
        bucket.release();
        current->getResult().getStatus().ignore();  // We don't care about the result.
    }
}

bool BucketCatalog::_removeBucket(const BucketId& bucketId, bool bucketIsUnused) {
    auto it = _allBuckets.find(bucketId);
    if (it == _allBuckets.end()) {
        return false;
    }

    auto [_, bucket] = *it;
    if (!bucketIsUnused) {
        _verifyBucketIsUnused(bucket.get());
    }

    _memoryUsage.fetchAndSubtract(bucket->memoryUsage);
    if (bucket->idleListEntry) {
        _idleBuckets.erase(*bucket->idleListEntry);
        bucket->idleListEntry = boost::none;
    }
    _openBuckets.erase({std::move(bucket->ns), std::move(bucket->metadata)});
    _allBuckets.erase(it);

    return true;
}

void BucketCatalog::_markBucketIdle(const std::shared_ptr<Bucket>& bucket) {
    invariant(bucket);
    stdx::lock_guard lk{_idleMutex};
    _idleBuckets.emplace_front(bucket.get());
    bucket->idleListEntry = _idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(const std::shared_ptr<Bucket>& bucket) {
    invariant(bucket);
    if (bucket->idleListEntry) {
        stdx::lock_guard lk{_idleMutex};
        _idleBuckets.erase(*bucket->idleListEntry);
        bucket->idleListEntry = boost::none;
    }
}

void BucketCatalog::_verifyBucketIsUnused(const Bucket* bucket) const {
    // Take a lock on the bucket so we guarantee no one else is accessing it. We can release it
    // right away since no one else can take it again without taking the catalog lock, which we
    // also hold outside this method.
    stdx::lock_guard<Mutex> lk{bucket->mutex};
}

void BucketCatalog::_expireIdleBuckets(ExecutionStats* stats) {
    // Must hold an exclusive lock on _bucketMutex from outside.
    stdx::lock_guard lk{_idleMutex};

    // As long as we still need space and have entries, close idle buckets.
    while (!_idleBuckets.empty() &&
           _memoryUsage.load() >
               static_cast<std::uint64_t>(gTimeseriesIdleBucketExpiryMemoryUsageThreshold)) {
        Bucket* bucket = _idleBuckets.back();
        _verifyBucketIsUnused(bucket);
        if (_removeBucket(bucket->id, true /* bucketIsUnused */)) {
            stats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(1);
        }
    }
}

std::size_t BucketCatalog::_numberOfIdleBuckets() const {
    stdx::lock_guard lk{_idleMutex};
    return _idleBuckets.size();
}

std::shared_ptr<BucketCatalog::Bucket> BucketCatalog::_allocateBucket(
    const std::tuple<NamespaceString, BucketMetadata>& key,
    const Date_t& time,
    ExecutionStats* stats,
    bool openedDuetoMetadata) {
    _expireIdleBuckets(stats);

    auto id = BucketId{++_bucketNum};
    _setIdTimestamp(&id, time);

    auto bucket = std::make_shared<Bucket>(id);
    _allBuckets.insert(std::make_pair(id, bucket));
    _openBuckets[key] = bucket;

    if (openedDuetoMetadata) {
        stats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(1);
    }

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

void BucketCatalog::_setIdTimestamp(BucketId* id, const Date_t& time) {
    id->_id->setTimestamp(durationCount<Seconds>(time.toDurationSinceEpoch()));
}

BucketCatalog::BucketMetadata::BucketMetadata(BSONObj&& obj,
                                              std::shared_ptr<const ViewDefinition>& v)
    : _metadata(obj), _view(v) {
    BSONObjBuilder objBuilder;
    normalizeObject(&objBuilder, _metadata);
    _sorted = objBuilder.obj();
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _sorted.binaryEqual(other._sorted);
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    return _metadata;
}

StringData BucketCatalog::BucketMetadata::getMetaField() const {
    return _metadata.firstElementFieldNameStringData();
}

const CollatorInterface* BucketCatalog::BucketMetadata::getCollator() const {
    return _view->defaultCollator();
}

BucketCatalog::Bucket::Bucket(const BucketId& i) : id(i) {}

void BucketCatalog::Bucket::calculateBucketFieldsAndSizeChange(
    const BSONObj& doc,
    boost::optional<StringData> metaField,
    StringSet* newFieldNamesToBeInserted,
    uint32_t* newFieldNamesSize,
    uint32_t* sizeToBeAdded) const {
    newFieldNamesToBeInserted->clear();
    *newFieldNamesSize = 0;
    *sizeToBeAdded = 0;
    auto numMeasurementsFieldLength = numDigits(numMeasurements);
    for (const auto& elem : doc) {
        if (elem.fieldNameStringData() == metaField) {
            // Ignore the metadata field since it will not be inserted.
            continue;
        }

        // If the field name is new, add the size of an empty object with that field name.
        if (!fieldNames.contains(elem.fieldName())) {
            newFieldNamesToBeInserted->insert(elem.fieldName());
            *newFieldNamesSize += elem.fieldNameSize();
            *sizeToBeAdded += BSON(elem.fieldName() << BSONObj()).objsize();
        }

        // Add the element size, taking into account that the name will be changed to its
        // positional number. Add 1 to the calculation since the element's field name size
        // accounts for a null terminator whereas the stringified position does not.
        *sizeToBeAdded += elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;
    }
}

bool BucketCatalog::Bucket::hasBeenCommitted() const {
    return numCommittedMeasurements != 0 || preparedBatch;
}

bool BucketCatalog::Bucket::allCommitted() const {
    return batches.empty() && !preparedBatch;
}

std::shared_ptr<BucketCatalog::WriteBatch> BucketCatalog::Bucket::activeBatch(
    const UUID& lsid, const std::shared_ptr<ExecutionStats>& stats) {
    auto it = batches.find(lsid);
    if (it == batches.end()) {
        it =
            batches.try_emplace(lsid, std::make_shared<WriteBatch>(shared_from_this(), lsid, stats))
                .first;
    }
    return it->second;
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog,
                                          const std::tuple<NamespaceString, BucketMetadata>& key,
                                          ExecutionStats* stats,
                                          const Date_t& time)
    : _catalog(catalog), _key(&key), _stats(stats), _time(&time) {
    // precompute the hash outside the lock, since it's expensive
    const auto& hasher = _catalog->_openBuckets.hash_function();
    auto hash = hasher(*_key);

    {
        auto lk = _catalog->_lockShared();
        bool bucketExisted = _findOpenBucketAndLock(hash);
        if (bucketExisted) {
            return;
        }
    }

    auto lk = _catalog->_lockExclusive();
    _findOrCreateOpenBucketAndLock(hash);
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog, const BucketId& bucketId)
    : _catalog(catalog) {
    auto lk = _catalog->_lockShared();

    auto it = _catalog->_allBuckets.find(bucketId);
    if (it != _catalog->_allBuckets.end()) {
        _bucket = it->second;
        _acquire();
    }
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog,
                                          const std::shared_ptr<Bucket>& bucket)
    : _catalog(catalog) {
    auto lk = _catalog->_lockShared();
    _bucket = bucket;
    _acquire();
}

BucketCatalog::BucketAccess::~BucketAccess() {
    if (isLocked()) {
        release();
    }
}

bool BucketCatalog::BucketAccess::_findOpenBucketAndLock(std::size_t hash) {
    auto it = _catalog->_openBuckets.find(*_key, hash);
    if (it == _catalog->_openBuckets.end()) {
        // Bucket does not exist.
        return false;
    }

    _bucket = it->second;
    _acquire();
    _id = _bucket->id;
    _catalog->_markBucketNotIdle(_bucket);

    return true;
}

void BucketCatalog::BucketAccess::_findOrCreateOpenBucketAndLock(std::size_t hash) {
    auto it = _catalog->_openBuckets.find(*_key, hash);
    if (it == _catalog->_openBuckets.end()) {
        // No open bucket for this metadata.
        _create();
        return;
    }

    _bucket = it->second;
    _acquire();
    _id = _bucket->id;
    _catalog->_markBucketNotIdle(_bucket);
}

void BucketCatalog::BucketAccess::_acquire() {
    invariant(_bucket);
    _guard = stdx::unique_lock<Mutex>(_bucket->mutex);
}

void BucketCatalog::BucketAccess::_create(bool openedDuetoMetadata) {
    _bucket = _catalog->_allocateBucket(*_key, *_time, _stats, openedDuetoMetadata);
    _acquire();
    _id = _bucket->id;
}

void BucketCatalog::BucketAccess::release() {
    invariant(_guard.owns_lock());
    _guard.unlock();
    _bucket.reset();
}

bool BucketCatalog::BucketAccess::isLocked() const {
    return _bucket && _guard.owns_lock();
}

BucketCatalog::Bucket* BucketCatalog::BucketAccess::operator->() {
    invariant(isLocked());
    return _bucket.get();
}

BucketCatalog::BucketAccess::operator bool() const {
    return isLocked();
}

BucketCatalog::BucketAccess::operator std::shared_ptr<BucketCatalog::Bucket>() const {
    return _bucket;
}

void BucketCatalog::BucketAccess::rollover(const std::function<bool(BucketAccess*)>& isBucketFull) {
    invariant(isLocked());
    invariant(_key);
    invariant(_time);

    auto oldId = _id;
    release();

    // Precompute the hash outside the lock, since it's expensive.
    const auto& hasher = _catalog->_openBuckets.hash_function();
    auto hash = hasher(*_key);

    auto lk = _catalog->_lockExclusive();
    _findOrCreateOpenBucketAndLock(hash);

    // Recheck if still full now that we've reacquired the bucket.
    bool newBucket = oldId != _id;  // Only record stats if bucket has changed, don't double-count.
    if (!newBucket || isBucketFull(this)) {
        // The bucket is indeed full, so create a new one.
        if (_bucket->allCommitted()) {
            // The bucket does not contain any measurements that are yet to be committed, so we can
            // remove it now. Otherwise, we must keep the bucket around until it is committed.
            release();
            _catalog->_removeBucket(_id, true /* bucketIsUnused */);
        } else {
            _bucket->full = true;
            release();
        }

        _create(false /* openedDueToMetadata */);
    }
}

void BucketCatalog::BucketAccess::setTime() {
    invariant(isLocked());
    invariant(_key);
    invariant(_stats);
    invariant(_time);

    _catalog->_setIdTimestamp(&_id, *_time);
}

Date_t BucketCatalog::BucketAccess::getTime() const {
    return _id->asDateT();
}

void BucketCatalog::MinMax::update(const BSONObj& doc,
                                   boost::optional<StringData> metaField,
                                   const StringData::ComparatorInterface* stringComparator,
                                   const std::function<bool(int, int)>& comp) {
    invariant(_type == Type::kObject || _type == Type::kUnset);

    _type = Type::kObject;
    for (auto&& elem : doc) {
        if (metaField && elem.fieldNameStringData() == metaField) {
            continue;
        }
        _updateWithMemoryUsage(&_object[elem.fieldName()], elem, stringComparator, comp);
    }
}

void BucketCatalog::MinMax::_update(BSONElement elem,
                                    const StringData::ComparatorInterface* stringComparator,
                                    const std::function<bool(int, int)>& comp) {
    auto typeComp = [&](BSONType type) {
        return comp(elem.canonicalType() - canonicalizeBSONType(type), 0);
    };

    if (elem.type() == Object) {
        if (_type == Type::kObject || _type == Type::kUnset ||
            (_type == Type::kArray && typeComp(Array)) ||
            (_type == Type::kValue && typeComp(_value.firstElement().type()))) {
            // Compare objects element-wise.
            if (std::exchange(_type, Type::kObject) != Type::kObject) {
                _updated = true;
                _memoryUsage = 0;
            }
            for (auto&& subElem : elem.Obj()) {
                _updateWithMemoryUsage(
                    &_object[subElem.fieldName()], subElem, stringComparator, comp);
            }
        }
        return;
    }

    if (elem.type() == Array) {
        if (_type == Type::kArray || _type == Type::kUnset ||
            (_type == Type::kObject && typeComp(Object)) ||
            (_type == Type::kValue && typeComp(_value.firstElement().type()))) {
            // Compare arrays element-wise.
            if (std::exchange(_type, Type::kArray) != Type::kArray) {
                _updated = true;
                _memoryUsage = 0;
            }
            auto elemArray = elem.Array();
            if (_array.size() < elemArray.size()) {
                _array.resize(elemArray.size());
            }
            for (size_t i = 0; i < elemArray.size(); i++) {
                _updateWithMemoryUsage(&_array[i], elemArray[i], stringComparator, comp);
            }
        }
        return;
    }

    if (_type == Type::kUnset || (_type == Type::kObject && typeComp(Object)) ||
        (_type == Type::kArray && typeComp(Array)) ||
        (_type == Type::kValue &&
         comp(elem.woCompare(_value.firstElement(), false, stringComparator), 0))) {
        _type = Type::kValue;
        _value = elem.wrap();
        _updated = true;
        _memoryUsage = _value.objsize();
    }
}

void BucketCatalog::MinMax::_updateWithMemoryUsage(
    MinMax* minMax,
    BSONElement elem,
    const StringData::ComparatorInterface* stringComparator,
    const std::function<bool(int, int)>& comp) {
    _memoryUsage -= minMax->getMemoryUsage();
    minMax->_update(elem, stringComparator, comp);
    _memoryUsage += minMax->getMemoryUsage();
}

BSONObj BucketCatalog::MinMax::toBSON() const {
    invariant(_type == Type::kObject);

    BSONObjBuilder builder;
    _append(&builder);
    return builder.obj();
}

void BucketCatalog::MinMax::_append(BSONObjBuilder* builder) const {
    invariant(_type == Type::kObject);

    for (const auto& minMax : _object) {
        invariant(minMax.second._type != Type::kUnset);
        if (minMax.second._type == Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart(minMax.first));
            minMax.second._append(&subObj);
        } else if (minMax.second._type == Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart(minMax.first));
            minMax.second._append(&subArr);
        } else {
            builder->append(minMax.second._value.firstElement());
        }
    }
}

void BucketCatalog::MinMax::_append(BSONArrayBuilder* builder) const {
    invariant(_type == Type::kArray);

    for (const auto& minMax : _array) {
        invariant(minMax._type != Type::kUnset);
        if (minMax._type == Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart());
            minMax._append(&subObj);
        } else if (minMax._type == Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart());
            minMax._append(&subArr);
        } else {
            builder->append(minMax._value.firstElement());
        }
    }
}

BSONObj BucketCatalog::MinMax::getUpdates() {
    invariant(_type == Type::kObject);

    BSONObjBuilder builder;
    _appendUpdates(&builder);
    return builder.obj();
}

bool BucketCatalog::MinMax::_appendUpdates(BSONObjBuilder* builder) {
    invariant(_type == Type::kObject || _type == Type::kArray);

    bool appended = false;
    if (_type == Type::kObject) {
        bool hasUpdateSection = false;
        BSONObjBuilder updateSection;
        StringMap<BSONObj> subDiffs;
        for (auto& minMax : _object) {
            invariant(minMax.second._type != Type::kUnset);
            if (minMax.second._updated) {
                if (minMax.second._type == Type::kObject) {
                    BSONObjBuilder subObj(updateSection.subobjStart(minMax.first));
                    minMax.second._append(&subObj);
                } else if (minMax.second._type == Type::kArray) {
                    BSONArrayBuilder subArr(updateSection.subarrayStart(minMax.first));
                    minMax.second._append(&subArr);
                } else {
                    updateSection.append(minMax.second._value.firstElement());
                }
                minMax.second._clearUpdated();
                appended = true;
                hasUpdateSection = true;
            } else if (minMax.second._type != Type::kValue) {
                BSONObjBuilder subDiff;
                if (minMax.second._appendUpdates(&subDiff)) {
                    // An update occurred at a lower level, so append the sub diff.
                    subDiffs[doc_diff::kSubDiffSectionFieldPrefix + minMax.first] = subDiff.obj();
                    appended = true;
                };
            }
        }
        if (hasUpdateSection) {
            builder->append(doc_diff::kUpdateSectionFieldName, updateSection.done());
        }

        // Sub diffs are required to come last.
        for (auto& subDiff : subDiffs) {
            builder->append(subDiff.first, std::move(subDiff.second));
        }
    } else {
        builder->append(doc_diff::kArrayHeader, true);
        DecimalCounter<size_t> count;
        for (auto& minMax : _array) {
            invariant(minMax._type != Type::kUnset);
            if (minMax._updated) {
                std::string updateFieldName = str::stream()
                    << doc_diff::kUpdateSectionFieldName << StringData(count);
                if (minMax._type == Type::kObject) {
                    BSONObjBuilder subObj(builder->subobjStart(updateFieldName));
                    minMax._append(&subObj);
                } else if (minMax._type == Type::kArray) {
                    BSONArrayBuilder subArr(builder->subarrayStart(updateFieldName));
                    minMax._append(&subArr);
                } else {
                    builder->appendAs(minMax._value.firstElement(), updateFieldName);
                }
                minMax._clearUpdated();
                appended = true;
            } else if (minMax._type != Type::kValue) {
                BSONObjBuilder subDiff;
                if (minMax._appendUpdates(&subDiff)) {
                    // An update occurred at a lower level, so append the sub diff.
                    builder->append(str::stream() << doc_diff::kSubDiffSectionFieldPrefix
                                                  << StringData(count),
                                    subDiff.done());
                    appended = true;
                }
            }
            ++count;
        }
    }

    return appended;
}

void BucketCatalog::MinMax::_clearUpdated() {
    invariant(_type != Type::kUnset);

    _updated = false;
    if (_type == Type::kObject) {
        for (auto& minMax : _object) {
            minMax.second._clearUpdated();
        }
    } else if (_type == Type::kArray) {
        for (auto& minMax : _array) {
            minMax._clearUpdated();
        }
    }
}

uint64_t BucketCatalog::MinMax::getMemoryUsage() const {
    return _memoryUsage + (sizeof(MinMax) * (_object.size() + _array.size()));
}

const OID& BucketCatalog::BucketId::operator*() const {
    return *_id;
}

const OID* BucketCatalog::BucketId::operator->() const {
    return _id.get();
}

bool BucketCatalog::BucketId::operator==(const BucketId& other) const {
    return _num == other._num;
}

bool BucketCatalog::BucketId::operator!=(const BucketId& other) const {
    return _num != other._num;
}

bool BucketCatalog::BucketId::operator<(const BucketId& other) const {
    return _num < other._num;
}

BucketCatalog::BucketId BucketCatalog::BucketId::min() {
    return {0};
}

BucketCatalog::BucketId::BucketId(uint64_t num) : _num(num) {}

BucketCatalog::WriteBatch::WriteBatch(const std::shared_ptr<Bucket>& bucket,
                                      const UUID& lsid,
                                      const std::shared_ptr<ExecutionStats>& stats)
    : _bucket{bucket}, _bucketId{bucket->id}, _lsid(lsid), _stats{stats} {}

bool BucketCatalog::WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<BucketCatalog::CommitInfo> BucketCatalog::WriteBatch::getResult() const {
    if (!_promise.getFuture().isReady()) {
        _stats->numWaits.fetchAndAddRelaxed(1);
    }
    return _promise.getFuture().getNoThrow();
}

std::shared_ptr<BucketCatalog::Bucket> BucketCatalog::WriteBatch::bucket() const {
    invariant(!finished());
    return _bucket;
}

BucketCatalog::BucketId BucketCatalog::WriteBatch::bucketId() const {
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

const StringSet& BucketCatalog::WriteBatch::newFieldNamesToBeInserted() const {
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
    return BSON("docs" << _measurements << "bucketMin" << _min << "bucketMax" << _max
                       << "numCommittedMeasurements" << int(_numPreviouslyCommittedMeasurements)
                       << "newFieldNamesToBeInserted"
                       << std::set<std::string>(_newFieldNamesToBeInserted.begin(),
                                                _newFieldNamesToBeInserted.end()));
}

void BucketCatalog::WriteBatch::_addMeasurement(const BSONObj& doc) {
    invariant(_active);
    _measurements.push_back(doc);
}

void BucketCatalog::WriteBatch::_recordNewFields(StringSet&& fields) {
    invariant(_active);
    _newFieldNamesToBeInserted.merge(fields);
}

void BucketCatalog::WriteBatch::_prepareCommit() {
    invariant(_commitRights.load());
    invariant(_active);
    _active = false;
    _numPreviouslyCommittedMeasurements = _bucket->numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    StringSet newFieldNamesToBeInserted;
    for (auto& fieldName : _newFieldNamesToBeInserted) {
        if (!_bucket->fieldNames.contains(fieldName)) {
            _bucket->fieldNames.insert(fieldName);
            newFieldNamesToBeInserted.insert(std::move(fieldName));
        }
    }
    _newFieldNamesToBeInserted = std::move(newFieldNamesToBeInserted);

    _bucket->memoryUsage -= _bucket->min.getMemoryUsage() + _bucket->max.getMemoryUsage();
    for (const auto& doc : _measurements) {
        _bucket->min.update(
            doc, _bucket->metadata.getMetaField(), _bucket->metadata.getCollator(), std::less<>{});
        _bucket->max.update(doc,
                            _bucket->metadata.getMetaField(),
                            _bucket->metadata.getCollator(),
                            std::greater<>{});
    }
    _bucket->memoryUsage += _bucket->min.getMemoryUsage() + _bucket->max.getMemoryUsage();

    const bool isUpdate = _numPreviouslyCommittedMeasurements > 0;
    _min = isUpdate ? _bucket->min.getUpdates() : _bucket->min.toBSON();
    _max = isUpdate ? _bucket->max.getUpdates() : _bucket->max.toBSON();
}

void BucketCatalog::WriteBatch::_finish(const CommitInfo& info) {
    invariant(_commitRights.load());
    invariant(!_active);
    _promise.emplaceValue(info);
    _bucket.reset();
}

void BucketCatalog::WriteBatch::_abort() {
    invariant(_commitRights.load());
    _promise.setError({ErrorCodes::TimeseriesBucketCleared,
                       str::stream() << "Time-series bucket " << *_bucketId << " for "
                                     << _bucket->ns << " was cleared"});
    _bucket.reset();
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
