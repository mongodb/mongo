/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/internal_unpack_bucket_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalUnpackBucketToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto dsInternalUnpackBucket =
        boost::dynamic_pointer_cast<const DocumentSourceInternalUnpackBucket>(documentSource);

    tassert(10565500, "expected 'DocumentSourceInternalUnpackBucket' type", dsInternalUnpackBucket);

    return make_intrusive<exec::agg::InternalUnpackBucketStage>(
        dsInternalUnpackBucket->kStageNameInternal,
        dsInternalUnpackBucket->getExpCtx(),
        dsInternalUnpackBucket->_sharedState,
        dsInternalUnpackBucket->_eventFilterDeps,
        dsInternalUnpackBucket->_unpackToBson,
        dsInternalUnpackBucket->_sampleSize);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(_internalUnpackBucket,
                           DocumentSourceInternalUnpackBucket::id,
                           documentSourceInternalUnpackBucketToStageFn);

InternalUnpackBucketStage::InternalUnpackBucketStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const std::shared_ptr<InternalUnpackBucketSharedState>& sharedState,
    DepsTracker depsTracker,
    const bool unpackToBson,
    const boost::optional<long long> sampleSize)
    : Stage(stageName, pExpCtx),
      _eventFilterDeps(std::move(depsTracker)),
      _sharedState(sharedState),
      _unpackToBson(unpackToBson),
      _sampleSize(sampleSize) {}

GetNextResult InternalUnpackBucketStage::doGetNext() {
    tassert(5521502, "calling doGetNext() when '_sampleSize' is set is disallowed", !_sampleSize);

    // Otherwise, fallback to unpacking every measurement in all buckets until the child stage is
    // exhausted.
    if (auto measure = getNextMatchingMeasure()) {
        return GetNextResult(std::move(*measure));
    }

    auto nextResult = pSource->getNext();
    while (nextResult.isAdvanced()) {
        auto bucket = nextResult.getDocument().toBson();
        auto bucketMatchedQuery = _sharedState->_wholeBucketFilter &&
            exec::matcher::matchesBSON(_sharedState->_wholeBucketFilter.get(), bucket);
        _sharedState->_bucketUnpacker.reset(std::move(bucket), bucketMatchedQuery);

        uassert(
            5346509,
            str::stream()
                << "A bucket with _id "
                << _sharedState->_bucketUnpacker.bucket()[timeseries::kBucketIdFieldName].toString()
                << " contains an empty data region",
            _sharedState->_bucketUnpacker.hasNext());
        if (auto measure = getNextMatchingMeasure()) {
            return GetNextResult(std::move(*measure));
        }
        nextResult = pSource->getNext();
    }

    return nextResult;
}

boost::optional<Document> InternalUnpackBucketStage::getNextMatchingMeasure() {
    while (_sharedState->_bucketUnpacker.hasNext()) {
        if (_sharedState->_eventFilter) {
            if (_unpackToBson) {
                auto measure = _sharedState->_bucketUnpacker.getNextBson();
                if (_sharedState->_bucketUnpacker.bucketMatchedQuery() ||
                    exec::matcher::matchesBSON(_sharedState->_eventFilter.get(), measure)) {
                    return Document(measure);
                }
            } else {
                auto measure = _sharedState->_bucketUnpacker.getNext();
                // MatchExpression only takes BSON documents, so we have to make one. As an
                // optimization, only serialize the fields we need to do the match.
                BSONObj measureBson = _eventFilterDeps.needWholeDocument
                    ? measure.toBson()
                    : document_path_support::documentToBsonWithPaths(measure,
                                                                     _eventFilterDeps.fields);
                if (_sharedState->_bucketUnpacker.bucketMatchedQuery() ||
                    exec::matcher::matchesBSON(_sharedState->_eventFilter.get(), measureBson)) {
                    return measure;
                }
            }
        } else {
            return _sharedState->_bucketUnpacker.getNext();
        }
    }
    return {};
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
