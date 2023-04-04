/**
 * Tests that compound wildcard indexes with queries on non-wildcard prefix are used correctly for
 * OR queries.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_concern_local,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");     // For arrayEq().
load("jstests/libs/wildcard_index_helpers.js");  // For WildcardIndexHelpers.

const documentList = [
    {
        _id: 428,
        "str": "Chicken RAM Nepal",
    },
    {
        _id: 1311,
        "str": "navigate Stravenue",
        "obj": {
            _id: 1313,
            "obj": {
                _id: 1314,
                "obj": {
                    _id: 1315,
                    "obj": {
                        _id: 1316,
                        "obj": {},
                    },
                },
            },
        },
    },
];

const pipeline =
    [{$match: {$or: [{"str": {$regex: /^Chicken/}}, {"obj.obj.obj.obj.obj": {$exists: true}}]}}];

const indexList = [{"obj.obj.obj.$**": 1}, {"str": -1, "obj.obj.obj.obj.$**": -1}];

const coll = db[jsTestName() + "_wild"];
const wild = db[jsTestName() + "_no_idx"];

coll.drop();
wild.drop();

assert.commandWorked(coll.insertMany(documentList));
assert.commandWorked(wild.insertMany(documentList));

assert.commandWorked(wild.createIndexes(indexList));

const noIdxResult = coll.aggregate(pipeline).toArray();
const idxResult = wild.aggregate(pipeline).toArray();

assertArrayEq({expected: documentList, actual: noIdxResult});
assertArrayEq({expected: noIdxResult, actual: idxResult});

const explain = assert.commandWorked(wild.explain('executionStats').aggregate(pipeline));

// We want to make sure that the correct expanded CWI key pattern was used. The CWI,
// {"str": -1, "obj.obj.obj.obj.$**": -1}, could be expanded internally to two key patterns:
//      1) {"str": -1, "obj.obj.obj.obj.obj": -1} for predicates including "obj.obj.obj.obj.obj".
//      2) {"str": -1, "$_path": -1} for queries only on the prefix field 'str'.
// The latter key pattern should be used for the predicate with {"str": {$regex: /^Chicken/}}.
const winningPlan = getWinningPlan(explain.queryPlanner);
const planStages = getPlanStages(winningPlan, 'IXSCAN');

let idxUsedCnt = 0;
for (const stage of planStages) {
    assert(stage.hasOwnProperty('indexName'), stage);
    if (stage.indexName === "str_-1_obj.obj.obj.obj.$**_-1") {
        idxUsedCnt++;

        // This key pattern should contain "$_path" rather than any specific field.
        const expectedKeyPattern = {"str": -1, "$_path": 1};
        assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        assert.eq(stage.indexBounds["$_path"], ["[MinKey, MaxKey]"], stage);
    }
    if (stage.indexName === "obj.obj.obj.$**_1") {
        idxUsedCnt++;

        // This key pattern is a single-field wildcard index.
        const expectedKeyPattern = {"$_path": 1, "obj.obj.obj.obj.obj": 1};
        assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        assert.eq(stage.indexBounds["$_path"],
                  [
                      "[\"obj.obj.obj.obj.obj\", \"obj.obj.obj.obj.obj\"]",
                      "[\"obj.obj.obj.obj.obj.\", \"obj.obj.obj.obj.obj/\")"
                  ],
                  stage);
        assert.eq(stage.indexBounds["obj.obj.obj.obj.obj"], ["[MinKey, MaxKey]"], stage);
    }
}
assert.eq(idxUsedCnt, 2);
})();
