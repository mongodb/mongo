/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_util.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/search/internal_search_cluster_parameters_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
namespace executor {

MongotTaskExecutorCursorGetMoreStrategy::MongotTaskExecutorCursorGetMoreStrategy(
    boost::optional<long long> startingBatchSize,
    DocsNeededBounds docsNeededBounds,
    boost::optional<TenantId> tenantId,
    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        searchIdLookupMetrics)
    : _currentBatchSize(startingBatchSize),
      _docsNeededBounds(docsNeededBounds),
      _batchSizeHistory({}),
      _tenantId(tenantId),
      _searchIdLookupMetrics(searchIdLookupMetrics) {
    if (startingBatchSize.has_value()) {
        _batchSizeHistory.emplace_back(*startingBatchSize);
    }
}

BSONObj MongotTaskExecutorCursorGetMoreStrategy::createGetMoreRequest(
    const CursorId& cursorId,
    const NamespaceString& nss,
    long long prevBatchNumReceived,
    long long totalNumReceived) {
    GetMoreCommandRequest getMoreRequest(cursorId, std::string{nss.coll()});

    boost::optional<long long> docsNeeded = _getNextDocsRequested(totalNumReceived);

    // TODO SERVER-92576 Remove docsRequested.
    if (_currentBatchSize.has_value() || docsNeeded.has_value()) {
        BSONObjBuilder getMoreBob;
        getMoreRequest.serialize(&getMoreBob);
        BSONObjBuilder cursorOptionsBob(getMoreBob.subobjStart(mongot_cursor::kCursorOptionsField));
        if (_currentBatchSize.has_value()) {
            cursorOptionsBob.append(mongot_cursor::kBatchSizeField,
                                    _getNextBatchSize(prevBatchNumReceived));
        } else {
            cursorOptionsBob.append(mongot_cursor::kDocsRequestedField, docsNeeded.get());
        }
        cursorOptionsBob.doneFast();
        return getMoreBob.obj();
    }
    return getMoreRequest.toBSON();
}

long long MongotTaskExecutorCursorGetMoreStrategy::_getNextBatchSize(
    long long prevBatchNumReceived) {
    tassert(8953002,
            "_getNextBatchSize() should only be called when using the batchSize field.",
            _currentBatchSize.has_value());

    auto internalSearchOptions =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<InternalSearchOptions>>("internalSearchOptions")
            ->getValue(_tenantId);

    // The maximum size we will allow the batch to grow to in a single growth step.
    // In case the growth factor is small enough that the next batchSize rounds back to the
    // previous batchSize, we use std::ceil to make sure it always grows by at least 1,
    // unless the growth factor is exactly 1.
    long long maxNextBatchSize =
        std::ceil(*_currentBatchSize * internalSearchOptions.getBatchSizeGrowthFactor());

    auto extractableLimit = docs_needed_bounds::calcExtractableLimit(_docsNeededBounds);
    if ((_searchIdLookupMetrics && extractableLimit.has_value()) &&
        (_searchIdLookupMetrics->getDocsReturnedByIdLookup() < extractableLimit.value())) {
        // This case, where we need to request a GetMore from mongot for a non-stored source query
        // with an extractable limit, is fairly unlikely. We knew the exact # of docs we wanted from
        // mongot upfront, but our searchidLookupMetrics indicated not enough of them were found on
        // mongod to satisfy the query. In this next batch, we use the number of docs we still need,
        // and the success rate of docs previously found to infer a best guess of the least number
        // of additional docs we need from mongot to complete the search query.
        if (_searchIdLookupMetrics->getDocsReturnedByIdLookup() == 0) {
            // Very unlikely case where no docs at all were found on mongod. We need to handle this
            // so we don't divide by zero.
            _currentBatchSize = maxNextBatchSize;
        } else {
            long long nDocsStillNeeded =
                extractableLimit.value() - _searchIdLookupMetrics->getDocsReturnedByIdLookup();
            long long possibleBatchSize =
                (nDocsStillNeeded / _searchIdLookupMetrics->getIdLookupSuccessRate()) *
                internalSearchOptions.getOversubscriptionFactor();
            _currentBatchSize = std::min(possibleBatchSize, maxNextBatchSize);
        }
    }
    // Don't increase batchSize if the previous batch was returned without filling to the
    // requested batchSize. That indicates the BSON limit was reached, which likely could happen
    // again.
    else if (prevBatchNumReceived == *_currentBatchSize) {
        _currentBatchSize = maxNextBatchSize;
    }
    _batchSizeHistory.emplace_back(*_currentBatchSize);
    return *_currentBatchSize;
}

boost::optional<long long> MongotTaskExecutorCursorGetMoreStrategy::_getNextDocsRequested(
    long long totalNumReceived) {
    auto extractableLimit = docs_needed_bounds::calcExtractableLimit(_docsNeededBounds);
    if (!extractableLimit.has_value()) {
        return boost::none;
    }

    if (_searchIdLookupMetrics) {
        // The return value will start at _mongotDocsRequested and will decrease by one
        // for each document that gets returned by the $idLookup stage. If a document gets
        // filtered out, docsReturnedByIdLookup will not change and so docsNeeded will stay the
        // same.
        return extractableLimit.get() - _searchIdLookupMetrics->getDocsReturnedByIdLookup();
    } else {
        // In the stored source case, the return value will start at _mongotDocsRequested and
        // will decrease by one for each document returned by this stage.
        return extractableLimit.get() - totalNumReceived;
    }
}

bool MongotTaskExecutorCursorGetMoreStrategy::_mustNeedAnotherBatch(
    long long totalNumReceived) const {
    return visit(
        OverloadedVisitor{
            [totalNumReceived](long long minBounds) { return (totalNumReceived < minBounds); },
            [](docs_needed_bounds::NeedAll minBounds) { return true; },
            [](docs_needed_bounds::Unknown minBounds) { return false; },
        },
        _docsNeededBounds.getMinBounds());
}

bool MongotTaskExecutorCursorGetMoreStrategy::shouldPrefetch(long long totalNumReceived,
                                                             long long numBatchesReceived) const {
    // If we aren't sending batchSize to mongot, then we prefetch the next batch, unless we have a
    // discrete maximum bounds used to set docsRequested. When docsRequested is set, we
    // optimistically assume that we will only need a single batch and attempt to avoid doing
    // unnecessary work on mongot. If $idLookup filters out enough documents such that we are not
    // able to satisfy the limit, then we will fetch the next batch syncronously on the subsequent
    // 'getNext()' call.
    if (!_currentBatchSize.has_value()) {
        return !std::holds_alternative<long long>(_docsNeededBounds.getMaxBounds());
    }

    if (_mustNeedAnotherBatch(totalNumReceived)) {
        return true;
    }
    return std::holds_alternative<docs_needed_bounds::Unknown>(_docsNeededBounds.getMaxBounds()) &&
        numBatchesReceived >= kAlwaysPrefetchAfterNBatches;
}

}  // namespace executor
}  // namespace mongo
