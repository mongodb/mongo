/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/change_streams/change_stream_shard_targeter_base.h"

#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

// Prefix that will be added to all log messages emitted by this base class.
#define STAGE_LOG_PREFIX "ChangeStreamShardTargeterBase: "

namespace mongo {

HistoricalPlacementFetcher& ChangeStreamShardTargeterBase::getHistoricalPlacementFetcher() const {
    return *_fetcher;
}

void ChangeStreamShardTargeterBase::setEventHandler(
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) {
    tassert(11132501, "ChangeStreamShardTargeterStateEventHandler must be provided", eventHandler);
    LOGV2_DEBUG(11132502,
                3,
                STAGE_LOG_PREFIX "Setting event handler",
                "previousEventHandler"_attr = _eventHandler ? _eventHandler->toString() : "none",
                "newEventHandler"_attr = eventHandler->toString());
    _eventHandler = std::move(eventHandler);
}

ChangeStreamShardTargeterStateEventHandler& ChangeStreamShardTargeterBase::getEventHandler_forTest()
    const {
    invariant(_eventHandler);
    return *_eventHandler;
}

void ChangeStreamShardTargeterBase::createStateEventHandler(const std::vector<ShardId>& shards) {
    // Call derived class to create the state handler.
    if (shards.empty()) {
        setEventHandler(createDbAbsentHandler());
    } else {
        setEventHandler(createDbPresentHandler());
    }
}

ShardTargeterDecision ChangeStreamShardTargeterBase::initialize(
    OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& readerContext) {
    tassert(10922910,
            "initialize() can only be called in strict mode",
            readerContext.getChangeStream().getReadMode() == ChangeStreamReadMode::kStrict);

    auto placement = _fetcher->fetch(opCtx,
                                     readerContext.getChangeStream().getNamespace(),
                                     atClusterTime,
                                     false /* checkIfPointInTimeIsInFuture */,
                                     false /* ignoreRemovedShardsMode */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }
    tassert(10720100,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);

    const auto& shards = placement.getShards();

    LOGV2_DEBUG(11600500,
                3,
                STAGE_LOG_PREFIX "Initializing change stream in strict mode",
                "changeStream"_attr = readerContext.getChangeStream(),
                "atClusterTime"_attr = atClusterTime,
                "shards"_attr = shards);

    // Determine which cursors to open. The cursors to open here are registered in the
    // 'readerContext' instance.
    auto openCursors = [&]() {
        if (shards.empty()) {
            // Collection/database was not allocated to any shards at PIT 'atClusterTime'. This
            // means we need to wait for the database to be created. Open a cursor on the config
            // server, so we get notified about future "insert" events into the "config.databases"
            // namespaces that signals database creation.
            readerContext.openCursorOnConfigServer(atClusterTime);
        } else {
            // Collection/database is allocated to at least one shard, so we can open the cursors
            // for these shards without having to open a cursor on the config server.
            stdx::unordered_set<ShardId> activeShardSet(shards.begin(), shards.end());
            readerContext.openCursorsOnDataShards(atClusterTime, activeShardSet);
        }
    };

    openCursors();
    createStateEventHandler(shards);

    return ShardTargeterDecision::kContinue;
}

std::pair<ShardTargeterDecision, boost::optional<Timestamp>>
ChangeStreamShardTargeterBase::startChangeStreamSegment(OperationContext* opCtx,
                                                        Timestamp atClusterTime,
                                                        ChangeStreamReaderContext& readerContext) {
    tassert(10922911,
            "startChangeStreamSegment() can only be called in ignoreRemovedShards mode",
            readerContext.getChangeStream().getReadMode() ==
                ChangeStreamReadMode::kIgnoreRemovedShards);

    auto placement = _fetcher->fetch(opCtx,
                                     readerContext.getChangeStream().getNamespace(),
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

    LOGV2_DEBUG(10922905,
                3,
                STAGE_LOG_PREFIX "Starting change stream segment in ignoreRemovedShards mode",
                "changeStream"_attr = readerContext.getChangeStream(),
                "atClusterTime"_attr = atClusterTime,
                "anyRemovedShardDetected"_attr =
                    placement.getAnyRemovedShardDetected().value_or(false),
                "openCursorAt"_attr = openCursorAt,
                "nextPlacementChangedAt"_attr = nextPlacementChangedAt,
                "currentActiveShards"_attr = readerContext.getCurrentlyTargetedDataShards(),
                "shards"_attr = shards);

    tassert(10922901,
            str::stream() << "The 'openCursorAt' value (" << openCursorAt
                          << ") must be greater or equal to the 'atClusterTime' value ("
                          << atClusterTime << ")",
            openCursorAt >= atClusterTime);

    tassert(10922902,
            str::stream() << "The 'nextPlacementChangedAt' value (" << *nextPlacementChangedAt
                          << ") must be greater than the 'openCursorAt' value (" << openCursorAt
                          << ")",
            !nextPlacementChangedAt.has_value() || *nextPlacementChangedAt > openCursorAt);

    tassert(10922913,
            "should not have 'nextPlacementChangedAt' value if no shards are present",
            !nextPlacementChangedAt.has_value() || !shards.empty());

    // Determine the cursors to open and close. The cursor open/close requests are registered in the
    // 'readerContext' instance.
    auto adjustCursors = [&]() {
        change_streams::updateActiveShardCursors(openCursorAt, shards, readerContext);

        if (shards.empty()) {
            // Collection/database was not allocated to any shards at PIT 'atClusterTime'. This
            // means we need to wait for the database to be created. Open a cursor on the config
            // server, so we get notified about future "insert" events into the "config.databases"
            // namespaces that signals database creation.
            readerContext.openCursorOnConfigServer(openCursorAt);
        } else {
            // Collection/database is allocated to at least one shard, so we do not need the cursor
            // on the config server anymore (in case one was opened before).
            readerContext.closeCursorOnConfigServer();
        }
    };

    adjustCursors();
    createStateEventHandler(shards);

    // If 'nextPlacementChangedAt' is set, the state machine in
    // 'ChangeStreamHandleTopologyChangeV2Stage' will enter the degraded fetching state. Otherwise
    // it will enter the normal fetching state.
    return std::make_pair(ShardTargeterDecision::kContinue, nextPlacementChangedAt);
}

ShardTargeterDecision ChangeStreamShardTargeterBase::handleEvent(
    OperationContext* opCtx, const Document& event, ChangeStreamReaderContext& readerContext) {
    tassert(10720101,
            "ChangeStreamShardTargeterBase::_eventHandler must be present for handling "
            "control events",
            _eventHandler);
    LOGV2_DEBUG(
        11132500, 3, STAGE_LOG_PREFIX "Handling event", "controlEvent"_attr = event.toString());

    auto controlEvent = parseControlEvent(event);
    return readerContext.inDegradedMode()
        ? _eventHandler->handleEventInDegradedMode(opCtx, controlEvent, *this, readerContext)
        : _eventHandler->handleEvent(opCtx, controlEvent, *this, readerContext);
}

}  // namespace mongo
