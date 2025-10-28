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

#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"

#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ShardTargeterDecision CollectionChangeStreamShardTargeterImpl::initialize(
    OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& readerCtx) {
    auto placement = _fetcher->fetch(opCtx,
                                     readerCtx.getChangeStream().getNamespace(),
                                     atClusterTime,
                                     false /* checkIfPointInTimeIsInFuture */,
                                     false /* ignoreRemovedShards */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }
    tassert(10720100,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);

    if (placement.getShards().empty()) {
        readerCtx.openCursorOnConfigServer(atClusterTime);
        setEventHandler(
            std::make_unique<CollectionChangeStreamShardTargeterDbAbsentStateEventHandler>());
    } else {
        stdx::unordered_set<ShardId> activeShardSet(placement.getShards().begin(),
                                                    placement.getShards().end());
        readerCtx.openCursorsOnDataShards(atClusterTime, activeShardSet);
        setEventHandler(
            std::make_unique<CollectionChangeStreamShardTargeterDbPresentStateEventHandler>());
    }

    return ShardTargeterDecision::kContinue;
}

ShardTargeterDecision CollectionChangeStreamShardTargeterImpl::handleEvent(
    OperationContext* opCtx, const Document& event, ChangeStreamReaderContext& readerContext) {
    tassert(10720101,
            "CollectionChangeStreamShardTargeterImpl::_eventHandler must be present for handling "
            "control events",
            _eventHandler);

    auto controlEvent = parseControlEvent(event);
    return readerContext.inDegradedMode()
        ? _eventHandler->handleEventInDegradedMode(opCtx, controlEvent, *this, readerContext)
        : _eventHandler->handleEvent(opCtx, controlEvent, *this, readerContext);
}

std::pair<ShardTargeterDecision, boost::optional<Timestamp>>
CollectionChangeStreamShardTargeterImpl::startChangeStreamSegment(
    OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& context) {
    MONGO_UNIMPLEMENTED_TASSERT(10783902);
}

HistoricalPlacementFetcher& CollectionChangeStreamShardTargeterImpl::getHistoricalPlacementFetcher()
    const {
    return *_fetcher;
}

ChangeStreamShardTargeterStateEventHandler*
CollectionChangeStreamShardTargeterImpl::getEventHandler() const {
    return _eventHandler.get();
}

void CollectionChangeStreamShardTargeterImpl::setEventHandler(
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) {
    _eventHandler = std::move(eventHandler);
}

}  // namespace mongo
