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

/**
 * This file contains tests for mongo/db/query/plan_ranker.h
 */

#include "mongo/db/query/plan_ranker.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

using std::make_unique;
using std::string;
using std::unique_ptr;
using std::vector;

unique_ptr<PlanStageStats> makeStats(const char* name,
                                     StageType type,
                                     unique_ptr<SpecificStats> specific) {
    auto stats = make_unique<PlanStageStats>(name, type);
    stats->common.works = 1;
    stats->specific = std::move(specific);
    return stats;
}

TEST(PlanRankerTest, NoFetchBonus) {
    // Two plans: one does a fetch, one does not. Assert the plan without the fetch has a higher
    // score. Note there is no projection involved: before SERVER-39241 was fixed we would give
    // these two plans the same score.

    auto goodPlan =
        makeStats("SHARDING_FILTER", STAGE_SHARDING_FILTER, make_unique<ShardingFilterStats>());
    goodPlan->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>()));

    auto badPlan =
        makeStats("SHARDING_FILTER", STAGE_SHARDING_FILTER, make_unique<ShardingFilterStats>());
    badPlan->children.emplace_back(makeStats("FETCH", STAGE_FETCH, make_unique<IndexScanStats>()));
    badPlan->children[0]->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>()));

    auto scorer = plan_ranker::makePlanScorer();
    auto goodScore = scorer->calculateScore(goodPlan.get());
    auto badScore = scorer->calculateScore(badPlan.get());

    ASSERT_GT(goodScore, badScore);
}

};  // namespace
