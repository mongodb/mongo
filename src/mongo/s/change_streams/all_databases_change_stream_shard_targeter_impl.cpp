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

#include "mongo/s/change_streams/all_databases_change_stream_shard_targeter_impl.h"

#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/all_databases_change_stream_state_event_handler.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

// Prefix that will be added to all log messages emitted by this class.
#define STAGE_LOG_PREFIX "AllDatabasesChangeStreamShardTargeterImpl: "

namespace mongo {

ShardTargeterDecision AllDatabasesChangeStreamShardTargeterImpl::initialize(
    OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& context) {
    tassert(11138101,
            "initialize() can only be called in strict mode",
            context.getChangeStream().getReadMode() == ChangeStreamReadMode::kStrict);

    auto placement = _fetcher->fetch(opCtx,
                                     context.getChangeStream().getNamespace(),
                                     atClusterTime,
                                     false /* checkIfPointInTimeIsInFuture */,
                                     false /* ignoreRemovedShardsMode */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }
    tassert(11138102,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);

    const auto& shards = placement.getShards();

    LOGV2_DEBUG(11138104,
                3,
                STAGE_LOG_PREFIX "Initializing change stream in strict mode",
                "changeStream"_attr = context.getChangeStream(),
                "atClusterTime"_attr = atClusterTime,
                "shards"_attr = shards);

    if (!shards.empty()) {
        // Determine which cursors to open. The cursors to open here are registered in the
        // 'readerContext' instance.
        // Open cursors on all shards.
        stdx::unordered_set<ShardId> activeShardSet(shards.begin(), shards.end());
        context.openCursorsOnDataShards(atClusterTime, activeShardSet);
    }
    // Always open cursor on config server in case shards get added or removed.
    context.openCursorOnConfigServer(atClusterTime);

    return ShardTargeterDecision::kContinue;
}

ShardTargeterDecision AllDatabasesChangeStreamShardTargeterImpl::handleEvent(
    OperationContext* opCtx, const Document& event, ChangeStreamReaderContext& readerContext) {
    tassert(11138105,
            "AllDatabasesChangeStreamShardTargeterImpl::_eventHandler must be present for handling "
            "control events",
            _eventHandler);
    LOGV2_DEBUG(
        11138106, 3, STAGE_LOG_PREFIX "Handling event", "controlEvent"_attr = event.toString());

    auto controlEvent = parseControlEvent(event);

    return readerContext.inDegradedMode()
        ? _eventHandler->handleEventInDegradedMode(opCtx, controlEvent, *this, readerContext)
        : _eventHandler->handleEvent(opCtx, controlEvent, *this, readerContext);
}

std::pair<ShardTargeterDecision, boost::optional<Timestamp>>
AllDatabasesChangeStreamShardTargeterImpl::startChangeStreamSegment(
    OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& context) {
    tassert(11138107,
            "startChangeStreamSegment() can only be called in ignoreRemovedShards mode",
            context.getChangeStream().getReadMode() == ChangeStreamReadMode::kIgnoreRemovedShards);

    auto placement = _fetcher->fetch(opCtx,
                                     context.getChangeStream().getNamespace(),
                                     atClusterTime,
                                     false /* checkIfPointInTimeIsInFuture */,
                                     true /* ignoreRemovedShardsMode */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return std::make_pair(ShardTargeterDecision::kSwitchToV1,
                              boost::optional<Timestamp>(boost::none));
    }

    // Validate status of historical placement result.
    change_streams::assertHistoricalPlacementStatusOK(placement);

    // Determine 'openCursorAt' value.
    const Timestamp openCursorAt = placement.getOpenCursorAt().value_or(atClusterTime);

    // Determine 'nextPlacementChangedAt' value.
    const boost::optional<Timestamp> nextPlacementChangedAt = placement.getNextPlacementChangedAt();

    const auto& shards = placement.getShards();

    LOGV2_DEBUG(11138108,
                3,
                STAGE_LOG_PREFIX "Starting change stream segment in ignoreRemovedShards mode",
                "changeStream"_attr = context.getChangeStream(),
                "atClusterTime"_attr = atClusterTime,
                "anyRemovedShardDetected"_attr =
                    placement.getAnyRemovedShardDetected().value_or(false),
                "openCursorAt"_attr = openCursorAt,
                "nextPlacementChangedAt"_attr = nextPlacementChangedAt,
                "currentActiveShards"_attr = context.getCurrentlyTargetedDataShards(),
                "shards"_attr = shards);

    tassert(11138109,
            str::stream() << "The 'openCursorAt' value (" << openCursorAt
                          << ") must be greater or equal to the 'atClusterTime' value ("
                          << atClusterTime << ")",
            openCursorAt >= atClusterTime);

    tassert(11138110,
            str::stream() << "The 'nextPlacementChangedAt' value (" << *nextPlacementChangedAt
                          << ") must be greater than the 'openCursorAt' value (" << openCursorAt
                          << ")",
            !nextPlacementChangedAt.has_value() || *nextPlacementChangedAt > openCursorAt);

    tassert(11138111,
            "should not have 'nextPlacementChangedAt' value if no shards are present",
            !nextPlacementChangedAt.has_value() || !shards.empty());

    // Determine the cursors to open and close. The cursor open/close requests are registered in the
    // 'readerContext' instance.
    auto adjustCursors = [&]() {
        change_streams::updateActiveShardCursors(openCursorAt, shards, context);

        // Open cursor on config server to watch for creation of further databases.
        if (!nextPlacementChangedAt.has_value()) {
            context.openCursorOnConfigServer(openCursorAt);
        } else {
            if (context.isCursorOnConfigServerOpen()) {
                context.closeCursorOnConfigServer();
            }
        }
    };

    adjustCursors();

    // If 'nextPlacementChangedAt' is set, the state machine in
    // 'ChangeStreamHandleTopologyChangeV2Stage' will enter the degraded fetching state.
    // Otherwise it will enter the normal fetching state.
    return std::make_pair(ShardTargeterDecision::kContinue, nextPlacementChangedAt);
}

HistoricalPlacementFetcher&
AllDatabasesChangeStreamShardTargeterImpl::getHistoricalPlacementFetcher() const {
    return *_fetcher;
}

void AllDatabasesChangeStreamShardTargeterImpl::setEventHandler(
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) {
    tassert(11138112, "ChangeStreamShardTargeterStateEventHandler must be provided", eventHandler);
    LOGV2_DEBUG(11138113,
                3,
                "Setting event handler",
                "previousEventHandler"_attr = _eventHandler ? _eventHandler->toString() : "none",
                "newEventHandler"_attr = eventHandler->toString());
    _eventHandler = std::move(eventHandler);
}
}  // namespace mongo
