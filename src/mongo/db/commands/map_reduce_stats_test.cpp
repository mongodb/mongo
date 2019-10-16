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

#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/map_reduce_stats.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

using namespace std::literals::string_literals;

namespace mongo {
namespace {

class MapReduceStatsTest : public mongo::unittest::Test {
public:
    MapReduceStatsTest() = default;

    std::vector<CommonStats> buildMapReducePipelineStats() const {
        std::vector<CommonStats> pipelineStats;

        size_t docCount = 100;
        long long execTimeMs = 0;

        pipelineStats.push_back(_buildStatsForStage("$cursor", docCount, execTimeMs));

        execTimeMs += 5;
        pipelineStats.push_back(_buildStatsForStage("$project", docCount, execTimeMs));

        docCount *= 2;
        execTimeMs += 5;
        pipelineStats.push_back(_buildStatsForStage("$unwind", docCount, execTimeMs));

        docCount -= 20;
        execTimeMs += 5;
        pipelineStats.push_back(_buildStatsForStage("$group", docCount, execTimeMs));

        if (_addFinalize) {
            execTimeMs += 5;
            pipelineStats.push_back(_buildStatsForStage("$project", docCount, execTimeMs));
        }

        if (_addOut) {
            execTimeMs += 5;
            pipelineStats.push_back(_buildStatsForStage("$out", docCount, execTimeMs));
        }

        if (_addMerge) {
            execTimeMs += 5;
            pipelineStats.push_back(_buildStatsForStage("$merge", docCount, execTimeMs));
        }

        if (_addInvalidStageName) {
            execTimeMs += 5;
            pipelineStats.push_back(_buildStatsForStage("$changeStreams", docCount, execTimeMs));
        }

        return pipelineStats;
    }

    void addFinalizeProject() {
        _addFinalize = true;
    }
    void addOut() {
        _addOut = true;
    }
    void addMerge() {
        _addMerge = true;
    }
    void addInvalidStageForMapReduce() {
        _addInvalidStageName = true;
    }

private:
    CommonStats _buildStatsForStage(const char* name, size_t count, long long timeMillis) const {
        CommonStats stageStats(name);
        stageStats.advanced = count;
        stageStats.executionTimeMillis = timeMillis;
        return stageStats;
    }

    bool _addFinalize = false;
    bool _addOut = false;
    bool _addMerge = false;
    bool _addInvalidStageName = false;
};


TEST_F(MapReduceStatsTest, ConfirmStatsUnsharded) {
    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  false,  // Not verbose
                                  99);    // Total time

    BSONObjBuilder objBuilder;
    mapReduceStats.appendStats(&objBuilder);
    ASSERT_BSONOBJ_EQ(fromjson("{timeMillis: 99,"
                               "counts: {input: 100, emit: 200, output: 180}}"),
                      objBuilder.obj());
}


TEST_F(MapReduceStatsTest, ConfirmStatsUnshardedVerbose) {
    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  true,  // Verbose
                                  99);   // Total time

    BSONObjBuilder objBuilder;
    mapReduceStats.appendStats(&objBuilder);
    ASSERT_BSONOBJ_EQ(fromjson("{timeMillis: 99,"
                               "timing: { mapTime: 5, reduceTime: 5, total: 99 },"
                               "counts: {input: 100, emit: 200, output: 180}}"),
                      objBuilder.obj());
}

TEST_F(MapReduceStatsTest, ConfirmStatsUnshardedWithOutStage) {
    addOut();

    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  false,  // Not verbose
                                  99);    // Total time

    BSONObjBuilder objBuilder;
    mapReduceStats.appendStats(&objBuilder);
    ASSERT_BSONOBJ_EQ(fromjson("{timeMillis: 99,"
                               "counts: {input: 100, emit: 200, output: 180}}"),
                      objBuilder.obj());
}

TEST_F(MapReduceStatsTest, ConfirmStatsUnshardedWithMergeStage) {
    addMerge();
    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  false,  // Not verbose
                                  99);    // Total time

    BSONObjBuilder objBuilder;
    mapReduceStats.appendStats(&objBuilder);
    ASSERT_BSONOBJ_EQ(fromjson("{timeMillis: 99,"
                               "counts: {input: 100, emit: 200, output: 180}}"),
                      objBuilder.obj());
}

TEST_F(MapReduceStatsTest, ConfirmStatsUnshardedWithFinalizeProjectStage) {
    addFinalizeProject();
    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  false,  // Not verbose
                                  99);    // Total time

    BSONObjBuilder objBuilder;
    mapReduceStats.appendStats(&objBuilder);
    ASSERT_BSONOBJ_EQ(fromjson("{timeMillis: 99,"
                               "counts: {input: 100, emit: 200, output: 180}}"),
                      objBuilder.obj());
}

DEATH_TEST_F(MapReduceStatsTest,
             DeathByUnknownStage,
             "Invariant failure stageName == \"$out\"_sd || stageName == \"$merge\"_sd") {
    addInvalidStageForMapReduce();
    MapReduceStats mapReduceStats(buildMapReducePipelineStats(),
                                  MapReduceStats::ResponseType::kUnsharded,
                                  false,  // Not verbose
                                  99);    // Total time
}

}  // namespace
}  // namespace mongo
