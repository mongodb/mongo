/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <map>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {

using namespace mongo;

//
// Null comparison and existence tests.
//

TEST_F(QueryPlannerTest, ExistsTrueQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsFalseQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, EqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NotEqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NotEqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null, $exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, EqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null, $exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerTest, EmptyBoundsWithAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lte: 5, $gte: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}
}
