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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"

#include <memory>

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
    virtual StringData getName() const = 0;
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
        const AsyncResultsMergerParams& params, bool recognizeControlEvents);
};

}  // namespace mongo
