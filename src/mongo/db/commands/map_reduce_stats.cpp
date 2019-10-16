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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/map_reduce_stats.h"

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

MapReduceStats::MapReduceStats(const std::vector<CommonStats>& pipelineStats,
                               ResponseType responseType,
                               bool verbose,
                               int executionTime)
    : _responseType(responseType), _verbose(verbose), _executionTime(executionTime) {

    long long prevTime = 0;
    bool projectSeen = false;
    for (const auto& stageStats : pipelineStats) {
        const auto stageName = stageStats.stageTypeStr;

        // While a mapReduce pipeline may include a $match, $sort and/or $limit stage they will be
        // absorbed by the $cursor stage push down to the query sub-system. The exception to this is
        // a $match with empty query predicate, which will not be pushed down.
        if (stageName == "$cursor"_sd || stageName == "$match"_sd) {
            _counts.input = stageStats.advanced;
        } else if (stageName == "$project"_sd) {
            if (!projectSeen) {
                _timing.map = stageStats.executionTimeMillis - prevTime;
                projectSeen = true;
            }
        } else if (stageName == "$unwind"_sd) {
            _counts.emit = stageStats.advanced;
        } else if (stageName == "$group"_sd) {
            _timing.reduce = stageStats.executionTimeMillis - prevTime;
            _counts.output = stageStats.advanced;
        } else {
            invariant(stageName == "$out"_sd || stageName == "$merge"_sd, stageName);
        }

        prevTime = stageStats.executionTimeMillis;
    }
}

void MapReduceStats::appendStats(BSONObjBuilder* resultBuilder) const {
    *resultBuilder << "timeMillis" << _executionTime;

    if (_verbose) {
        appendTiming(resultBuilder);
    }

    appendCounts(resultBuilder);

    if (_responseType == ResponseType::kSharded) {
        appendShardCounts(resultBuilder);
        appendPostProcessCounts(resultBuilder);
    }
}

void MapReduceStats::appendCounts(BSONObjBuilder* resultBuilder) const {
    BSONObjBuilder countBuilder = resultBuilder->subobjStart("counts");
    countBuilder.appendNumber("input", _counts.input);
    countBuilder.appendNumber("emit", _counts.emit);
    countBuilder.appendNumber("output", _counts.output);
    countBuilder.doneFast();
}

void MapReduceStats::appendTiming(BSONObjBuilder* resultBuilder) const {
    BSONObjBuilder timingBuilder = resultBuilder->subobjStart("timing");
    if (_responseType == ResponseType::kUnsharded) {
        timingBuilder.appendIntOrLL("mapTime", _timing.map);
        timingBuilder.appendIntOrLL("reduceTime", _timing.reduce);
        timingBuilder.appendIntOrLL("total", _executionTime);
    } else {
        invariant(_responseType == ResponseType::kSharded);
        // TODO SERVER-43290: Add support for sharded collection statistics.
        timingBuilder << "shardProcessing" << 0;
        timingBuilder << "postProcessing" << 0;
    }
    timingBuilder.doneFast();
}

void MapReduceStats::appendShardCounts(BSONObjBuilder* resultBuilder) const {
    BSONObjBuilder shardCountsBuilder = resultBuilder->subobjStart("shardCounts");

    // TODO SERVER-43290: Add support for sharded collection statistics.
    BSONObjBuilder dummyShardBuilder = shardCountsBuilder.subobjStart("shardName-rs0/host:port");
    dummyShardBuilder << "input" << 0;
    dummyShardBuilder << "emit" << 0;
    dummyShardBuilder << "output" << 0;
    dummyShardBuilder.doneFast();

    shardCountsBuilder.doneFast();
}

void MapReduceStats::appendPostProcessCounts(BSONObjBuilder* resultBuilder) const {
    BSONObjBuilder countsBuilder = resultBuilder->subobjStart("postProcessCounts");

    // TODO SERVER-43290: Add support for sharded collection statistics.
    BSONObjBuilder dummyMergeBuilder = countsBuilder.subobjStart("shardName-rs0/host:port");
    dummyMergeBuilder << "input" << 0;
    dummyMergeBuilder << "emit" << 0;
    dummyMergeBuilder << "output" << 0;
    dummyMergeBuilder.doneFast();

    countsBuilder.doneFast();
}

}  // namespace mongo
