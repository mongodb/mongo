/**
 * Test that $eq: null predicates include both null and missing, for both a
 * collection scan and index scan.
 *
 * @tags: [
 *   # Checks for CQF-specific node names in explain output.
 *   requires_cqf,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For getPlanSkeleton.

db.setLogLevel(4, "query");

const coll = db.cqf_null_missing;
coll.drop();

assert.commandWorked(coll.insert([
    {a: 2},
    {a: {b: null}},
    {a: {c: 1}},
]));
// Generate enough documents for index to be preferable.
assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({a: {b: i + 10}}))));

{
    const pipeline = [{$match: {'a.b': null}}];
    jsTestLog(`No indexes. Query: ${tojsononeline(pipeline)}`);
    const explain = coll.explain("executionStats").aggregate(pipeline);
    print(`nReturned: ${explain.executionStats.nReturned}\n`);
    print(`Plan skeleton: `);
    printjson(getPlanSkeleton(explain));
}

{
    const pipeline = [{$match: {'a.b': null}}];
    const index = {'a.b': 1};
    jsTestLog(`Index on ${tojsononeline(index)}. Query: ${tojson(pipeline)}`);
    assert.commandWorked(coll.createIndex(index));
    const explain = coll.explain("executionStats").aggregate([{$match: {'a.b': null}}]);
    print(`nReturned: ${explain.executionStats.nReturned}\n`);
    print(`Plan skeleton: `);
    printjson(getPlanSkeleton(explain));
}
})();
