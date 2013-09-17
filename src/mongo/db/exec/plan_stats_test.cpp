/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/exec/plan_stats.h
 */

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    // basic test on field initializers
    TEST(CommonStatsTest, defaultValues) {
        CommonStats stats;
        ASSERT_EQUALS(stats.works, static_cast<uint64_t>(0));
        ASSERT_EQUALS(stats.yields, static_cast<uint64_t>(0));
        ASSERT_EQUALS(stats.invalidates, static_cast<uint64_t>(0));
        ASSERT_EQUALS(stats.advanced, static_cast<uint64_t>(0));
        ASSERT_EQUALS(stats.needTime, static_cast<uint64_t>(0));
        ASSERT_EQUALS(stats.needFetch, static_cast<uint64_t>(0));
        ASSERT_FALSE(stats.isEOF);
    }

    // shared code for writeExplainTo test cases between CommonStats and PlanStageStats
    // because we are using same test data and base stats object
    struct CommonStatsWriteExplainTo { void operator()(BSONObjBuilder * bob, const CommonStats & stats) { stats.writeExplainTo(bob); } };
    struct PlanStageStatsWriteExplainTo { void operator()(BSONObjBuilder * bob, const CommonStats & stats) {
        PlanStageStats pss(stats);
        pss.writeExplainTo(bob);
    } };

    // attempting to write to null bson builder should result in no-op
    template <typename F> void _testWriteExplainToNullBuilder(F f) {
        CommonStats stats;
        f(NULL, stats);
    }

    TEST(CommonStatsTest, writeExplainToNullBuilder) {
        _testWriteExplainToNullBuilder(CommonStatsWriteExplainTo());
    }

    TEST(PlanStageStatsTest, writeExplainToNullBuilder) {
        _testWriteExplainToNullBuilder(PlanStageStatsWriteExplainTo());
    }

    template <typename F> void _testWriteExplainToSimple(F f) {
        CommonStats stats;
        stats.works = static_cast<uint64_t>(2);
        stats.advanced = static_cast<uint64_t>(3);
        BSONObjBuilder bob;
        f(&bob, stats);
        BSONObj obj = bob.done();
        ASSERT_TRUE(obj.hasField("works"));
        ASSERT_EQUALS(obj.getIntField("works"), 2);
        ASSERT_TRUE(obj.hasField("advanced"));
        ASSERT_EQUALS(obj.getIntField("advanced"), 3);
    }

    TEST(CommonStatsTest, writeExplainTo) {
        _testWriteExplainToSimple(CommonStatsWriteExplainTo());
    }

    TEST(PlanStageStatsTest, writeExplainTo) {
        _testWriteExplainToSimple(PlanStageStatsWriteExplainTo());
    }

}  // namespace
