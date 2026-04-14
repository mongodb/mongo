/**
 * Tests that the internalJoinMethod query knob correctly forces the join optimizer to use the
 * specified join method (HJ, NLJ, INLJ) for all joins in the plan, and that forcing an
 * infeasible method (e.g., INLJ without indexes) results in an error.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {assertAllJoinsUseMethod} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
const db = conn.getDB(jsTestName());

const coll = db[jsTestName()];
const foreign1 = db[jsTestName() + "_foreign1"];
const foreign2 = db[jsTestName() + "_foreign2"];

coll.drop();
foreign1.drop();
foreign2.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 2, b: "bar"},
        {_id: 2, a: 2, b: "foo"},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll.createIndex({dummy: 1, a: 1, b: 1}));

assert.commandWorked(
    foreign1.insertMany([
        {_id: 0, a: 1, c: "x"},
        {_id: 1, a: 2, c: "y"},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreign1.createIndex({dummy: 1, a: 1, c: 1}));

assert.commandWorked(
    foreign2.insertMany([
        {_id: 0, b: "foo", d: 10},
        {_id: 1, b: "bar", d: 20},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreign2.createIndex({dummy: 1, b: 1, d: 1}));

assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

const singleJoinPipeline = [
    {$lookup: {from: foreign1.getName(), as: "f1", localField: "a", foreignField: "a"}},
    {$unwind: "$f1"},
];

const twoJoinPipeline = [
    {$lookup: {from: foreign1.getName(), as: "f1", localField: "a", foreignField: "a"}},
    {$unwind: "$f1"},
    {$lookup: {from: foreign2.getName(), as: "f2", localField: "b", foreignField: "b"}},
    {$unwind: "$f2"},
];

const enumerators = [
    {name: "bottomUp (CHEAPEST)", params: {internalJoinReorderMode: "bottomUp", internalJoinPlanTreeShape: "leftDeep"}},
    {
        name: "bottomUp (ALL)",
        params: {
            internalJoinReorderMode: "bottomUp",
            internalJoinPlanTreeShape: "leftDeep",
            internalMinAllPlansEnumerationSubsetLevel: 0,
        },
    },
    {name: "random", params: {internalJoinReorderMode: "random", internalRandomJoinOrderSeed: 42}},
];

/**
 * Runs a forced join method test with both the bottom-up and random enumerators. Asserts that every
 * join node in the plan uses 'forcedMethod'.
 */
function runForcedJoinMethodTest(pipeline, forcedMethod) {
    for (const {name, params} of enumerators) {
        jsTest.log.info(`Force ${forcedMethod} with ${name} enumerator`);
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinMethod: forcedMethod, ...params}));

        const explain = coll.explain().aggregate(pipeline);
        assertAllJoinsUseMethod(explain, forcedMethod);
    }
}

/**
 * Asserts that forcing the given join method causes an error with both enumerators.
 */
function assertForcedMethodFails(pipeline, forcedMethod) {
    for (const {name, params} of enumerators) {
        jsTest.log.info(`Force ${forcedMethod} with ${name} enumerator (expect failure)`);
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinMethod: forcedMethod, ...params}));

        assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.QueryRejectedBySettings);
    }
}

// Force HJ.
runForcedJoinMethodTest(singleJoinPipeline, "HJ");
runForcedJoinMethodTest(twoJoinPipeline, "HJ");

// Force NLJ.
runForcedJoinMethodTest(singleJoinPipeline, "NLJ");
runForcedJoinMethodTest(twoJoinPipeline, "NLJ");

// Force INLJ with indexes.
assert.commandWorked(foreign1.createIndex({a: 1}));
assert.commandWorked(foreign2.createIndex({b: 1}));

runForcedJoinMethodTest(singleJoinPipeline, "INLJ");
runForcedJoinMethodTest(twoJoinPipeline, "INLJ");

// Force INLJ without indexes.
assert.commandWorked(foreign1.dropIndex({a: 1}));
assert.commandWorked(foreign2.dropIndex({b: 1}));

assertForcedMethodFails(singleJoinPipeline, "INLJ");
assertForcedMethodFails(twoJoinPipeline, "INLJ");

MongoRunner.stopMongod(conn);
