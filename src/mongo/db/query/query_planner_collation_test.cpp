/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace {

using namespace mongo;

TEST_F(QueryPlannerTest, StringComparisonWithNullCollatorOnIndexResultsInCollscan) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithNullCollatorOnQueryResultsInCollscan) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(fromjson("{a: 1}"), &collator);

    runQuery(fromjson("{a: {$lt: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithUnequalCollatorsResultsInCollscan) {
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(fromjson("{a: 1}"), &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithMatchingCollationUsesIndexWithTransformedBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, collation: {locale: 'reverse'}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null, "
        "bounds: {a: [['', 'oof', true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, StringComparisonAndNonStringComparisonCanUseSeparateIndices) {
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);

    addIndex(fromjson("{a: 1}"), &reverseStringCollator);
    addIndex(fromjson("{b: 1}"), &alwaysEqualCollator);

    // The string predicate can use index {a: 1}, since the collators match. The non-string
    // comparison can use index {b: 1}, even though the collators don't match.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}, b: {$lte: 4}}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$lte: 4}}, collation: {locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, "
        "filter: null, bounds: {a: [['', 'oof', true, false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}, node: {ixscan: "
        "{pattern: {b: 1}, filter: null, "
        "bounds: {b: [[-Infinity, 4, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, StringEqWrtCollatorCannotBeCovered) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 'string'}, projection: {_id: 0, a: 1}, collation: "
                 "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [['gnirts', 'gnirts', true, "
        "true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, StringGteWrtCollatorCannotBeCovered) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gte: 'string'}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [['gnirts', {}, true, "
        "false]]}}}}}}}");
}

TEST_F(QueryPlannerTest, InContainingStringCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$in: [2, 'foo']}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [[2,2,true,true],"
        "['oof','oof',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, TypeStringCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$type: 'string'}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: {a:{$type:'string'}}, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['',{},true,false]]}}}}}}}");
}

TEST_F(QueryPlannerTest, NotWithStringBoundsCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$ne: 2}}, projection: {_id: 0, a: 1}, collation: "
                 "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey',2,true,false], [2,'MaxKey',false,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, ExistsTrueCannotBeCoveredWithSparseIndexAndCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);
    params.mainCollectionInfo.indexes.back().sparse = true;

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$exists: true}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, MinMaxWithStringBoundsCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', min: {a: 1, b: 2}, max: {a: 2, b: 1}, "
        "projection: {_id: 0, a: 1, b: 1}, hint: {a: 1, b: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, PrefixOnlyRegexCannotUseAnIndexWithACollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: /^simple/}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, PrefixOnlyRegexCanUseAnIndexWithoutACollatorWithTightBounds) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: /^simple/}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1}, filter: null, bounds: "
        "{a: [['simple', 'simplf', true, false], [/^simple/, /^simple/, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexCanUseAnIndexWithoutACollatorAsInexactCovered) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: /nonsimple/}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1}, filter: {a: /nonsimple/}, bounds: "
        "{a: [['', {}, true, false], [/nonsimple/, /nonsimple/, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, AccessPlannerCorrectlyCombinesComparisonKeyBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gte: 'foo', $lte: 'zfoo'}, b: 'bar'}, collation: {locale: "
        "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, collation: {locale: 'reverse'}, "
        "node: {ixscan: {pattern: {a: 1, b: "
        "1}, filter: null, bounds: {a: [['oof','oofz',true,true]], b: "
        "[['rab','rab',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, OrQueryResultsInCollscanWhenOnlyOneBranchHasIndexWithMatchingCollation) {
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);

    addIndex(fromjson("{a: 1}"), &reverseStringCollator);
    addIndex(fromjson("{b: 1}"), &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 'foo'}, {b: 'bar'}]}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, OrQueryCanBeIndexedWhenBothBranchesHaveIndexWithMatchingCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);
    addIndex(fromjson("{b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 'foo'}, {b: 'bar'}]}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [['oof','oof',true,true]]}}},"
        "{ixscan: {pattern: {b: 1}, bounds: {b: [['rab','rab',true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchObjectResultsInCorrectComparisonKeyBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{'a.b': 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$elemMatch: {b: {$gte: 'foo', $lte: 'zfoo'}}}}, collation: "
        "{locale: "
        "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:'foo',$lte:'zfoo'}}}}, collation: {locale: "
        "'reverse'}, node: {ixscan: {pattern: "
        "{'a.b': 1}, filter: null, bounds: {'a.b': [['oof','oofz',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, QueryForNestedObjectWithMatchingCollatorCanUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {b: 1}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, QueryForNestedObjectWithNonMatchingCollatorCantUseIndexWithCollator) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQuery(fromjson("{a: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, CannotUseIndexWithNonMatchingCollatorForSort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {b: 1}, sort: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, type: 'simple', limit: 0,"
        "node: {cscan: {dir: 1, filter: {b: 1}}}}}");
}

TEST_F(QueryPlannerTest, CanUseIndexWithMatchingCollatorForSort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {b: 1}, sort: {a: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, type: 'simple', limit: 0,"
        "node: {cscan: {dir: 1, filter: {b: 1}, collation: {locale: 'reverse'}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b: 1}, collation: {locale: 'reverse'}, node: {ixscan: {pattern: {a: "
        "1}}}}}");
}

TEST_F(QueryPlannerTest, IndexWithNonMatchingCollatorCausesInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {'$exists': true}}, sort: {a: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node:"
        "{fetch: {node : {ixscan: {pattern: {a: 1}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {a: {'$exists': true}}}}}}");
}

TEST_F(QueryPlannerTest, IndexWithMatchingCollatorDoesNotCauseInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {'$exists': true}}, sort: {a: 1},"
                 "collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node : {ixscan: {pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {a: {'$exists': true}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexWithNonMatchingCollatorCausesInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1, c: 1, d: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 2, c: {a: 1}},"
                 "sort: {a: 1, b: 1, c: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1, c: 1}, limit: 0, type: 'simple', node:"
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1, c: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {a: 1, b: 2, c: {a: 1}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexWithNonMatchingPrefixedCollatorDoesNotCauseInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1, c: 1, d: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 2, c: {a: 1 } },"
                 "sort: {a: 1, b: 1 }}"));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node : {ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter : {a: 1, b: 2, c: {a: 1}}}}}}");
}

TEST_F(QueryPlannerTest, SuccessfullyPlanWhenMinMaxHaveNumberBoundariesAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1, c: 1}, max: {a: 3, b: 3, c: 3},"
                 "hint: {a:1, b: 1, c: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}");
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasStringBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 'foo', c: 1}, "
                 "hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasStringBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', max: {a: 1, b: 'foo', c: 1}, hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasObjectBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: {d: 'foo'}, c: 1}, "
                 "hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasObjectBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', max: {a: 1, b: {d: 'foo'}, c: 1}, "
                 "hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasArrayBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1, c: [1, 'foo']}, "
                 "hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasArrayBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(
        fromjson("{find: 'testns', max: {a: 1, b: 1, c: [1, 'foo']}, "
                 "hint: {a: 1, b: 1, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenHintingIndexIncompatibleWithMinDueToCollation) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "indexToHint"_sd);
    addIndex(fromjson("{a: 1}"));
    runInvalidQueryAsCommand(fromjson("{find: 'testns', min: {a: 'foo'}, hint: 'indexToHint'}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenHintingIndexIncompatibleWithMaxDueToCollation) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "indexToHint"_sd);
    addIndex(fromjson("{a: 1}"));
    runInvalidQueryAsCommand(fromjson("{find: 'testns', max: {a: 'foo'}, hint: 'indexToHint'}"));
}

TEST_F(QueryPlannerTest, SuccessWithIndexWithMatchingSimpleCollationWhenMinHasStringBoundary) {
    addIndex(fromjson("{a: 1}"), nullptr, "noCollation"_sd);

    runQueryAsCommand(fromjson("{find: 'testns', min: {a: 'foo'}, hint: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'noCollation'}}}}");
}

TEST_F(QueryPlannerTest, SuccessWithIndexWithMatchingNonSimpleCollationWhenMinHasStringBoundary) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "withCollation"_sd);

    runQueryAsCommand(fromjson(
        "{find: 'testns', min: {a: 'foo'}, hint: {a: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'withCollation'}}}}");
}

TEST_F(QueryPlannerTest, SuccessWithIndexWithMatchingSimpleCollationWhenMaxHasStringBoundary) {
    addIndex(fromjson("{a: 1}"), nullptr, "noCollation"_sd);

    runQueryAsCommand(fromjson("{find: 'testns', max: {a: 'foo'}, hint: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'noCollation'}}}}");
}

TEST_F(QueryPlannerTest, MustSortInMemoryWhenMinMaxQueryHasCollationAndIndexDoesNot) {
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1}, max: {a: 2, b: 1}, hint: {a:1, b:1}, "
                 "collation: {locale: 'reverse'}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);

    assertSolutionExists(
        "{fetch: {node: {sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'default', node: {ixscan: "
        "{pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustSortInMemoryWhenMinMaxIndexHasCollationAndQueryDoesNot) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b:1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1}, max: {a: 2, b: 1}, sort: {a: 1, b: 1}, "
                 "hint: {a: 1, b: 1}}"));

    assertNumSolutions(1U);

    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node:"
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CanProduceCoveredSortPlanWhenQueryHasCollationButIndexDoesNot) {
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(fromjson(
        "{find: 'testns', projection: {a: 1, b: 1, _id: 0}, min: {a: 1, b: 1}, max: {a: 2, "
        "b: 1}, hint: {a: 1, b: 1}, collation: {locale: 'reverse'}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'default', node: "
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CannotUseIndexWhenQueryHasNoCollationButIndexHasNonSimpleCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', projection: {a: 1, b:1, _id: 0}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, CannotUseIndexWhenQueryHasDifferentNonSimpleCollationThanIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', projection: {a: 1, b:1, _id: 0}, collation: {locale: "
                 "'reverse'}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {cscan: {dir: 1}}}}}}");
}

// This test verifies that an in-memory sort stage is added and sort provided by an index is not
// used when the collection has a compound index with a non-simple collation and we issue a
// non-collatable point-query on the prefix of the index key together with a sort on a suffix of the
// index key. This is a test for SERVER-48993.
TEST_F(QueryPlannerTest,
       MustSortInMemoryWhenPointPrefixQueryHasSimpleCollationButIndexHasNonSimpleCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    // No explicit collation on the query. This will implicitly use the simple collation since the
    // collection does not have a default collation.
    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 2}, sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");

    // A query with an explicit simple collation.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 2}, sort: {b: 1}, collation: {locale: 'simple'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

// This test verifies that an in-memory sort stage is added and sort provided by an index is not
// used when the collection has a compound index with a non-specified collation and we issue a
// non-collatable point-query on the prefix of the index key together with a sort on the suffix and
// an explicit non-simple collation. This is a test for SERVER-48993.
TEST_F(QueryPlannerTest,
       MustSortInMemoryWhenPointPrefixQueryHasNonSimpleCollationButIndexHasSimpleCollation) {
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 2}, sort: {b: 1}, collation: {locale: "
                 "'reverse'}}"));

    assertSolutionExists(
        "{fetch: {node: "
        "{sort: {pattern: {b: 1}, limit: 0, type: 'default', node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

// This test verifies that an in-memory sort stage is added and sort provided by indexes is not used
// when the collection has a compound index with a non-simple collation and we issue a
// non-collatable point-query on the prefix of the index key together with a sort on the suffix and
// an explicit non-simple collation that differs from the index collation. This is a test for
// SERVER-48993.
TEST_F(QueryPlannerTest, MustSortInMemoryWhenPointPrefixQueryCollationDoesNotMatchIndexCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 2}, sort: {b: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

// This test verifies that a sort is provided by an index when the collection has a compound index
// with a non-simple collation and we issue a query with a different non-simple collation is a
// non-collatable point-query on the prefix, a non-collatable range-query on the suffix, and a sort
// on the suffix key. This is a test for SERVER-48993.
TEST_F(QueryPlannerTest,
       IndexCanSortWhenPointPrefixQueryCollationDoesNotMatchIndexButSortRangeIsNonCollatable) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 2, b: {$gte: 0, $lt: 10}}, sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, "
        "false]]}}}}}");
}

// This test verifies that an in-memory sort stage is added when the collection has a compound index
// with a non-simple collation and we issue a query with a different non-simple collation is a
// non-collatable point-query on the prefix, a collatable range-query on the suffix, and a sort on
// the suffix key. This is a test for SERVER-48993.
TEST_F(QueryPlannerTest,
       MustSortInMemoryWhenPointPrefixQueryCollationDoesNotMatchIndexAndSortRangeIsCollatable) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 2, b: {$gte: 'B', $lt: 'T'}}, sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[2, 2, true, true]]}}}}}}}");
}

// This test verifies that a SORT_MERGE stage is added when the collection has a compound index with
// a non-simple collation and we issue a query with a different non-simple collation is a
// non-collatable multi-point query on the prefix, a non-collatable range-query on the suffix, and a
// sort on the suffix key.This is a test for SERVER-48993.
TEST_F(QueryPlannerTest,
       CanExplodeForSortWhenPointPrefixQueryCollationDoesNotMatchIndexButSortRangeIsNonCollatable) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$in: [2, 5]}, b: {$gte: 0, $lt: 10}}, sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {node: "
        "{mergeSort: {nodes: {"
        "n0: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[2, 2, true, true]], b: [[0, 10, "
        "true, false]]}}}, "
        "n1: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[5, 5, true, true]], b: [[0, 10, "
        "true, false]]}}} "
        "}}}}}");
}

/**
 * This test confirms that we place a fetch stage before sort in the case where both query
 * and index have the same non-simple collation. To handle this scenario without this fetch would
 * require a mechanism to ensure we don't attempt to encode for collation an already encoded index
 * key entry when generating the sort key.
 */
TEST_F(QueryPlannerTest, MustFetchBeforeSortWhenQueryHasSameNonSimpleCollationAsIndex) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, _id: 0}, "
                 "collation: {locale: 'reverse'}, sort: {b: 1, a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1, a: 1}, limit: 0, type: 'simple', node: "
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node:"
        "{fetch: {filter: null, collation: {locale: 'reverse'}, node:"
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, NoSortStageWhenMinMaxIndexCollationDoesNotMatchButBoundsContainNoStrings) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 8, c: 1}, max: {a: 1, b: 8, c: 100}, "
                 "hint: {a: 1, b: 1, c: 1}, collation: {locale: 'reverse'}, "
                 "sort: {a: 1, b: 1, c: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}");
}

TEST_F(QueryPlannerTest, NoFilterWhenMinMaxRecordSufficientAndCollationSame) {
    // Test that a clustered collection scan can drop elements from the filter if the underlying
    // scan can apply equivalent filtering using min/max record id.
    params.clusteredInfo = mongo::clustered_util::makeDefaultClusteredIdIndex();

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {_id: {$gte: 'x', $lt: 'z'}}, hint: {_id: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {}}}");
}

TEST_F(QueryPlannerTest, PartialFilterCollationSame) {
    // Test that a clustered collection scan can drop elements from the filter if the underlying
    // scan can apply equivalent filtering using min/max record id, but will retain any
    // other filter terms which _cannot_ be so represented as limits on the record id.
    params.clusteredInfo = mongo::clustered_util::makeDefaultClusteredIdIndex();

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$and: [ { _id: { $gte: 'x' } }, { foo: { $lt: 'z' } } "
                 "]}, hint: {_id: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: { $and: [ { foo: { $lt: 'z' } } ] }}}");
}

TEST_F(QueryPlannerTest, FilterWhenMinMaxRecordNotSufficientWhenCollationIsNotSame) {
    // Test that a clustered collection scan will NOT drop elements from the filter
    // if collation differs between the clustered collection and the query.
    params.clusteredInfo = mongo::clustered_util::makeDefaultClusteredIdIndex();

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {_id: {$gte: 'x', $lt: 'z'}}, hint: {_id: 1}, "
                 "collation: {locale: 'lt'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: { $and: [ { _id: { $gte: 'x' } }, { _id: { $lt: 'z' } } ] }, "
        "collation: {locale: 'lt'}}}");
}

TEST_F(QueryPlannerTest, FilterWhenCollationIsNotSameEvenForUnaffected) {
    // Test that a clustered collection scan will NOT drop elements from the filter
    // if collation differs between the clustered collection and the query, _even_ if
    // the parameters to the query are not affected by collation (e.g., numbers).

    // For a single specific query, the filter _could_ be simplified, but it would not
    // be correct to allow that query to be cached, as a later reuse may use
    // values which _are_ affected.
    params.clusteredInfo = mongo::clustered_util::makeDefaultClusteredIdIndex();

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {_id: {$gte: 1, $lt: 2}}, hint: {_id: 1}, "
                 "collation: {locale: 'lt'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: { $and: [ { _id: { $gte: 1 } }, { _id: { $lt: 2 } } ] }, "
        "collation: {locale: 'lt'}}}");
}

}  // namespace
