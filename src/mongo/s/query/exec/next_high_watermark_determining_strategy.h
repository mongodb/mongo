// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface for strategies that the AsyncResultsMerger can use in tailable, awaitData mode to
 * determine the next high watermark.
 */
class NextHighWaterMarkDeterminingStrategy {
public:
    virtual ~NextHighWaterMarkDeterminingStrategy();

    /**
     * This function is called by the 'AsyncResultsMerger' to determine the next high water
     * mark. The event at the top of the merge queue needs to be provided via 'nextEvent', the last
     * determined high watermark will be provided via 'currentHighWaterMark'.
     The 'AsyncResultsMerger' is supposed to update its high water mark to the returned value.
     */
    virtual BSONObj operator()(const BSONObj& nextEvent, const BSONObj& currentHighWaterMark) = 0;

    /**
     * Return the name of the strategy.
     */
    virtual std::string_view getName() const = 0;
};

using NextHighWaterMarkDeterminingStrategyPtr =
    std::unique_ptr<NextHighWaterMarkDeterminingStrategy>;

/**
 * Static factory methods for creating NextHighWaterMarkDeterminingStrategy objects.
 */
struct NextHighWaterMarkDeterminingStrategyFactory {
    /**
     * Build a functor that will always throw.
     * This is used for all modes other than tailable, awaitData, in which we do not expect the
     * 'AsyncResultsMerger' to call the function into the functor to determine the next high
     * watermark at all.
     */
    static NextHighWaterMarkDeterminingStrategyPtr createInvalidHighWaterMarkDeterminingStrategy();

    /**
     * Build a functor, to be used by the 'AsyncResultsMerger', that determines the next high
     * watermark. The correct functor will be created based on the function arguments.
     */
    static NextHighWaterMarkDeterminingStrategyPtr createForChangeStream(
        bool compareWholeSortKey, bool recognizeControlEvents);
};

}  // namespace mongo
