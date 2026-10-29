// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo {

class ChangeStreamShardTargeterMock : public ChangeStreamShardTargeter {
public:
    using TimestampOrDocument = std::variant<Timestamp, Document>;
    using ReaderContextCallback =
        std::function<void(TimestampOrDocument, ChangeStreamReaderContext&)>;

    struct Response {
        TimestampOrDocument tsOrDocument;
        ShardTargeterDecision decision;
        boost::optional<Timestamp> endTs;
        ReaderContextCallback callback;
    };

    ShardTargeterDecision initialize(OperationContext* opCtx,
                                     Timestamp atClusterTime,
                                     ChangeStreamReaderContext& context) override {
        Response response = popResponse(atClusterTime);

        // Execute callback so that ChangeStreamReaderContext can make any modifications.
        if (response.callback) {
            response.callback(atClusterTime, context);
        }

        return response.decision;
    }

    std::pair<ShardTargeterDecision, boost::optional<Timestamp>> startChangeStreamSegment(
        OperationContext* opCtx,
        Timestamp atClusterTime,
        ChangeStreamReaderContext& context) override {
        Response response = popResponse(atClusterTime);

        // Execute callback so that ChangeStreamReaderContext can make any modifications.
        if (response.callback) {
            response.callback(atClusterTime, context);
        }

        return std::make_pair(response.decision, response.endTs);
    }

    ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                      const Document& event,
                                      ChangeStreamReaderContext& context) override {
        Response response = popResponse(Document{});

        // Execute callback so that ChangeStreamReaderContext can make any modifications.
        if (response.callback) {
            response.callback(event, context);
        }

        return response.decision;
    }

    bool empty() const {
        return _responses.empty();
    }

    /**
     * Queue the provided responses in the mock.
     */
    template <typename T>
    requires(std::is_same_v<typename T::value_type, Response>)
    void bufferResponses(const T& responses) {
        std::copy(responses.begin(), responses.end(), std::back_inserter(_responses));
    }

private:
    Response popResponse(TimestampOrDocument expected) {
        tassert(10767900, "queue should not be empty", !empty());

        auto response = _responses.front();
        if (std::holds_alternative<Timestamp>(expected)) {
            tassert(10767901,
                    "buffered response should contain a timestamp",
                    std::holds_alternative<Timestamp>(response.tsOrDocument));
            tassert(10767902,
                    "cluster time should be equal to expected cluster time",
                    std::get<Timestamp>(response.tsOrDocument) == std::get<Timestamp>(expected));
        } else {
            tassert(10767903,
                    "buffered response should contain a document",
                    std::holds_alternative<Document>(response.tsOrDocument));
        }
        _responses.pop_front();
        return response;
    }

    std::deque<Response> _responses;
};

}  // namespace mongo
