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
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view_catalog.h"

namespace mongo {
namespace {
const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();

BSONObj updateMinOrMax(const BSONObj& doc,
                       BSONObj&& minOrMax,
                       boost::optional<StringData> metaField,
                       const std::function<bool(int, int)>& comp) {
    StringDataSet fieldsToUpdate;

    for (const auto& elem : doc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == metaField) {
            continue;
        }

        auto minOrMaxElem = minOrMax[fieldName];
        if (elem.type() == Object) {
            auto subdoc = elem.Obj();

            // If comparing two object, do so element-wise.
            if (minOrMaxElem.type() == Object) {
                minOrMax = minOrMax.addField(
                    BSON(fieldName << updateMinOrMax(subdoc, minOrMaxElem.Obj(), metaField, comp))
                        .firstElement());
                continue;
            }

            // Remove empty subdocuments.
            StringDataSet emptySubdocs;
            for (const auto& subelem : subdoc) {
                if (subelem.type() == Object && subelem.Obj().isEmpty()) {
                    emptySubdocs.insert(subelem.fieldNameStringData());
                }
            }
            if (!emptySubdocs.empty()) {
                subdoc = subdoc.removeFields(emptySubdocs);
            }

            if (!subdoc.isEmpty() &&
                (minOrMaxElem.eoo() || comp(subdoc.woCompare(minOrMaxElem.wrap()), 0))) {
                minOrMax = minOrMax.addField(BSON(fieldName << subdoc).firstElement());
            }

            continue;
        }

        // If comparing two arrays, do so element-wise.
        if (elem.type() == Array && minOrMaxElem.type() == Array) {
            BSONObjBuilder builder;
            builder.appendArray(fieldName,
                                updateMinOrMax(elem.Obj(), minOrMaxElem.Obj(), metaField, comp));
            minOrMax = minOrMax.addField(builder.obj().firstElement());
            continue;
        }

        if (minOrMaxElem.eoo() || comp(elem.woCompare(minOrMaxElem, false), 0)) {
            fieldsToUpdate.insert(fieldName);
        }
    }

    if (!fieldsToUpdate.empty()) {
        minOrMax = minOrMax.addFields(doc, fieldsToUpdate);
    }

    return std::move(minOrMax);
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
    auto key = std::make_pair(ns, BucketMetadata{metadata.obj()});

    auto time = doc[options.getTimeField()].Date();
    auto setBucketTime = [time = durationCount<Seconds>(time.toDurationSinceEpoch())](
                             OID* bucketId) { bucketId->setTimestamp(time); };

    auto it = _bucketIds.find(key);
    if (it == _bucketIds.end()) {
        // A bucket for this namespace and metadata pair does not yet exist.
        it = _bucketIds.insert({std::move(key), OID::gen()}).first;
        setBucketTime(&it->second);
        _orderedBuckets.insert({ns, it->first.second, it->second});
    }

    _idleBuckets.erase(it->second);
    auto bucket = &_buckets[it->second];

    StringSet newFieldNamesToBeInserted;
    uint32_t sizeToBeAdded = 0;
    for (const auto& elem : doc) {
        if (elem.fieldNameStringData() == options.getMetaField()) {
            // Ignore the metadata field since it will not be inserted.
            continue;
        }

        // If the field name is new, add the size of an empty object with that field name.
        if (!bucket->fieldNames.contains(elem.fieldName())) {
            newFieldNamesToBeInserted.insert(elem.fieldName());
            sizeToBeAdded += BSON(elem.fieldName() << BSONObj()).objsize();
        }

        // Add the element size, taking into account that the name will be changed to its positional
        // number. Add 1 to the calculation since the element's field name size accounts for a null
        // terminator whereas the stringified position does not.
        sizeToBeAdded +=
            elem.size() - elem.fieldNameSize() + std::to_string(bucket->numMeasurements).size() + 1;
    }

    auto bucketTime = it->second.asDateT();
    if (bucket->numMeasurements == kTimeseriesBucketMaxCount ||
        bucket->size + sizeToBeAdded > kTimeseriesBucketMaxSizeBytes ||
        time - bucketTime >= kTimeseriesBucketMaxTimeRange || time < bucketTime) {
        // The bucket is full, so create a new one.
        bucket->full = true;
        it->second = OID::gen();
        setBucketTime(&it->second);
        _orderedBuckets.insert({ns, it->first.second, it->second});
        bucket = &_buckets[it->second];
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
    }
    bucket->min =
        updateMinOrMax(doc, std::move(bucket->min), options.getMetaField(), std::less<>());
    bucket->max =
        updateMinOrMax(doc, std::move(bucket->max), options.getMetaField(), std::greater<>());

    // If there is exactly 1 uncommitted measurement, the caller is the committer. Otherwise, it is
    // a waiter.
    boost::optional<Future<CommitInfo>> commitInfoFuture;
    if (bucket->numMeasurements - bucket->numCommittedMeasurements > 1) {
        auto [promise, future] = makePromiseFuture<CommitInfo>();
        bucket->promises[bucket->numMeasurements - 1] = std::move(promise);
        commitInfoFuture = std::move(future);
    }

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

    // Inform waiters that their measurements have been committed.
    for (uint16_t i = 0; i < bucket.numPendingCommitMeasurements; i++) {
        auto it = bucket.promises.find(i + bucket.numCommittedMeasurements);
        if (it != bucket.promises.end()) {
            it->second.emplaceValue(*previousCommitInfo);
            bucket.promises.erase(it);
        }
    }

    bucket.numWriters -= bucket.numPendingCommitMeasurements;
    bucket.numCommittedMeasurements +=
        std::exchange(bucket.numPendingCommitMeasurements, measurements.size());

    auto allCommitted = measurements.empty();
    CommitData data = {std::move(measurements),
                       bucket.min,
                       bucket.max,
                       bucket.numCommittedMeasurements,
                       std::move(newFieldNamesToBeInserted)};

    if (allCommitted) {
        if (bucket.full) {
            // Everything in the bucket has been committed, and nothing more will be added since the
            // bucket is full. Thus, we can remove it.
            _orderedBuckets.erase(
                {std::move(it->second.ns), std::move(it->second.metadata), bucketId});
            _buckets.erase(it);
        } else if (--bucket.numWriters == 0) {
            _idleBuckets.insert(bucketId);
        }
    }

    return data;
}

void BucketCatalog::clear(const NamespaceString& ns) {
    stdx::lock_guard lk(_mutex);

    auto shouldClear = [&ns](const NamespaceString& bucketNs) {
        return ns.coll().empty() ? ns.db() == bucketNs.db() : ns == bucketNs;
    };

    for (auto it = _orderedBuckets.lower_bound({ns, {}, {}});
         it != _orderedBuckets.end() && shouldClear(std::get<NamespaceString>(*it));) {
        auto& bucketId = std::get<OID>(*it);
        _buckets.erase(bucketId);
        _idleBuckets.erase(bucketId);
        _bucketIds.erase({std::get<NamespaceString>(*it), std::get<BucketMetadata>(*it)});
        it = _orderedBuckets.erase(it);
    }
}

void BucketCatalog::clear(StringData dbName) {
    clear(NamespaceString(dbName, ""));
}

bool BucketCatalog::BucketMetadata::operator<(const BucketMetadata& other) const {
    auto size = metadata.objsize();
    auto otherSize = other.metadata.objsize();
    auto cmp = std::memcmp(metadata.objdata(), other.metadata.objdata(), std::min(size, otherSize));
    return cmp == 0 && size != otherSize ? size < otherSize : cmp < 0;
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return UnorderedFieldsBSONObjComparator().compare(metadata, other.metadata) == 0;
}
}  // namespace mongo
