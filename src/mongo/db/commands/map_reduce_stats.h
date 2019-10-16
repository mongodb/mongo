/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/plan_stats.h"

namespace mongo {

/**
 * Responsible for building the set of statistics required for a MapReduce command response and
 * appending to a response message.
 */
class MapReduceStats {
public:
    /**
     * The mapReduce command response format differs when run against a sharded vs unsharded
     * collection.
     */
    enum class ResponseType {
        kUnsharded,
        kSharded,
    };

    MapReduceStats(const std::vector<CommonStats>& stageStatistics,
                   ResponseType responseType,
                   bool verbose,
                   int executionTime);

    void appendStats(BSONObjBuilder* resultBuilder) const;

    /**
     * Creates a dummy MapReduceStats instance for use in unit testing.
     */
    static MapReduceStats createForTest() {
        return MapReduceStats();
    }

private:
    struct StageCounts {
        size_t input = 0;
        size_t emit = 0;
        size_t output = 0;
    } _counts;

    struct StageTiming {
        long long map = 0;
        long long reduce = 0;
    } _timing;

    MapReduceStats() = default;

    void appendCounts(BSONObjBuilder* resultBuilder) const;

    void appendTiming(BSONObjBuilder* resultBuilder) const;

    void appendShardCounts(BSONObjBuilder* resultBuilder) const;

    void appendPostProcessCounts(BSONObjBuilder* resultBuilder) const;

    const ResponseType _responseType = ResponseType::kUnsharded;
    const bool _verbose = false;
    const int _executionTime = 0;
};

}  // namespace mongo
