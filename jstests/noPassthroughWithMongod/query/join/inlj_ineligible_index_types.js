/**
 * Ensures that ineligible indexes on the RHS (foreign) collection are never used as INLJ probe
 * indexes. Ineligible index types include: hashed, 2dsphere, 2d, text, wildcard, and BTREE indexes
 * that are hidden, partial, sparse, or have a non-simple collation.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {getPlanStage} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
    },
});
const db = conn.getDB(jsTestName());

const local = db.local;
const foreign = db.foreign;

function setup() {
    local.drop();
    foreign.drop();

    assert.commandWorked(
        local.insertMany([
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 3},
        ]),
    );
    assert.commandWorked(
        foreign.insertMany([
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 3},
        ]),
    );
}

function setupGeo2d() {
    local.drop();
    foreign.drop();

    assert.commandWorked(
        local.insertMany([
            {_id: 0, a: [0, 0]},
            {_id: 1, a: [1, 1]},
            {_id: 2, a: [2, 2]},
        ]),
    );
    assert.commandWorked(
        foreign.insertMany([
            {_id: 0, a: [0, 0]},
            {_id: 1, a: [1, 1]},
            {_id: 2, a: [2, 2]},
        ]),
    );
}

function setupGeo2dsphere() {
    local.drop();
    foreign.drop();

    const makePt = (i) => ({type: "Point", coordinates: [i, i]});
    assert.commandWorked(
        local.insertMany([
            {_id: 0, a: makePt(0)},
            {_id: 1, a: makePt(1)},
            {_id: 2, a: makePt(2)},
        ]),
    );
    assert.commandWorked(
        foreign.insertMany([
            {_id: 0, a: makePt(0)},
            {_id: 1, a: makePt(1)},
            {_id: 2, a: makePt(2)},
        ]),
    );
}

// Force INLJ such that the second join node is always on the RHS/ an IndexProbe.
function getPipeline(foreignColl) {
    return [
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST", hint: {node: NumberInt(0)}},
                    {
                        level: NumberInt(1),
                        mode: "CHEAPEST",
                        hint: {method: "INLJ", isLeftChild: false, node: NumberInt(1)},
                    },
                ],
            },
        },
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "a", as: "joined"}},
        {$unwind: "$joined"},
    ];
}

/**
 * Asserts that forcing INLJ fails because no eligible probe index exists.
 */
function assertINLJFails(baseColl, foreignColl) {
    assert.throwsWithCode(
        () => baseColl.aggregate(getPipeline(foreignColl)).toArray(),
        [ErrorCodes.QueryFeatureNotAllowed, ErrorCodes.QueryRejectedBySettings],
    );
}

/**
 * Ensure we pick the "correct"/ "valid" index "a_1".
 */
function validateINLJ(explain) {
    const inlj = getPlanStage(explain, "INDEXED_NESTED_LOOP_JOIN_EMBEDDING");
    assert(inlj);
    const ixprobe = getPlanStage(explain, "INDEX_PROBE_NODE");
    assert(ixprobe);
    assert.eq(ixprobe.indexName, "a_1");
    assert.eq(ixprobe.isMultiKey, false);
    assert.eq(ixprobe.isUnique, false);
    assert.eq(ixprobe.isSparse, false);
    assert.eq(ixprobe.isPartial, false);
}

/**
 * Asserts that forcing INLJ succeeds and produces an INLJ plan.
 */
function assertINLJSucceeds() {
    validateINLJ(local.explain().aggregate(getPipeline(foreign)));
    validateINLJ(foreign.explain().aggregate(getPipeline(local)));
}

function testVariousIndexScenarios(badIndexDesc, badIndexOpts = {}, skipHappyPath = false) {
    // Local good, foreign bad.
    assert.commandWorked(local.dropIndexes());
    assert.commandWorked(foreign.dropIndexes());
    assert.commandWorked(local.createIndex({a: 1}));
    assert.commandWorked(foreign.createIndex(badIndexDesc, badIndexOpts));
    assertINLJFails(local, foreign); // Make foreign use INLJProbe.

    // Foreign good, local bad.
    assert.commandWorked(local.dropIndexes());
    assert.commandWorked(foreign.dropIndexes());
    assert.commandWorked(local.createIndex(badIndexDesc, badIndexOpts));
    assert.commandWorked(foreign.createIndex({a: 1}));
    assertINLJFails(foreign, local); // Make local use INLJProbe.

    // Both bad.
    assert.commandWorked(foreign.dropIndexes());
    assert.commandWorked(foreign.createIndex(badIndexDesc, badIndexOpts));
    assertINLJFails(local, foreign);
    assertINLJFails(foreign, local);

    if (skipHappyPath) {
        return;
    }

    // Both have good index- skip past bad index, use good index.
    assert.commandWorked(local.createIndex({a: 1}));
    assert.commandWorked(foreign.createIndex({a: 1}));
    assertINLJSucceeds();
}

// Base case: no issues using INLJ with this query.
setup();
assert.commandWorked(local.createIndex({a: 1}));
assert.commandWorked(foreign.createIndex({a: 1}));
assertINLJSucceeds();

testVariousIndexScenarios({a: "hashed"});
testVariousIndexScenarios({a: "text"});
testVariousIndexScenarios({"$**": 1});
testVariousIndexScenarios({"a": 1, "$**": 1}, {"wildcardProjection": {"_id": 1}});
testVariousIndexScenarios({"a": 1}, {hidden: true}, true);
testVariousIndexScenarios({"a": 1}, {partialFilterExpression: {a: {$gt: 0}}}, true);
testVariousIndexScenarios({"a": 1}, {sparse: true}, true);
testVariousIndexScenarios({"a": 1}, {collation: {locale: "en"}}, true);

setupGeo2d();
testVariousIndexScenarios({a: "2d"}, {}, true);

setupGeo2dsphere();
testVariousIndexScenarios({a: "2dsphere"}, {}, true);

MongoRunner.stopMongod(conn);
