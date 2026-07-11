// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

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

    std::string_view getName() const override {
        return "invalid"sv;
    }
};

/**
 * Strategy to determine the next high watermark in V1 change streams.
 */
class NextHighWaterMarkDeterminingStrategyChangeStreamV1 final
    : public NextHighWaterMarkDeterminingStrategy {
public:
    explicit NextHighWaterMarkDeterminingStrategyChangeStreamV1(bool compareWholeSortKey)
        : _compareWholeSortKey(compareWholeSortKey) {}

    BSONObj operator()(const BSONObj& nextEvent, const BSONObj& currentHighWaterMark) override {
        return AsyncResultsMerger::extractSortKey(nextEvent, _compareWholeSortKey).getOwned();
    }

    std::string_view getName() const override {
        return "changeStreamV1"sv;
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
    explicit NextHighWaterMarkDeterminingStrategyRecognizeControlEvents(bool compareWholeSortKey)
        : _compareWholeSortKey(compareWholeSortKey) {}

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
                nextResumeToken.firstElement().type() == BSONType::object);
        const auto nextEventClusterTime =
            ResumeToken::parse(nextResumeToken.firstElement().Obj()).getData().clusterTime;
        tassert(10359103,
                "Expected existing high water mark to be an object",
                currentHighWaterMark.firstElement().type() == BSONType::object);

        const auto currentHighWaterMarkClusterTime =
            ResumeToken::parse(currentHighWaterMark.firstElement().Obj()).getData().clusterTime;
        if (currentHighWaterMarkClusterTime < nextEventClusterTime) {
            return BSON(
                "" << ResumeToken::makeHighWaterMarkToken(nextEventClusterTime, 2 /* version */)
                          .toBSON());
        }
        return currentHighWaterMark;
    }

    std::string_view getName() const override {
        return "recognizeControlEvents"sv;
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
NextHighWaterMarkDeterminingStrategyFactory::createForChangeStream(bool compareWholeSortKey,
                                                                   bool recognizeControlEvents) {
    if (recognizeControlEvents) {
        return std::make_unique<NextHighWaterMarkDeterminingStrategyRecognizeControlEvents>(
            compareWholeSortKey);
    }

    // Handler for v1 change streams.
    return std::make_unique<NextHighWaterMarkDeterminingStrategyChangeStreamV1>(
        compareWholeSortKey);
}

}  // namespace mongo
