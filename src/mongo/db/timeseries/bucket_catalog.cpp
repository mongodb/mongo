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
                               totalSize);
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

std::pair<OID, Date_t> generateBucketId(const Date_t& time, const TimeseriesOptions& options) {
    OID bucketId = OID::gen();

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = timeseries::roundTimestampToGranularity(time, options.getGranularity());
    uint64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    bucketId.setTimestamp(roundedSeconds);

    // Now, if we stopped here we could end up with bucket OID collisions. Consider the case where
    // we have the granularity set to 'Hours'. This means we will round down to the nearest day, so
    // any bucket generated on the same machine on the same day will have the same timestamp portion
    // and unique instance portion of the OID. Only the increment will differ. Since we only use 3
    // bytes for the increment portion, we run a serious risk of overflow if we are generating lots
    // of buckets.
    //
    // To address this, we'll take the difference between the actual timestamp and the rounded
    // timestamp and add it to the instance portion of the OID to ensure we can't have a collision.
    // for timestamps generated on the same machine.
    //
    // This leaves open the possibility that in the case of step-down/step-up, we could get a
    // collision if the old primary and the new primary have unique instance bits that differ by
    // less than the maximum rounding difference. This is quite unlikely though, and can be resolved
    // by restarting the new primary. It remains an open question whether we can fix this in a
    // better way.
    // TODO (SERVER-61412): Avoid time-series bucket OID collisions after election
    auto instance = bucketId.getInstanceUnique();
    uint32_t sum = DataView(reinterpret_cast<char*>(instance.bytes)).read<uint32_t>(1) +
        (durationCount<Seconds>(time.toDurationSinceEpoch()) - roundedSeconds);
    DataView(reinterpret_cast<char*>(instance.bytes)).write<uint32_t>(sum, 1);
    bucketId.setInstanceUnique(instance);

    return {bucketId, roundedTime};
}

Status getTimeseriesBucketClearedError(const OID& bucketId,
                                       const boost::optional<NamespaceString>& ns = boost::none) {
    std::string nsIdentification;
    if (ns) {
        nsIdentification.assign(str::stream() << " for namespace " << *ns);
    }
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << bucketId << nsIdentification
                          << " was cleared"};
}
}  // namespace

struct BucketCatalog::ExecutionStats {
    AtomicWord<long long> numBucketInserts;
    AtomicWord<long long> numBucketUpdates;
    AtomicWord<long long> numBucketsOpenedDueToMetadata;
    AtomicWord<long long> numBucketsClosedDueToCount;
    AtomicWord<long long> numBucketsClosedDueToSchemaChange;
    AtomicWord<long long> numBucketsClosedDueToSize;
    AtomicWord<long long> numBucketsClosedDueToTimeForward;
    AtomicWord<long long> numBucketsClosedDueToTimeBackward;
    AtomicWord<long long> numBucketsClosedDueToMemoryThreshold;
    AtomicWord<long long> numCommits;
    AtomicWord<long long> numWaits;
    AtomicWord<long long> numMeasurementsCommitted;
};

class BucketCatalog::Bucket {
public:
    friend class BucketCatalog;

    Bucket(const OID& id, StripeNumber stripe) : _id(id), _stripe(stripe) {}

    /**
     * Returns the ID for the underlying bucket.
     */
    const OID& id() const {
        return _id;
    }

    /**
     * Returns the number of the stripe that owns the bucket
     */
    StripeNumber stripe() const {
        return _stripe;
    }

    // Returns the time associated with the bucket (id)
    Date_t getTime() const {
        return _id.asDateT();
    }

    /**
     * Returns the timefield for the underlying bucket.
     */
    StringData getTimeField() {
        return _timeField;
    }

    /**
     * Returns whether all measurements have been committed.
     */
    bool allCommitted() const {
        return _batches.empty() && !_preparedBatch;
    }

    /**
     * Returns total number of measurements in the bucket.
     */
    uint32_t numMeasurements() const {
        return _numMeasurements;
    }

    /**
     * Determines if the schema for an incoming measurement is incompatible with those already
     * stored in the bucket.
     *
     * Returns true if incompatible
     */
    bool schemaIncompatible(const BSONObj& input,
                            boost::optional<StringData> metaField,
                            const StringData::ComparatorInterface* comparator) {
        auto result = _schema.update(input, metaField, comparator);
        return (result == timeseries::Schema::UpdateStatus::Failed);
    }

private:
    /**
     * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
     * to overflow, we will create a new bucket and recalculate the change to the bucket size
     * and data fields.
     */
    void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
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

    /**
     * Returns whether BucketCatalog::commit has been called at least once on this bucket.
     */
    bool _hasBeenCommitted() const {
        return _numCommittedMeasurements != 0 || _preparedBatch;
    }

    /**
     * Return a pointer to the current, open batch.
     */
    std::shared_ptr<WriteBatch> _activeBatch(OperationId opId,
                                             const std::shared_ptr<ExecutionStats>& stats) {
        auto it = _batches.find(opId);
        if (it == _batches.end()) {
            it =
                _batches
                    .try_emplace(
                        opId, std::make_shared<WriteBatch>(BucketHandle{_id, _stripe}, opId, stats))
                    .first;
        }
        return it->second;
    }

    // The bucket ID for the underlying document
    const OID _id;

    // The stripe which owns this bucket.
    const StripeNumber _stripe;

    // The namespace that this bucket is used for.
    NamespaceString _ns;

    // The metadata of the data that this bucket contains.
    BucketMetadata _metadata;

    // Extra metadata combinations that are supported without normalizing the metadata object.
    static constexpr std::size_t kNumFieldOrderCombinationsWithoutNormalizing = 1;
    boost::container::static_vector<BSONObj, kNumFieldOrderCombinationsWithoutNormalizing>
        _nonNormalizedKeyMetadatas;

    // Top-level field names of the measurements that have been inserted into the bucket.
    StringSet _fieldNames;

    // Time field for the measurements that have been inserted into the bucket.
    std::string _timeField;

    // The minimum and maximum values for each field in the bucket.
    timeseries::MinMax _minmax;

    // The reference schema for measurements in this bucket. May reflect schema of uncommitted
    // measurements.
    timeseries::Schema _schema;

    // The latest time that has been inserted into the bucket.
    Date_t _latestTime;

    // The total size in bytes of the bucket's BSON serialization, including measurements to be
    // inserted.
    uint64_t _size = 0;

    // The total number of measurements in the bucket, including uncommitted measurements and
    // measurements to be inserted.
    uint32_t _numMeasurements = 0;

    // The number of committed measurements in the bucket.
    uint32_t _numCommittedMeasurements = 0;

    // Whether the bucket is full. This can be due to number of measurements, size, or time
    // range.
    bool _full = false;

    // The batch that has been prepared and is currently in the process of being committed, if
    // any.
    std::shared_ptr<WriteBatch> _preparedBatch;

    // Batches, per operation, that haven't been committed or aborted yet.
    stdx::unordered_map<OperationId, std::shared_ptr<WriteBatch>> _batches;

    // If the bucket is in idleBuckets, then its position is recorded here.
    boost::optional<Stripe::IdleList::iterator> _idleListEntry = boost::none;

    // Approximate memory usage of this bucket.
    uint64_t _memoryUsage = sizeof(*this);
};

/**
 * Bundle of information that 'insert' needs to pass down to helper methods that may create a new
 * bucket.
 */
struct BucketCatalog::CreationInfo {
    const BucketKey& key;
    StripeNumber stripe;
    const Date_t& time;
    const TimeseriesOptions& options;
    ExecutionStats* stats;
    ClosedBuckets* closedBuckets;
    bool openedDuetoMetadata = true;
};

BucketCatalog::WriteBatch::WriteBatch(const BucketHandle& bucket,
                                      OperationId opId,
                                      const std::shared_ptr<ExecutionStats>& stats)
    : _bucket{bucket}, _opId(opId), _stats{stats} {}

bool BucketCatalog::WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<BucketCatalog::CommitInfo> BucketCatalog::WriteBatch::getResult() const {
    if (!_promise.getFuture().isReady()) {
        _stats->numWaits.fetchAndAddRelaxed(1);
    }
    return _promise.getFuture().getNoThrow();
}

const BucketCatalog::BucketHandle& BucketCatalog::WriteBatch::bucket() const {
    return _bucket;
}

const std::vector<BSONObj>& BucketCatalog::WriteBatch::measurements() const {
    return _measurements;
}

const BSONObj& BucketCatalog::WriteBatch::min() const {
    return _min;
}

const BSONObj& BucketCatalog::WriteBatch::max() const {
    return _max;
}

const StringMap<std::size_t>& BucketCatalog::WriteBatch::newFieldNamesToBeInserted() const {
    return _newFieldNamesToBeInserted;
}

uint32_t BucketCatalog::WriteBatch::numPreviouslyCommittedMeasurements() const {
    return _numPreviouslyCommittedMeasurements;
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
    _measurements.push_back(doc);
}

void BucketCatalog::WriteBatch::_recordNewFields(NewFieldNames&& fields) {
    for (auto&& field : fields) {
        _newFieldNamesToBeInserted[field] = field.hash();
    }
}

void BucketCatalog::WriteBatch::_prepareCommit(Bucket* bucket) {
    invariant(_commitRights.load());
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
    _promise.emplaceValue(info);
}

void BucketCatalog::WriteBatch::_abort(const Status& status) {
    if (finished()) {
        return;
    }

    _promise.setError(status);
}

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

BSONObj BucketCatalog::getMetadata(const BucketHandle& handle) const {
    auto const& stripe = _stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket = _findBucket(stripe, stripeLock, handle.id);
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

    auto timeElem = doc[options.getTimeField()];
    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << options.getTimeField() << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    auto time = timeElem.Date();

    auto stats = _getExecutionStats(ns);
    invariant(stats);

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = doc[*metaFieldName];
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};
    auto stripeNumber = _getStripeNumber(key);

    ClosedBuckets closedBuckets;
    CreationInfo info{key, stripeNumber, time, options, stats.get(), &closedBuckets};

    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = _useOrCreateBucket(&stripe, stripeLock, info);
    invariant(bucket);

    NewFieldNames newFieldNamesToBeInserted;
    uint32_t newFieldNamesSize = 0;
    uint32_t sizeToBeAdded = 0;
    bucket->_calculateBucketFieldsAndSizeChange(doc,
                                                options.getMetaField(),
                                                &newFieldNamesToBeInserted,
                                                &newFieldNamesSize,
                                                &sizeToBeAdded);

    auto shouldCloseBucket = [&](Bucket* bucket) -> bool {
        if (bucket->schemaIncompatible(doc, metaFieldName, comparator)) {
            stats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(1);
            return true;
        }
        if (bucket->_numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
            stats->numBucketsClosedDueToCount.fetchAndAddRelaxed(1);
            return true;
        }
        if (bucket->_size + sizeToBeAdded > static_cast<std::uint64_t>(gTimeseriesBucketMaxSize)) {
            stats->numBucketsClosedDueToSize.fetchAndAddRelaxed(1);
            return true;
        }
        auto bucketTime = bucket->getTime();
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

    if (!bucket->_ns.isEmpty() && shouldCloseBucket(bucket)) {
        info.openedDuetoMetadata = false;
        bucket = _rollover(&stripe, stripeLock, bucket, info);

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
        bucket->_metadata = key.metadata;

        // The namespace is stored two times: the bucket itself and openBuckets.
        // We don't have a great approximation for the
        // _schema size, so we use initial document size minus metadata as an approximation. Since
        // the metadata itself is stored once, in the bucket, we can combine the two and just use
        // the initial document size. A unique pointer to the bucket is stored once: allBuckets. A
        // raw pointer to the bucket is stored at most twice: openBuckets, idleBuckets.
        bucket->_memoryUsage += (ns.size() * 2) + doc.objsize() + sizeof(Bucket) +
            sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);

        bucket->_schema.update(doc, options.getMetaField(), comparator);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage);

    return InsertResult{batch, closedBuckets};
}

Status BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] { return batch->_promise.getFuture().getNoThrow().getStatus(); };

    if (batch->finished()) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    _waitToCommitBatch(&stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};
    Bucket* bucket =
        _useBucketInState(&stripe, stripeLock, batch->bucket().id, BucketState::kPrepared);

    if (batch->finished()) {
        // Someone may have aborted it while we were waiting.
        return getBatchStatus();
    } else if (!bucket) {
        _abort(&stripe, stripeLock, batch, getTimeseriesBucketClearedError(batch->bucket().id));
        return getBatchStatus();
    }

    auto prevMemoryUsage = bucket->_memoryUsage;
    batch->_prepareCommit(bucket);
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage - prevMemoryUsage);

    return Status::OK();
}

boost::optional<BucketCatalog::ClosedBucket> BucketCatalog::finish(
    std::shared_ptr<WriteBatch> batch, const CommitInfo& info) {
    invariant(!batch->finished());

    boost::optional<ClosedBucket> closedBucket;

    batch->_finish(info);

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket =
        _useBucketInState(&stripe, stripeLock, batch->bucket().id, BucketState::kNormal);
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
        auto it = stripe.allBuckets.find(batch->bucket().id);
        if (it != stripe.allBuckets.end()) {
            bucket = it->second.get();
            bucket->_preparedBatch.reset();
            _abort(&stripe,
                   stripeLock,
                   bucket,
                   nullptr,
                   getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));
        }
    } else if (bucket->allCommitted()) {
        if (bucket->_full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);

            auto it = stripe.allBuckets.find(batch->bucket().id);
            if (it != stripe.allBuckets.end()) {
                bucket = it->second.get();

                closedBucket = ClosedBucket{batch->bucket().id,
                                            bucket->getTimeField().toString(),
                                            bucket->numMeasurements()};

                // Only remove from allBuckets and idleBuckets. If it was marked full, we know
                // that happened in Stripe::rollover, and that there is already a new open
                // bucket for this metadata.
                _markBucketNotIdle(&stripe, stripeLock, bucket);
                _eraseBucketState(batch->bucket().id);

                stripe.allBuckets.erase(batch->bucket().id);
            }
        } else {
            _markBucketIdle(&stripe, stripeLock, bucket);
        }
    }
    return closedBucket;
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);
    invariant(batch->_commitRights.load());

    if (batch->finished()) {
        return;
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    _abort(&stripe, stripeLock, batch, status);
}

void BucketCatalog::clear(const OID& oid) {
    auto result = _setBucketState(oid, BucketState::kCleared);
    if (result && *result == BucketState::kPreparedAndCleared) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throw WriteConflictException();
    }
}

void BucketCatalog::clear(const std::function<bool(const NamespaceString&)>& shouldClear) {
    for (auto& stripe : _stripes) {
        stdx::lock_guard stripeLock{stripe.mutex};
        for (auto it = stripe.allBuckets.begin(); it != stripe.allBuckets.end();) {
            auto nextIt = std::next(it);

            const auto& bucket = it->second;
            if (shouldClear(bucket->_ns)) {
                {
                    stdx::lock_guard catalogLock{_mutex};
                    _executionStats.erase(bucket->_ns);
                }
                _abort(&stripe,
                       stripeLock,
                       bucket.get(),
                       nullptr,
                       getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));
            }

            it = nextIt;
        }
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
    builder->appendNumber("numBucketsClosedDueToSchemaChange",
                          stats->numBucketsClosedDueToSchemaChange.load());
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

BucketCatalog::BucketMetadata::BucketMetadata(BSONElement elem,
                                              const StringData::ComparatorInterface* comparator)
    : _metadataElement(elem), _comparator(comparator) {
    if (_metadataElement) {
        BSONObjBuilder objBuilder;
        // We will get an object of equal size, just with reordered fields.
        objBuilder.bb().reserveBytes(_metadataElement.size());
        normalizeTopLevel(&objBuilder, _metadataElement);
        _metadata = objBuilder.obj();
    }
    // Updates the BSONElement to refer to the copied BSONObj.
    _metadataElement = _metadata.firstElement();
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    return _metadata;
}

StringData BucketCatalog::BucketMetadata::getMetaField() const {
    return StringData(_metadataElement.fieldName());
}

const StringData::ComparatorInterface* BucketCatalog::BucketMetadata::getComparator() const {
    return _comparator;
}

BucketCatalog::BucketKey::BucketKey(const NamespaceString& n, const BucketMetadata& m)
    : ns(n), metadata(m), hash(absl::Hash<BucketKey>{}(*this)) {}

std::size_t BucketCatalog::BucketHasher::operator()(const BucketKey& key) const {
    // Use the default absl hasher.
    return key.hash;
}

BucketCatalog::StripeNumber BucketCatalog::_getStripeNumber(const BucketKey& key) {
    return key.hash % kNumberOfStripes;
}

const BucketCatalog::Bucket* BucketCatalog::_findBucket(const Stripe& stripe,
                                                        WithLock,
                                                        const OID& id,
                                                        ReturnClearedBuckets mode) const {
    auto it = stripe.allBuckets.find(id);
    if (it != stripe.allBuckets.end()) {
        if (mode == ReturnClearedBuckets::kYes) {
            return it->second.get();
        }

        auto state = _getBucketState(id);
        if (state && state != BucketState::kCleared && state != BucketState::kPreparedAndCleared) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                                 WithLock stripeLock,
                                                 const OID& id,
                                                 ReturnClearedBuckets mode) {
    return const_cast<Bucket*>(_findBucket(*stripe, stripeLock, id, mode));
}

BucketCatalog::Bucket* BucketCatalog::_useBucketInState(Stripe* stripe,
                                                        WithLock,
                                                        const OID& id,
                                                        BucketState targetState) {
    auto it = stripe->allBuckets.find(id);
    if (it != stripe->allBuckets.end()) {
        auto state = _setBucketState(it->second->_id, targetState);
        if (state && state != BucketState::kCleared && state != BucketState::kPreparedAndCleared) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useOrCreateBucket(Stripe* stripe,
                                                         WithLock stripeLock,
                                                         const CreationInfo& info) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
        // No open bucket for this metadata.
        return _allocateBucket(stripe, stripeLock, info);
    }

    Bucket* bucket = it->second;

    auto state = _getBucketState(bucket->id());
    if (state == BucketState::kNormal || state == BucketState::kPrepared) {
        _markBucketNotIdle(stripe, stripeLock, bucket);
        return bucket;
    }

    _abort(stripe,
           stripeLock,
           bucket,
           nullptr,
           getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));

    return _allocateBucket(stripe, stripeLock, info);
}

void BucketCatalog::_waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        std::shared_ptr<WriteBatch> current;

        {
            stdx::lock_guard stripeLock{stripe->mutex};
            Bucket* bucket =
                _useBucket(stripe, stripeLock, batch->bucket().id, ReturnClearedBuckets::kNo);
            if (!bucket || batch->finished()) {
                return;
            }

            current = bucket->_preparedBatch;
            if (!current) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->_preparedBatch = batch;
                bucket->_batches.erase(batch->_opId);
                return;
            }
        }

        // We have to wait for someone else to finish.
        current->getResult().getStatus().ignore();  // We don't care about the result.
    }
}

bool BucketCatalog::_removeBucket(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    auto it = stripe->allBuckets.find(bucket->id());
    if (it == stripe->allBuckets.end()) {
        return false;
    }

    invariant(bucket->_batches.empty());
    invariant(!bucket->_preparedBatch);

    _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    _markBucketNotIdle(stripe, stripeLock, bucket);
    stripe->openBuckets.erase({bucket->_ns, bucket->_metadata});
    _eraseBucketState(bucket->id());

    stripe->allBuckets.erase(it);

    return true;
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = _useBucket(stripe, stripeLock, batch->bucket().id, ReturnClearedBuckets::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        batch->_abort(status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    _abort(stripe, stripeLock, bucket, batch, status);
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           Bucket* bucket,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket->_batches) {
        current->_abort(status);
    }
    bucket->_batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket->_preparedBatch) {
        if (prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            prepared->_abort(status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        [[maybe_unused]] bool removed = _removeBucket(stripe, stripeLock, bucket);
    }
}

void BucketCatalog::_markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    stripe->idleBuckets.push_front(bucket);
    bucket->_idleListEntry = stripe->idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    if (bucket->_idleListEntry) {
        stripe->idleBuckets.erase(*bucket->_idleListEntry);
        bucket->_idleListEntry = boost::none;
    }
}

void BucketCatalog::_expireIdleBuckets(Stripe* stripe,
                                       WithLock stripeLock,
                                       ExecutionStats* stats,
                                       std::vector<BucketCatalog::ClosedBucket>* closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numClosed = 0;
    while (!stripe->idleBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numClosed <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe->idleBuckets.back();
        ClosedBucket closed{
            bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()};

        if (_removeBucket(stripe, stripeLock, bucket)) {
            stats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(1);
            closedBuckets->push_back(closed);
            ++numClosed;
        }
    }
}

BucketCatalog::Bucket* BucketCatalog::_allocateBucket(Stripe* stripe,
                                                      WithLock stripeLock,
                                                      const CreationInfo& info) {
    _expireIdleBuckets(stripe, stripeLock, info.stats, info.closedBuckets);

    auto [bucketId, roundedTime] = generateBucketId(info.time, info.options);

    auto [it, inserted] =
        stripe->allBuckets.try_emplace(bucketId, std::make_unique<Bucket>(bucketId, info.stripe));
    tassert(6130900, "Expected bucket to be inserted", inserted);
    Bucket* bucket = it->second.get();
    stripe->openBuckets[info.key] = bucket;
    _initializeBucketState(bucketId);

    if (info.openedDuetoMetadata) {
        info.stats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(1);
    }

    bucket->_timeField = info.options.getTimeField().toString();

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->_minmax.update(
        controlDoc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());

    return bucket;
}

BucketCatalog::Bucket* BucketCatalog::_rollover(Stripe* stripe,
                                                WithLock stripeLock,
                                                Bucket* bucket,
                                                const CreationInfo& info) {

    if (bucket->allCommitted()) {
        // The bucket does not contain any measurements that are yet to be committed, so we can
        // remove it now.
        info.closedBuckets->push_back(ClosedBucket{
            bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()});

        bool removed = _removeBucket(stripe, stripeLock, bucket);
        invariant(removed);
    } else {
        // We must keep the bucket around until it is committed, just mark it full so it we know to
        // clean it up when the last batch finishes.
        bucket->_full = true;
    }

    return _allocateBucket(stripe, stripeLock, info);
}

std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }

    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return res.first->second;
}

const std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) const {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{_mutex};

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

void BucketCatalog::_initializeBucketState(const OID& id) {
    stdx::lock_guard catalogLock{_mutex};
    _bucketStates.emplace(id, BucketState::kNormal);
}

void BucketCatalog::_eraseBucketState(const OID& id) {
    stdx::lock_guard catalogLock{_mutex};
    _bucketStates.erase(id);
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::_getBucketState(const OID& id) const {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _bucketStates.find(id);
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::_setBucketState(const OID& id,
                                                                           BucketState target) {
    stdx::lock_guard catalogLock{_mutex};
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

class BucketCatalog::ServerStatus : public ServerStatusSection {
    struct BucketCounts {
        BucketCounts& operator+=(const BucketCounts& other) {
            if (&other != this) {
                all += other.all;
                open += other.open;
                idle += other.idle;
            }
            return *this;
        }

        std::size_t all = 0;
        std::size_t open = 0;
        std::size_t idle = 0;
    };

    BucketCounts _getBucketCounts(const BucketCatalog& catalog) const {
        BucketCounts sum;
        for (auto const& stripe : catalog._stripes) {
            stdx::lock_guard stripeLock{stripe.mutex};
            sum += {stripe.allBuckets.size(), stripe.openBuckets.size(), stripe.idleBuckets.size()};
        }
        return sum;
    }

public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            stdx::lock_guard catalogLock{bucketCatalog._mutex};
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto counts = _getBucketCounts(bucketCatalog);
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(counts.all));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));
        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo
