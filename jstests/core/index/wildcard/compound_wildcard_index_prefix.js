/**
 * Tests that compound wildcard indexes can support queries on non-wildcard prefix.
 *
 * @tags: [
 *   assumes_read_concern_local,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {WildcardIndexHelpers} from "jstests/libs/query/wildcard_index_helpers.js";

const coll = db.query_on_prefix_of_compound_wildcard_index;

const indexSpec = {
    keyPattern: {a: 1, b: 1, "sub.$**": 1, c: 1},
};
const supportedQueries = [{a: 1}, {a: 1, b: 1}, {a: 1, b: {$gt: 1}}];

const notSupportedQueries = [{b: 1}, {c: 1}];

const nonBlockingSorts = [{a: 1}, {a: 1, b: 1}];

const blockingSorts = [{a: 1, c: 1}, {a: 1, b: 1, c: 1}, {c: 1}];

// Create the compound wildcard index and store the 'indexName' in 'indexSpec'.
WildcardIndexHelpers.createIndex(coll, indexSpec);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: i, b: i * 2, sub: {a: i}, c: i * 3}));
}

function assertBlockingSort(explain, isBlocking) {
    const plan = getWinningPlanFromExplain(explain);
    const ixScans = getPlanStages(plan, "IXSCAN");
    const sorts = getPlanStages(plan, "SORT");

    if (isBlocking) {
        assert.gt(sorts.length, 0);
    } else {
        assert.eq(sorts.length, 0, explain);
    }
}

for (const query of notSupportedQueries) {
    let explainRes = assert.commandWorked(coll.find(query).explain("executionStats"));

    WildcardIndexHelpers.assertExpectedIndexIsNotUsed(explainRes, indexSpec.indexName);
}

for (const query of supportedQueries) {
    let explainRes = assert.commandWorked(coll.find(query).explain("executionStats"));

    WildcardIndexHelpers.assertExpectedIndexIsUsed(explainRes, indexSpec.indexName);

    for (const sortOrder of nonBlockingSorts) {
        explainRes = assert.commandWorked(coll.find(query).sort(sortOrder).explain("executionStats"));

        WildcardIndexHelpers.assertExpectedIndexIsUsed(explainRes, indexSpec.indexName);
        assertBlockingSort(explainRes, false);

        // Compare query results against $natural plan.
        const actual = coll.find(query).sort(sortOrder).toArray();
        const expected = coll.find(query).sort(sortOrder).hint({$natural: 1}).toArray();
        assertArrayEq({actual, expected});
    }

    for (const sortOrder of blockingSorts) {
        explainRes = assert.commandWorked(coll.find(query).sort(sortOrder).explain("executionStats"));

        WildcardIndexHelpers.assertExpectedIndexIsUsed(explainRes, indexSpec.indexName);
        assertBlockingSort(explainRes, true);

        // Compare query results against $natural plan.
        const actual = coll.find(query).sort(sortOrder).toArray();
        const expected = coll.find(query).sort(sortOrder).hint({$natural: 1}).toArray();
        assertArrayEq({actual, expected});
    }
}
