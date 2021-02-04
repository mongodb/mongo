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
}  // namespace

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

BSONObj BucketCatalog::getMetadata(const OID& bucketId) const {
    stdx::lock_guard lk(_mutex);
    auto it = _buckets.find(bucketId);
    if (it == _buckets.cend()) {
        return {};
    }
    const auto& bucket = it->second;
    return bucket.metadata.metadata;
}

BucketCatalog::InsertResult BucketCatalog::insert(OperationContext* opCtx,
                                                  const NamespaceString& ns,
                                                  const BSONObj& doc) {
    stdx::lock_guard lk(_mutex);

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
    auto key = std::make_pair(ns, BucketMetadata{metadata.obj(), viewDef});

    auto& stats = _executionStats[ns];

    auto time = doc[options.getTimeField()].Date();
    auto createNewBucketId = [&] {
        _expireIdleBuckets(&stats);
        auto bucketId = OID::gen();
        bucketId.setTimestamp(durationCount<Seconds>(time.toDurationSinceEpoch()));
        return bucketId;
    };

    auto it = _bucketIds.find(key);
    if (it == _bucketIds.end()) {
        // A bucket for this namespace and metadata pair does not yet exist.
        it = _bucketIds.insert({std::move(key), createNewBucketId()}).first;
        _orderedBuckets.insert({ns, it->first.second, it->second});
        stats.numBucketsOpenedDueToMetadata++;
    }

    _idleBuckets.erase(it->second);
    auto bucket = &_buckets[it->second];

    StringSet newFieldNamesToBeInserted;
    uint32_t newFieldNamesSize = 0;
    uint32_t sizeToBeAdded = 0;
    bucket->calculateBucketFieldsAndSizeChange(doc,
                                               options.getMetaField(),
                                               &newFieldNamesToBeInserted,
                                               &newFieldNamesSize,
                                               &sizeToBeAdded);

    auto isBucketFull = [&]() {
        if (bucket->numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
            stats.numBucketsClosedDueToCount++;
            return true;
        }
        if (bucket->size + sizeToBeAdded > static_cast<std::uint64_t>(gTimeseriesBucketMaxSize)) {
            stats.numBucketsClosedDueToSize++;
            return true;
        }
        auto bucketTime = it->second.asDateT();
        if (time - bucketTime >= kTimeseriesBucketMaxTimeRange) {
            stats.numBucketsClosedDueToTimeForward++;
            return true;
        }
        if (time < bucketTime) {
            stats.numBucketsClosedDueToTimeBackward++;
            return true;
        }
        return false;
    };

    if (!bucket->ns.isEmpty() && isBucketFull()) {
        // The bucket is full, so create a new one.
        if (bucket->numPendingCommitMeasurements == 0 &&
            bucket->numCommittedMeasurements == bucket->numMeasurements) {
            // The bucket does not contain any measurements that are yet to be committed, so we can
            // remove it now. Otherwise, we must keep the bucket around until it is committed.
            _memoryUsage -= bucket->memoryUsage;
            _buckets.erase(it->second);
            _orderedBuckets.erase({it->first.first, it->first.second, it->second});
        } else {
            bucket->full = true;
        }
        it->second = createNewBucketId();
        _orderedBuckets.insert({ns, it->first.second, it->second});
        bucket = &_buckets[it->second];
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
    }

    bucket->numWriters++;
    bucket->numMeasurements++;
    bucket->size += sizeToBeAdded;
    bucket->measurementsToBeInserted.push_back(doc);
    bucket->newFieldNamesToBeInserted.merge(newFieldNamesToBeInserted);
    if (bucket->ns.isEmpty()) {
        // The namespace and metadata only need to be set if this bucket was newly created.
        bucket->ns = ns;
        bucket->metadata = it->first.second;

        // The namespace is stored three times: the bucket itself, _bucketIds, and _orderedBuckets.
        // The metadata is stored three times: the bucket itself, _bucketIds, and _orderedBuckets.
        // The bucketId is stored four times: _buckets, _bucketIds, _orderedBuckets, and
        // _idleBuckets.
        bucket->memoryUsage +=
            (ns.size() * 3) + (bucket->metadata.metadata.objsize() * 3) + (sizeof(OID) * 4);
    } else {
        _memoryUsage -= bucket->memoryUsage;
    }
    bucket->memoryUsage -= bucket->min.getMemoryUsage() + bucket->max.getMemoryUsage();
    bucket->min.update(doc, options.getMetaField(), viewDef->defaultCollator(), std::less<>());
    bucket->max.update(doc, options.getMetaField(), viewDef->defaultCollator(), std::greater<>());
    bucket->memoryUsage +=
        newFieldNamesSize + bucket->min.getMemoryUsage() + bucket->max.getMemoryUsage();
    _memoryUsage += bucket->memoryUsage;

    return {it->second, std::move(commitInfoFuture)};
}

BucketCatalog::CommitData BucketCatalog::commit(const OID& bucketId,
                                                boost::optional<CommitInfo> previousCommitInfo) {
    stdx::lock_guard lk(_mutex);
    auto it = _buckets.find(bucketId);
    invariant(it != _buckets.end());
    auto& bucket = it->second;

    // The only case in which previousCommitInfo should not be provided is the first time a given
    // committer calls this function.
    invariant(!previousCommitInfo || bucket.numCommittedMeasurements != 0 ||
              bucket.numPendingCommitMeasurements != 0);

    auto newFieldNamesToBeInserted = bucket.newFieldNamesToBeInserted;
    bucket.fieldNames.merge(bucket.newFieldNamesToBeInserted);
    bucket.newFieldNamesToBeInserted.clear();

    std::vector<BSONObj> measurements;
    bucket.measurementsToBeInserted.swap(measurements);

    auto& stats = _executionStats[bucket.ns];
    stats.numMeasurementsCommitted += measurements.size();

    // Inform waiters that their measurements have been committed.
    for (uint32_t i = 1; i < bucket.numPendingCommitMeasurements; i++) {
        bucket.promises.front().emplaceValue(*previousCommitInfo);
        bucket.promises.pop();
    }
    if (bucket.numPendingCommitMeasurements) {
        stats.numWaits += bucket.numPendingCommitMeasurements - 1;
    }

    bucket.numWriters -= bucket.numPendingCommitMeasurements;
    bucket.numCommittedMeasurements +=
        std::exchange(bucket.numPendingCommitMeasurements, measurements.size());

    auto [bucketMin, bucketMax] = [&bucket]() -> std::pair<BSONObj, BSONObj> {
        if (bucket.numCommittedMeasurements == 0) {
            return {bucket.min.toBSON(), bucket.max.toBSON()};
        } else {
            return {bucket.min.getUpdates(), bucket.max.getUpdates()};
        }
    }();

    auto allCommitted = measurements.empty();
    CommitData data = {std::move(measurements),
                       std::move(bucketMin),
                       std::move(bucketMax),
                       bucket.numCommittedMeasurements,
                       std::move(newFieldNamesToBeInserted)};

    if (allCommitted) {
        if (bucket.full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _memoryUsage -= bucket.memoryUsage;
            _orderedBuckets.erase(
                {std::move(it->second.ns), std::move(it->second.metadata), bucketId});
            _buckets.erase(it);
        } else if (bucket.numWriters == 0) {
            _idleBuckets.insert(bucketId);
        }
    } else {
        stats.numCommits++;
        if (bucket.numCommittedMeasurements == 0) {
            stats.numBucketInserts++;
        } else {
            stats.numBucketUpdates++;
        }
    }

    return data;
}

void BucketCatalog::clear(const OID& bucketId) {
    stdx::lock_guard lk(_mutex);

    auto it = _buckets.find(bucketId);
    if (it == _buckets.end()) {
        return;
    }
    auto& bucket = it->second;

    while (!bucket.promises.empty()) {
        bucket.promises.front().setError({ErrorCodes::TimeseriesBucketCleared,
                                          str::stream() << "Time-series bucket " << bucketId
                                                        << " for " << bucket.ns << " was cleared"});
        bucket.promises.pop();
    }

    _removeBucket(bucketId);
}

void BucketCatalog::clear(const NamespaceString& ns) {
    stdx::lock_guard lk(_mutex);

    auto shouldClear = [&ns](const NamespaceString& bucketNs) {
        return ns.coll().empty() ? ns.db() == bucketNs.db() : ns == bucketNs;
    };

    for (auto it = _orderedBuckets.lower_bound({ns, {}, {}});
         it != _orderedBuckets.end() && shouldClear(std::get<NamespaceString>(*it));) {
        auto nextIt = std::next(it);
        _executionStats.erase(std::get<NamespaceString>(*it));
        _removeBucket(std::get<OID>(*it), it);
        it = nextIt;
    }
}

void BucketCatalog::clear(StringData dbName) {
    clear(NamespaceString(dbName, ""));
}

void BucketCatalog::appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const {
    stdx::lock_guard lk(_mutex);

    auto it = _executionStats.find(ns);
    const auto& stats = it == _executionStats.end() ? ExecutionStats() : it->second;

    builder->appendNumber("numBucketInserts", stats.numBucketInserts);
    builder->appendNumber("numBucketUpdates", stats.numBucketUpdates);
    builder->appendNumber("numBucketsOpenedDueToMetadata", stats.numBucketsOpenedDueToMetadata);
    builder->appendNumber("numBucketsClosedDueToCount", stats.numBucketsClosedDueToCount);
    builder->appendNumber("numBucketsClosedDueToSize", stats.numBucketsClosedDueToSize);
    builder->appendNumber("numBucketsClosedDueToTimeForward",
                          stats.numBucketsClosedDueToTimeForward);
    builder->appendNumber("numBucketsClosedDueToTimeBackward",
                          stats.numBucketsClosedDueToTimeBackward);
    builder->appendNumber("numBucketsClosedDueToMemoryThreshold",
                          stats.numBucketsClosedDueToMemoryThreshold);
    builder->appendNumber("numCommits", stats.numCommits);
    builder->appendNumber("numWaits", stats.numWaits);
    builder->appendNumber("numMeasurementsCommitted", stats.numMeasurementsCommitted);
    if (stats.numCommits) {
        builder->appendNumber("avgNumMeasurementsPerCommit",
                              stats.numMeasurementsCommitted / stats.numCommits);
    }
}

void BucketCatalog::_removeBucket(const OID& bucketId,
                                  boost::optional<OrderedBuckets::iterator> orderedBucketsIt,
                                  boost::optional<IdleBuckets::iterator> idleBucketsIt) {
    auto it = _buckets.find(bucketId);
    _memoryUsage -= it->second.memoryUsage;

    if (orderedBucketsIt) {
        _orderedBuckets.erase(*orderedBucketsIt);
    } else {
        _orderedBuckets.erase({it->second.ns, it->second.metadata, it->first});
    }

    if (idleBucketsIt) {
        _idleBuckets.erase(*idleBucketsIt);
    } else {
        _idleBuckets.erase(it->first);
    }

    _bucketIds.erase({std::move(it->second.ns), std::move(it->second.metadata)});
    _buckets.erase(it);
}

void BucketCatalog::_expireIdleBuckets(ExecutionStats* stats) {
    while (!_idleBuckets.empty() &&
           _memoryUsage >
               static_cast<std::uint64_t>(gTimeseriesIdleBucketExpiryMemoryUsageThreshold)) {
        _removeBucket(*_idleBuckets.begin(), boost::none, _idleBuckets.begin());
        stats->numBucketsClosedDueToMemoryThreshold++;
    }
}

bool BucketCatalog::BucketMetadata::operator<(const BucketMetadata& other) const {
    auto size = metadata.objsize();
    auto otherSize = other.metadata.objsize();
    auto cmp = std::memcmp(metadata.objdata(), other.metadata.objdata(), std::min(size, otherSize));
    return cmp == 0 && size != otherSize ? size < otherSize : cmp < 0;
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return view->defaultCollator() == other.view->defaultCollator() &&
        UnorderedFieldsBSONObjComparator(view->defaultCollator())
            .compare(metadata, other.metadata) == 0;
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

class BucketCatalog::ServerStatus : public ServerStatusSection {
public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        stdx::lock_guard lk(bucketCatalog._mutex);

        if (bucketCatalog._executionStats.empty()) {
            return {};
        }

        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", bucketCatalog._buckets.size());
        builder.appendNumber("numOpenBuckets", bucketCatalog._bucketIds.size());
        builder.appendNumber("numIdleBuckets", bucketCatalog._idleBuckets.size());
        builder.appendNumber("memoryUsage", static_cast<long long>(bucketCatalog._memoryUsage));
        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo
