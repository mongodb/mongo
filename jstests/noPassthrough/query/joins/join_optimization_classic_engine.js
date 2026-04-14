/**
 * Ensures that the join optimizer is not used when forceClassicEngine is enabled. It does so by running the
 * same lookup + unwind pipeline twice and ensures the join optimization was used during the first run with
 * trySbeEngine but not during the second invocation with forceClassicEngine.
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

let conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll.createIndex({dummy: 1, a: 1, b: 1}));

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();
assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreignColl1.createIndex({dummy: 1, a: 1, c: 1, d: 1}));

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", d: 2},
        {_id: 1, b: "bar", d: 6},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreignColl2.createIndex({dummy: 1, b: 1, d: 1}));

let pipeline = [
    {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
    {$unwind: "$y"},
];

assert.commandWorked(
    conn.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: "trySbeEngine",
        internalEnableJoinOptimization: true,
    }),
);

let explain = coll.explain().aggregate(pipeline);

assert(joinOptUsed(explain), explain);

assert.commandWorked(
    conn.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: "forceClassicEngine",
    }),
);

explain = coll.explain().aggregate(pipeline);

assert(!joinOptUsed(explain), explain);

MongoRunner.stopMongod(conn);
