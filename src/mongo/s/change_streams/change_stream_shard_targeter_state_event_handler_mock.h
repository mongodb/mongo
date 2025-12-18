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

#pragma once

#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
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
