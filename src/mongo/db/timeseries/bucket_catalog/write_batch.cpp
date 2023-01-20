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

#include "mongo/db/timeseries/bucket_catalog/write_batch.h"

#include <boost/iterator/transform_iterator.hpp>

#include "mongo/db/timeseries/bucket_catalog/bucket.h"

namespace mongo::timeseries::bucket_catalog {

WriteBatch::WriteBatch(const BucketHandle& bucket,
                       OperationId opId,
                       ExecutionStatsController& stats)
    : _bucket{bucket}, _opId(opId), _stats(stats) {}

bool WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<CommitInfo> WriteBatch::getResult() {
    if (!_promise.getFuture().isReady()) {
        _stats.incNumWaits();
    }
    return _promise.getFuture().getNoThrow();
}

const BucketHandle& WriteBatch::bucket() const {
    return _bucket;
}

const WriteBatch::BatchMeasurements& WriteBatch::measurements() const {
    return _measurements;
}

const BSONObj& WriteBatch::min() const {
    return _min;
}

const BSONObj& WriteBatch::max() const {
    return _max;
}

const StringMap<std::size_t>& WriteBatch::newFieldNamesToBeInserted() const {
    return _newFieldNamesToBeInserted;
}

uint32_t WriteBatch::numPreviouslyCommittedMeasurements() const {
    return _numPreviouslyCommittedMeasurements;
}

bool WriteBatch::needToDecompressBucketBeforeInserting() const {
    return _decompressed.has_value();
}

const DecompressionResult& WriteBatch::decompressed() const {
    invariant(_decompressed.has_value());
    return _decompressed.value();
}

bool WriteBatch::finished() const {
    return _promise.getFuture().isReady();
}

BSONObj WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) { return nameHashPair.first; };
    return BSON("docs" << std::vector<BSONObj>(_measurements.begin(), _measurements.end())
                       << "bucketMin" << _min << "bucketMax" << _max << "numCommittedMeasurements"
                       << int(_numPreviouslyCommittedMeasurements) << "newFieldNamesToBeInserted"
                       << std::set<std::string>(
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.begin(),
                                                             toFieldName),
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.end(),
                                                             toFieldName)));
}

void WriteBatch::_addMeasurement(const BSONObj& doc) {
    _measurements.push_back(doc);
}

void WriteBatch::_recordNewFields(Bucket* bucket, NewFieldNames&& fields) {
    for (auto&& field : fields) {
        _newFieldNamesToBeInserted[field] = field.hash();
        bucket->_uncommittedFieldNames.emplace(field);
    }
}

void WriteBatch::_prepareCommit(Bucket* bucket) {
    invariant(_commitRights.load());
    _numPreviouslyCommittedMeasurements = bucket->_numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = _newFieldNamesToBeInserted.begin(); it != _newFieldNamesToBeInserted.end();) {
        StringMapHashedKey fieldName(it->first, it->second);
        bucket->_uncommittedFieldNames.erase(fieldName);
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

    if (bucket->_decompressed.has_value()) {
        _decompressed = std::move(bucket->_decompressed);
        bucket->_decompressed.reset();
        bucket->_memoryUsage -=
            (_decompressed.value().before.objsize() + _decompressed.value().after.objsize());
    }
}

void WriteBatch::_finish(const CommitInfo& info) {
    invariant(_commitRights.load());
    _promise.emplaceValue(info);
}

void WriteBatch::_abort(const Status& status) {
    if (finished()) {
        return;
    }

    _promise.setError(status);
}

}  // namespace mongo::timeseries::bucket_catalog
