/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/query/planner_analysis.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    TEST(QueryPlannerAnalysis, GetSortPatternBasic) {
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1}")));
        ASSERT_EQUALS(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1}")));
        ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1}")));
        ASSERT_EQUALS(fromjson("{a: 1, b: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: -1}")));
        ASSERT_EQUALS(fromjson("{a: -1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 1}")));
        ASSERT_EQUALS(fromjson("{a: -1, b: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: -1}")));
    }

    TEST(QueryPlannerAnalysis, GetSortPatternOtherElements) {
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 0}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 100}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: Infinity}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: true}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: false}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: []}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: {}}")));

        ASSERT_EQUALS(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -100}")));
        ASSERT_EQUALS(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -Infinity}")));

        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{}")));
    }

    TEST(QueryPlannerAnalysis, GetSortPatternSpecialIndexTypes) {
        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'hashed'}")));
        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'text'}")));
        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: '2dsphere'}")));
        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: ''}")));
        ASSERT_EQUALS(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'foo'}")));

        ASSERT_EQUALS(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 'text'}")));
        ASSERT_EQUALS(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: '2dsphere'}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text'}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere'}")));

        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text', c: 1}")));
        ASSERT_EQUALS(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere',"
                                                                    " c: 1}")));

        ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text'}")));
        ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text',"
                                                                    " d: 1}")));
    }

}  // namespace
