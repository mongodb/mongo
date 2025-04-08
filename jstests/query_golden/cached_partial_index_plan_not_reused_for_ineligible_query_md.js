/*
 * Test that a query eligible for partial index and gets cached does not affect the results of a
 * query with the same shape but ineligible for the partial index. This file is a regression
 * testcase for SERVER-102825.
 */

import {code, line, section, subSection} from "jstests/libs/pretty_md.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {outputPlanCacheStats} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

const docs = [
    {_id: 0, a: 0},
];

const q1 = {
    $or: [{a: 1}, {a: {$lte: 'a string'}}],
    _id: {$lte: 5}
};
const q2 = {
    $or: [{a: 1}, {a: {$lte: 10}}],
    _id: {$lte: 5}
};

coll.drop();
assert.commandWorked(coll.insert(docs));
section("Collection setup");
line("Inserting documents");
code(tojson(docs));

function runTest(pred) {
    subSection("Query");
    code(`${tojson(pred)}`);
    subSection("Results");
    const results = coll.find(pred).toArray();
    code(tojson(results));
}

section("Queries with no indexes");
runTest(q1);
runTest(q2);

const partialFilter = {
    $or: [{a: 1}, {a: {$lte: 'a string'}}]
};
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: partialFilter}));
section("Index setup");
line("Creating Index");
code(tojson({
    a: 1,
    partialFilterExpression: partialFilter,
}));

section("Queries with partial index");
// Ensure q1 is cached which uses the partial filter.
for (let i = 0; i < 3; i++) {
    runTest(q1);
}

subSection("Explain");
// Assert that q1 uses the partial index
const explain = coll.find(q1).explain();
code(tojson(getWinningPlanFromExplain(explain)));
subSection("Plan cache");
line("Verifying that the plan cache contains an entry with the partial index");
outputPlanCacheStats(coll);

// q2 should not used the partial index and return the document.
line(
    "Verify that 2nd query does not use cached partial index plan and returns the correct document");
runTest(q2);
