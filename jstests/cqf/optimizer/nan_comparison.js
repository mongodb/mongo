/**
 * Tests comparisons against NaN.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {usedBonsaiOptimizer} from "jstests/libs/optimizer_utils.js";

const coll = db.nan_comparison;
coll.drop();

coll.insert({_id: 0, a: NaN});

function runFindAssertBonsai(filter, expectedResult) {
    const result = coll.find(filter).toArray();
    assertArrayEq({actual: result, expected: expectedResult});

    // Assert Bonsai was used.
    const explain =
        assert.commandWorked(db.runCommand({explain: {find: coll.getName(), filter: filter}}));
    assert(usedBonsaiOptimizer(explain), tojson(explain));
}

// None of the below filters should be satisfied for NaN.
runFindAssertBonsai({a: {$lt: 0}}, []);
runFindAssertBonsai({a: {$lte: 0}}, []);
runFindAssertBonsai({a: {$lt: -Infinity}}, []);
runFindAssertBonsai({a: {$lte: -Infinity}}, []);
