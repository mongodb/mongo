/**
 * Test that indexes on arrays can satisfy an $eq/$lt predicate, which matches if any
 * array element matches.
 *
 * @tags: [
 *   # Checks for 'IndexScan' node in explain.
 *   requires_cqf,
 * ]
 */
import {show} from "jstests/libs/golden_test.js";
import {getPlanSkeleton} from "jstests/libs/query/optimizer_utils.js";

db.setLogLevel(4, "query");

const coll = db.cqf_array_index;
coll.drop();
assert.commandWorked(coll.insert([
    {a: [1, 2, 3, 4]},
    {a: [2, 3, 4]},
    {a: [2]},
    {a: 2},
    {a: [1, 3]},
]));
// Generate enough documents for index to be preferable.
assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({a: i + 10}))));
assert.commandWorked(coll.createIndex({a: 1}));

function run(pipeline) {
    jsTestLog(`Query: ${tojsononeline(pipeline)}`);
    show(coll.aggregate(pipeline));
    const explain = coll.explain("executionStats").aggregate(pipeline);
    print(`nReturned: ${explain.executionStats.nReturned}\n`);
    print(`Plan skeleton: `);
    printjson(getPlanSkeleton(explain));
}

run([{$match: {a: 2}}, {$unset: '_id'}]);
run([{$match: {a: {$lt: 2}}}, {$unset: '_id'}]);
