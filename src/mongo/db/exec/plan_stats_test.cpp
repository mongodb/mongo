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

/**
 * Basic test on field initializers
 */
TEST(CommonStatsTest, defaultValues) {
    CommonStats stats;
    ASSERT_EQUALS(stats.works, static_cast<size_t>(0));
    ASSERT_EQUALS(stats.yields, static_cast<size_t>(0));
    ASSERT_EQUALS(stats.invalidates, static_cast<size_t>(0));
    ASSERT_EQUALS(stats.advanced, static_cast<size_t>(0));
    ASSERT_EQUALS(stats.needTime, static_cast<size_t>(0));
    ASSERT_EQUALS(stats.needYield, static_cast<size_t>(0));
    ASSERT_FALSE(stats.isEOF);
}

/**
 * Verifies null argument check in CommonStats::writeExplainTo
 */
TEST(CommonStatsTest, writeExplainToNullBuilder) {
    CommonStats stats;
    stats.writeExplainTo(NULL);
}

/**
 * Verifies null argument check in PlanStageStats::writeExplainTo
 */
TEST(PlanStageStatsTest, writeExplainToNullBuilder) {
    CommonStats stats;
    PlanStageStats pss(stats);
    pss.writeExplainTo(NULL);
}

/**
 * Checks BSON output of CommonStats::writeExplainTo to ensure it contains
 * correct values for CommonStats fields
 */
TEST(CommonStatsTest, writeExplainTo) {
    CommonStats stats;
    stats.works = static_cast<size_t>(2);
    stats.advanced = static_cast<size_t>(3);
    BSONObjBuilder bob;
    stats.writeExplainTo(&bob);
    BSONObj obj = bob.done();
    ASSERT_TRUE(obj.hasField("works"));
    ASSERT_EQUALS(obj.getIntField("works"), 2);
    ASSERT_TRUE(obj.hasField("advanced"));
    ASSERT_EQUALS(obj.getIntField("advanced"), 3);
}

/**
 * Checks BSON output of PlanStageStats::writeExplainTo to ensure it contains
 * correct values for CommonStats fields
 */
TEST(PlanStageStatsTest, writeExplainTo) {
    CommonStats stats;
    stats.works = static_cast<size_t>(2);
    stats.advanced = static_cast<size_t>(3);
    BSONObjBuilder bob;
    PlanStageStats pss(stats);
    pss.writeExplainTo(&bob);
    BSONObj obj = bob.done();
    ASSERT_TRUE(obj.hasField("works"));
    ASSERT_EQUALS(obj.getIntField("works"), 2);
    ASSERT_TRUE(obj.hasField("advanced"));
    ASSERT_EQUALS(obj.getIntField("advanced"), 3);
}

}  // namespace
