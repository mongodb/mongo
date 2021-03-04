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

KeyString::Value toKeyString(const BSONObj& obj, const CollatorInterface* collator) {
    // TODO SERVER-54736: Change KeyString API to allow building subobjects in place and avoid
    // temporary BSONObjBuilder
    BSONObjBuilder objBuilder;
    normalizeObject(&objBuilder, obj);

    KeyString::StringTransformFn getComparisonString = [&](StringData stringData) {
        return collator->getComparisonString(stringData);
    };
    const KeyString::StringTransformFn& transform = collator ? getComparisonString : nullptr;

    KeyString::HeapBuilder ksBuilder{KeyString::Version::kLatestVersion, KeyString::ALL_ASCENDING};
    for (auto&& elem : objBuilder.obj()) {
        ksBuilder.appendBSONElement(elem, transform);
    }
    ksBuilder.appendDiscriminator(KeyString::Discriminator::kInclusive);
    return ksBuilder.release();
}
}  // namespace

const std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::kEmptyStats{
    std::make_shared<BucketCatalog::ExecutionStats>()};

BSONObj BucketCatalog::CommitData::toBSON() const {
    return BSON("docs" << docs << "bucketMin" << bucketMin << "bucketMax" << bucketMax
                       << "numCommittedMeasurements" << int(numCommittedMeasurements)
                       << "newFieldNamesToBeInserted"
                       << std::set<std::string>(newFieldNamesToBeInserted.begin(),
                                                newFieldNamesToBeInserted.end()));
}

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

StatusWith<BucketCatalog::InsertResult> BucketCatalog::insert(OperationContext* opCtx,
                                                              const NamespaceString& ns,
                                                              const BSONObj& doc) {
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
        auto bucketTime = (*bucket).id().getTime();
        if (time - bucketTime >= kTimeseriesBucketMaxTimeRange) {
            stats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(1);
            return true;
        }
        if (time < bucketTime) {
            if (!(*bucket)->hasBeenCommitted() &&
                (*bucket)->latestTime - time < kTimeseriesBucketMaxTimeRange) {
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

    // If this is the first uncommitted measurement, the caller is the committer. Otherwise, it is a
    // waiter.
    boost::optional<Future<CommitInfo>> commitInfoFuture;
    if (bucket->numMeasurements > bucket->numCommittedMeasurements) {
        auto [promise, future] = makePromiseFuture<CommitInfo>();
        bucket->promises.push(std::move(promise));
        commitInfoFuture = std::move(future);
    } else {
        bucket->promises.push(boost::none);
    }

    bucket->numWriters++;
    bucket->numMeasurements++;
    bucket->size += sizeToBeAdded;
    bucket->measurementsToBeInserted.push_back(doc);
    bucket->newFieldNamesToBeInserted.merge(newFieldNamesToBeInserted);
    if (time > bucket->latestTime) {
        bucket->latestTime = time;
    }
    if (bucket->ns.isEmpty()) {
        // The namespace and metadata only need to be set if this bucket was newly created.
        bucket->ns = ns;
        bucket->metadata = std::get<BucketMetadata>(key);

        // The namespace is stored three times: the bucket itself, _bucketIds, and _nsBuckets.
        // The metadata is stored two times: the bucket itself and _bucketIds.
        // The bucketId is stored four times: _buckets, _bucketIds, _nsBuckets, and
        // _idleBuckets.
        bucket->memoryUsage += (ns.size() * 3) + (bucket->metadata.toBSON().objsize() * 2) +
            ((sizeof(BucketId) + sizeof(OID)) * 4);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->memoryUsage);
    }
    bucket->memoryUsage -= bucket->min.getMemoryUsage() + bucket->max.getMemoryUsage();
    bucket->min.update(doc, options.getMetaField(), viewDef->defaultCollator(), std::less<>());
    bucket->max.update(doc, options.getMetaField(), viewDef->defaultCollator(), std::greater<>());
    bucket->memoryUsage +=
        newFieldNamesSize + bucket->min.getMemoryUsage() + bucket->max.getMemoryUsage();
    _memoryUsage.fetchAndAdd(bucket->memoryUsage);

    return {InsertResult{bucket.id(), std::move(commitInfoFuture)}};
}

BucketCatalog::CommitData BucketCatalog::commit(const BucketId& bucketId,
                                                boost::optional<CommitInfo> previousCommitInfo) {
    BucketAccess bucket{this, bucketId};
    invariant(bucket);

    // The only case in which previousCommitInfo should not be provided is the first time a given
    // committer calls this function.
    invariant(!previousCommitInfo || bucket->hasBeenCommitted());

    auto newFieldNamesToBeInserted = bucket->newFieldNamesToBeInserted;
    bucket->fieldNames.merge(bucket->newFieldNamesToBeInserted);
    bucket->newFieldNamesToBeInserted.clear();

    std::vector<BSONObj> measurements;
    bucket->measurementsToBeInserted.swap(measurements);

    auto stats = _getExecutionStats(bucket->ns);
    stats->numMeasurementsCommitted.fetchAndAddRelaxed(measurements.size());

    // Inform waiters that their measurements have been committed.
    for (uint32_t i = 0; i < bucket->numPendingCommitMeasurements; i++) {
        if (auto& promise = bucket->promises.front()) {
            promise->emplaceValue(*previousCommitInfo);
        }
        bucket->promises.pop();
    }
    if (bucket->numPendingCommitMeasurements) {
        stats->numWaits.fetchAndAddRelaxed(bucket->numPendingCommitMeasurements - 1);
    }

    bucket->numWriters -= bucket->numPendingCommitMeasurements;
    bucket->numCommittedMeasurements +=
        std::exchange(bucket->numPendingCommitMeasurements, measurements.size());

    auto [bucketMin, bucketMax] = [&bucket]() -> std::pair<BSONObj, BSONObj> {
        if (bucket->numCommittedMeasurements == 0) {
            return {bucket->min.toBSON(), bucket->max.toBSON()};
        } else {
            return {bucket->min.getUpdates(), bucket->max.getUpdates()};
        }
    }();

    auto allCommitted = measurements.empty();
    CommitData data = {std::move(measurements),
                       std::move(bucketMin),
                       std::move(bucketMax),
                       bucket->numCommittedMeasurements,
                       std::move(newFieldNamesToBeInserted)};

    if (allCommitted) {
        if (bucket->full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _memoryUsage.fetchAndSubtract(bucket->memoryUsage);

            invariant(bucket->promises.empty());

            bucket.release();
            auto lk = _lockExclusive();

            // Only remove from _nsBuckets and _buckets. If it was marked full, we know that
            // happened in BucketAccess::rollover, and that there is already a new open bucket for
            // this metadata.
            auto it = _buckets.find(bucketId);
            invariant(it != _buckets.end());
            _nsBuckets.erase({std::move(it->second->ns), bucketId});
            _buckets.erase(it);
        } else if (bucket->numWriters == 0) {
            _markBucketIdle(bucketId);
        }
    } else {
        stats->numCommits.fetchAndAddRelaxed(1);
        if (bucket->numCommittedMeasurements == 0) {
            stats->numBucketInserts.fetchAndAddRelaxed(1);
        } else {
            stats->numBucketUpdates.fetchAndAddRelaxed(1);
        }
    }

    return data;
}

void BucketCatalog::clear(const BucketId& bucketId) {
    BucketAccess bucket{this, bucketId};
    if (!bucket) {
        return;
    }

    // Retain pointer to bucket, release so we can get an exclusive lock.
    std::shared_ptr<Bucket> underlyingBucket{bucket};
    bucket.release();
    auto lk = _lockExclusive();

    {
        stdx::lock_guard blk{underlyingBucket->lock};
        while (!underlyingBucket->promises.empty()) {
            if (auto& promise = underlyingBucket->promises.front()) {
                promise->setError({ErrorCodes::TimeseriesBucketCleared,
                                   str::stream() << "Time-series bucket " << *bucketId << " for "
                                                 << underlyingBucket->ns << " was cleared"});
            }
            underlyingBucket->promises.pop();
        }
    }

    _removeBucket(bucketId);
}

void BucketCatalog::clear(const NamespaceString& ns) {
    auto lk = _lockExclusive();

    auto shouldClear = [&ns](const NamespaceString& bucketNs) {
        return ns.coll().empty() ? ns.db() == bucketNs.db() : ns == bucketNs;
    };

    for (auto it = _nsBuckets.lower_bound({ns, BucketIdInternal::min()});
         it != _nsBuckets.end() && shouldClear(std::get<NamespaceString>(*it));) {
        auto nextIt = std::next(it);
        {
            stdx::lock_guard statsLock{_executionStatsLock};
            _executionStats.erase(std::get<NamespaceString>(*it));
        }
        _removeBucket(std::get<BucketId>(*it), it);
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
    return _stripedMutex.lockShared();
}

BucketCatalog::StripedMutex::ExclusiveLock BucketCatalog::_lockExclusive() const {
    return _stripedMutex.lockExclusive();
}

void BucketCatalog::_removeBucket(const BucketId& bucketId,
                                  boost::optional<NsBuckets::iterator> nsBucketsIt,
                                  boost::optional<IdleBuckets::iterator> idleBucketsIt) {
    auto it = _buckets.find(bucketId);
    {
        // Take a lock on the bucket so we guarantee no one else is accessing it. We can release it
        // right away since no one else can take it again without taking the catalog lock, which we
        // also hold outside this method.
        stdx::lock_guard<Mutex> lk{it->second->lock};
        _memoryUsage.fetchAndSubtract(it->second->memoryUsage);
    }

    if (nsBucketsIt) {
        _nsBuckets.erase(*nsBucketsIt);
    } else {
        _nsBuckets.erase({it->second->ns, it->first});
    }

    if (idleBucketsIt) {
        _markBucketNotIdle(*idleBucketsIt);
    } else {
        _markBucketNotIdle(it->first);
    }

    _bucketIds.erase({std::move(it->second->ns), std::move(it->second->metadata)});
    _buckets.erase(it);
}

void BucketCatalog::_markBucketIdle(const BucketId& bucketId) {
    stdx::lock_guard lk{_idleBucketsLock};
    _idleBuckets.insert(bucketId);
}

void BucketCatalog::_markBucketNotIdle(const BucketId& bucketId) {
    stdx::lock_guard lk{_idleBucketsLock};
    _idleBuckets.erase(bucketId);
}

void BucketCatalog::_markBucketNotIdle(const IdleBuckets::iterator& it) {
    _idleBuckets.erase(it);
}

void BucketCatalog::_expireIdleBuckets(ExecutionStats* stats) {
    stdx::lock_guard lk{_idleBucketsLock};
    while (!_idleBuckets.empty() &&
           _memoryUsage.load() >
               static_cast<std::uint64_t>(gTimeseriesIdleBucketExpiryMemoryUsageThreshold)) {
        _removeBucket(*_idleBuckets.begin(), boost::none, _idleBuckets.begin());
        stats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(1);
    }
}

std::size_t BucketCatalog::_numberOfIdleBuckets() const {
    stdx::lock_guard lk{_idleBucketsLock};
    return _idleBuckets.size();
}

BucketCatalog::BucketIdInternal BucketCatalog::_createNewBucketId(const Date_t& time,
                                                                  ExecutionStats* stats) {
    _expireIdleBuckets(stats);
    return BucketIdInternal{time, ++_bucketNum};
}

std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) {
    stdx::lock_guard lock(_executionStatsLock);

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }

    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return res.first->second;
}

const std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) const {
    stdx::lock_guard lock(_executionStatsLock);

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

BucketCatalog::BucketMetadata::BucketMetadata(BSONObj&& obj,
                                              std::shared_ptr<const ViewDefinition>& v)
    : _metadata(obj), _view(v), _keyString(toKeyString(_metadata, _view->defaultCollator())) {}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _view->defaultCollator() == other._view->defaultCollator() &&
        _keyString == other._keyString;
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    return _metadata;
}

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
    return numCommittedMeasurements != 0 || numPendingCommitMeasurements != 0;
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog,
                                          const std::tuple<NamespaceString, BucketMetadata>& key,
                                          ExecutionStats* stats,
                                          const Date_t& time)
    : _catalog(catalog), _key(&key), _stats(stats), _time(&time) {
    // precompute the hash outside the lock, since it's expensive
    auto hasher = _catalog->_bucketIds.hash_function();
    auto hash = hasher(*_key);

    bool bucketExisted = _findAndLockExisting(hash);
    if (bucketExisted) {
        return;
    }

    _findOrCreateAndLock(hash);
}

BucketCatalog::BucketAccess::BucketAccess(BucketCatalog* catalog, const BucketId& bucketId)
    : _catalog(catalog) {
    auto lk = _catalog->_lockShared();

    auto it = _catalog->_buckets.find(bucketId);
    if (it != _catalog->_buckets.end()) {
        _bucket = it->second;
        _acquire();
    }
}

BucketCatalog::BucketAccess::~BucketAccess() {
    if (isLocked()) {
        release();
    }
}

bool BucketCatalog::BucketAccess::_findAndLockExisting(std::size_t hash) {
    auto lk = _catalog->_lockShared();
    {
        auto it = _catalog->_bucketIds.find(*_key, hash);
        if (it == _catalog->_bucketIds.end()) {
            // Bucket does not exist.
            return false;
        }
        _id = it->second;
    }

    _catalog->_markBucketNotIdle(_id);
    auto it = _catalog->_buckets.find(_id);
    if (it == _catalog->_buckets.end()) {
        // Bucket does not exist.
        return false;
    }
    _bucket = it->second;

    _acquire();

    return true;
}

void BucketCatalog::BucketAccess::_findOrCreateAndLock(std::size_t hash) {
    auto lk = _catalog->_lockExclusive();
    {
        auto it = _catalog->_bucketIds.find(*_key, hash);
        if (it == _catalog->_bucketIds.end()) {
            // No open bucket for this metadata, allocate an ID.
            it = _catalog->_bucketIds.insert({*_key, _catalog->_createNewBucketId(*_time, _stats)})
                     .first;
            _catalog->_nsBuckets.insert({std::get<mongo::NamespaceString>(*_key), it->second});
            _stats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(1);
        }
        _id = it->second;
    }

    _catalog->_markBucketNotIdle(_id);
    auto it = _catalog->_buckets.find(_id);
    if (it != _catalog->_buckets.end()) {
        _bucket = it->second;
    } else {
        // No bucket with this ID, create it.
        _create(_id);
    }
    _acquire();
}

void BucketCatalog::BucketAccess::_acquire() {
    invariant(_bucket);
    _guard = stdx::unique_lock<Mutex>(_bucket->lock);
}

void BucketCatalog::BucketAccess::_create(const BucketId& id) {
    auto [it, _] = _catalog->_buckets.emplace(id, std::make_shared<Bucket>());
    _bucket = it->second;
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
    auto hasher = _catalog->_bucketIds.hash_function();
    auto hash = hasher(*_key);

    auto lk = _catalog->_lockExclusive();
    BucketIdInternal* mappedId;
    {
        auto it = _catalog->_bucketIds.find(*_key, hash);
        if (it == _catalog->_bucketIds.end()) {
            // A bucket for this namespace and metadata pair does not yet exist.
            it = _catalog->_bucketIds.insert({*_key, _catalog->_createNewBucketId(*_time, _stats)})
                     .first;
            _catalog->_nsBuckets.insert({std::get<mongo::NamespaceString>(*_key), it->second});
            _stats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(1);
        }
        _id = it->second;
        mappedId = &it->second;
    }

    _catalog->_markBucketNotIdle(_id);
    auto it = _catalog->_buckets.find(_id);
    if (it != _catalog->_buckets.end()) {
        _bucket = it->second;
    } else {
        _create(_id);
    }
    _acquire();

    // Recheck if still full now that we've reacquired the bucket.
    bool newBucket = oldId != _id;  // Only record stats if bucket has changed, don't double-count.
    if (!newBucket || isBucketFull(this)) {
        // The bucket is indeed full, so create a new one.
        if (_bucket->numPendingCommitMeasurements == 0 &&
            _bucket->numCommittedMeasurements == _bucket->numMeasurements) {
            // The bucket does not contain any measurements that are yet to be committed, so we can
            // remove it now. Otherwise, we must keep the bucket around until it is committed.
            _catalog->_memoryUsage.fetchAndSubtract(_bucket->memoryUsage);

            release();

            _catalog->_buckets.erase(_id);
            _catalog->_nsBuckets.erase({std::get<NamespaceString>(*_key), _id});
        } else {
            _bucket->full = true;
            release();
        }

        _id = _catalog->_createNewBucketId(*_time, _stats);
        *mappedId = _id;
        _catalog->_nsBuckets.insert({std::get<mongo::NamespaceString>(*_key), _id});
        _create(_id);
        _acquire();
    }
}

const BucketCatalog::BucketIdInternal& BucketCatalog::BucketAccess::id() {
    return _id;
}

void BucketCatalog::BucketAccess::setTime() {
    invariant(isLocked());
    invariant(_key);
    invariant(_stats);
    invariant(_time);

    _id.setTime(*_time);
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

BucketCatalog::BucketIdInternal BucketCatalog::BucketIdInternal::min() {
    return {{}, 0};
}

BucketCatalog::BucketIdInternal::BucketIdInternal(const Date_t& time, uint64_t num)
    : BucketId(num) {
    setTime(time);
}

Date_t BucketCatalog::BucketIdInternal::getTime() const {
    return _id->asDateT();
}

void BucketCatalog::BucketIdInternal::setTime(const Date_t& time) {
    _id->setTimestamp(durationCount<Seconds>(time.toDurationSinceEpoch()));
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
            stdx::lock_guard eslk{bucketCatalog._executionStatsLock};
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto lk = bucketCatalog._lockShared();
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", bucketCatalog._buckets.size());
        builder.appendNumber("numOpenBuckets", bucketCatalog._bucketIds.size());
        builder.appendNumber("numIdleBuckets", bucketCatalog._numberOfIdleBuckets());
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));
        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo
