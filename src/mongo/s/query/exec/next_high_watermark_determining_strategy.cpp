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

#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

/**
 * High watermarks are only useful in tailable, awaitData mode. In all other modes, the function
 * to determine the next high watermark should never be called by the 'AsyncResultsMerger'.
 * We use the following simple implementation that always tasserts to ensure that.
 */
class NextHighWaterMarkDeterminingStrategyInvalid final
    : public NextHighWaterMarkDeterminingStrategy {
public:
    BSONObj operator()(const BSONObj& nextEvent, const BSONObj& currentHighWaterMark) override {
        tasserted(10359107, "cannot determine high watermark in this configuration");
    }

    StringData getName() const override {
        return "invalid"_sd;
    }
};

/**
 * Strategy to determine the next high watermark in V1 change streams.
 */
class NextHighWaterMarkDeterminingStrategyChangeStreamV1 final
    : public NextHighWaterMarkDeterminingStrategy {
public:
    explicit NextHighWaterMarkDeterminingStrategyChangeStreamV1(
        const AsyncResultsMergerParams& params)
        : _compareWholeSortKey(params.getCompareWholeSortKey()) {}

    BSONObj operator()(const BSONObj& nextEvent, const BSONObj& currentHighWaterMark) override {
        return AsyncResultsMerger::extractSortKey(nextEvent, _compareWholeSortKey).getOwned();
    }

    StringData getName() const override {
        return "changeStreamV1"_sd;
    }

private:
    const bool _compareWholeSortKey;
};

/**
 * Strategy to determine the next high watermark in V2 change streams, recognizing control events.
 */
class NextHighWaterMarkDeterminingStrategyRecognizeControlEvents final
    : public NextHighWaterMarkDeterminingStrategy {
public:
    explicit NextHighWaterMarkDeterminingStrategyRecognizeControlEvents(
        const AsyncResultsMergerParams& params)
        : _compareWholeSortKey(params.getCompareWholeSortKey()) {}

    BSONObj operator()(const BSONObj& nextEvent, const BSONObj& currentHighWaterMark) override {
        tassert(10359109,
                "Current high water mark should never be empty",
                !currentHighWaterMark.isEmpty());

        BSONObj nextResumeToken =
            AsyncResultsMerger::extractSortKey(nextEvent, _compareWholeSortKey).getOwned();

        if (!nextEvent.hasField(Document::metaFieldChangeStreamControlEvent)) {
            return nextResumeToken;
        }

        tassert(10359102,
                "Expected sortKey to be an object",
                nextResumeToken.firstElement().type() == BSONType::Object);
        const auto nextEventClusterTime =
            ResumeToken::parse(nextResumeToken.firstElement().Obj()).getData().clusterTime;
        tassert(10359103,
                "Expected existing high water mark to be an object",
                currentHighWaterMark.firstElement().type() == BSONType::Object);

        const auto currentHighWaterMarkClusterTime =
            ResumeToken::parse(currentHighWaterMark.firstElement().Obj()).getData().clusterTime;
        if (currentHighWaterMarkClusterTime < nextEventClusterTime) {
            return BSON(
                "" << ResumeToken::makeHighWaterMarkToken(nextEventClusterTime, 2 /* version */)
                          .toBSON());
        }
        return currentHighWaterMark;
    }

    StringData getName() const override {
        return "recognizeControlEvents"_sd;
    }

private:
    const bool _compareWholeSortKey;
};

}  // namespace


NextHighWaterMarkDeterminingStrategy::~NextHighWaterMarkDeterminingStrategy() = default;

NextHighWaterMarkDeterminingStrategyPtr
NextHighWaterMarkDeterminingStrategyFactory::createInvalidHighWaterMarkDeterminingStrategy() {
    return std::make_unique<NextHighWaterMarkDeterminingStrategyInvalid>();
}

NextHighWaterMarkDeterminingStrategyPtr
NextHighWaterMarkDeterminingStrategyFactory::createForChangeStream(
    const AsyncResultsMergerParams& params, bool recognizeControlEvents) {
    if (recognizeControlEvents) {
        return std::make_unique<NextHighWaterMarkDeterminingStrategyRecognizeControlEvents>(params);
    }

    // Handler for v1 change streams.
    return std::make_unique<NextHighWaterMarkDeterminingStrategyChangeStreamV1>(params);
}

}  // namespace mongo
