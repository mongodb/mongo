// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {
class ChangeStreamShardTargeterStateEventHandlingContextMock
    : public ChangeStreamShardTargeterStateEventHandlingContext {
public:
    explicit ChangeStreamShardTargeterStateEventHandlingContextMock(
        std::unique_ptr<HistoricalPlacementFetcherMock> fetcher)
        : _fetcher(std::move(fetcher)) {}

    HistoricalPlacementFetcher& getHistoricalPlacementFetcher() const override {
        return *_fetcher;
    }

    void setEventHandler(
        std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) override {
        setHandlerCalls.push_back(std::move(eventHandler));
    }

    ChangeStreamShardTargeterStateEventHandler* lastSetEventHandler() const {
        return setHandlerCalls.empty() ? nullptr : setHandlerCalls.back().get();
    }

    void reset() {
        setHandlerCalls.clear();
        _fetcher = nullptr;
    }

    std::vector<std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>> setHandlerCalls;

private:
    std::unique_ptr<HistoricalPlacementFetcherMock> _fetcher;
};

class ChangeStreamShardTargeterEventHandlerMock
    : public ChangeStreamShardTargeterStateEventHandler {
public:
    struct Call {
        bool degradedMode;
        ControlEvent controlEvent;

        bool operator==(const Call& other) const = default;
    };

    explicit ChangeStreamShardTargeterEventHandlerMock(ShardTargeterDecision normal,
                                                       ShardTargeterDecision degraded)
        : normalDecision(normal), degradedDecision(degraded) {}

    ChangeStreamShardTargeterStateEventHandler::DbPresenceState getDbPresenceState()
        const override {
        // The mock does not know about absence/presence of the database.
        return ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kUnknown;
    }

    ShardTargeterDecision handleEvent(OperationContext*,
                                      const ControlEvent& e,
                                      ChangeStreamShardTargeterStateEventHandlingContext&,
                                      ChangeStreamReaderContext&) override {
        calls.emplace_back(false, e);
        ++normalCallCount;
        return normalDecision;
    }

    ShardTargeterDecision handleEventInDegradedMode(
        OperationContext*,
        const ControlEvent& e,
        ChangeStreamShardTargeterStateEventHandlingContext&,
        ChangeStreamReaderContext&) override {
        calls.emplace_back(true, e);
        ++degradedCallCount;
        return degradedDecision;
    }

    std::string toString() const override {
        return "ChangeStreamShardTargeterEventHandlerMock";
    }

    std::vector<Call> calls;
    size_t normalCallCount = 0;
    size_t degradedCallCount = 0;
    ShardTargeterDecision normalDecision;
    ShardTargeterDecision degradedDecision;
};
}  // namespace mongo
