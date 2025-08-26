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

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/util/assert_util.h"

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
