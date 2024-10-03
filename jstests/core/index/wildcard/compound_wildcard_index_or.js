/**
 * Tests that compound wildcard indexes with queries on non-wildcard prefix are used correctly for
 * OR queries.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_concern_local,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanStages, getQueryPlanner, getWinningPlan} from "jstests/libs/query/analyze_plan.js";

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

let explain = assert.commandWorked(wild.explain('executionStats').aggregate(pipeline));

// We want to make sure that the correct expanded CWI key pattern was used. The CWI,
// {"str": -1, "obj.obj.obj.obj.$**": -1}, could be expanded internally to two key patterns:
//      1) {"str": -1, "obj.obj.obj.obj.obj": -1} for predicates including "obj.obj.obj.obj.obj".
//      2) {"str": -1, "$_path": -1} for queries only on the prefix field 'str'.
// The latter key pattern should be used for the predicate with {"str": {$regex: /^Chicken/}}.
let winningPlan = getWinningPlan(getQueryPlanner(explain));
let planStages = getPlanStages(winningPlan, 'IXSCAN');

let idxUsedCnt = 0;
for (const stage of planStages) {
    assert(stage.hasOwnProperty('indexName'), stage);
    if (stage.indexName === "str_-1_obj.obj.obj.obj.$**_-1") {
        idxUsedCnt++;

        const expectedKeyPattern = {"str": -1, "$_path": 1};
        assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        // The index bounds of "$_path" should always be expanded to "all-value" bounds no matter
        // whether the CWI's key pattern being expanded to a known field or not.
        assert.eq(stage.indexBounds["$_path"], ["[MinKey, MinKey]", "[\"\", {})"], stage);
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
assert.eq(idxUsedCnt, 2, winningPlan);

// Test that two different CWI can be used to answer a $or query.
const collTwoCWI = db[jsTestName() + "_wild_2"];
const docs = [
    {num: 1, sub: {num: 1, str: 'aa'}, str: '1'},
    {num: 2, sub: {num: 2, str: 'bb'}, str: '2'},
    {num: 3, sub: {num: 3, str: 'cc'}, str: '3'},
];
collTwoCWI.drop();
assert.commandWorked(collTwoCWI.insertMany(docs));
assert.commandWorked(collTwoCWI.createIndexes([{num: 1, "sub.$**": 1}, {"sub.$**": 1, num: 1}]));

explain = assert.commandWorked(
    collTwoCWI.find({$or: [{num: {$gte: 1}}, {'sub.str': 'aa'}]}).explain("executionStats"));
winningPlan = getWinningPlan(explain.queryPlanner);
planStages = getPlanStages(winningPlan, 'IXSCAN');

idxUsedCnt = 0;
for (const stage of planStages) {
    assert(stage.hasOwnProperty('indexName'), stage);
    if (stage.indexName === "sub.$**_1_num_1") {
        idxUsedCnt++;

        const expectedKeyPattern = {"$_path": 1, "sub.str": 1, "num": 1};
        assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        // The "$_path" field shouldn't be expanded because this CWI is wildcard-field-prefixed.
        assert.eq(stage.indexBounds["$_path"], ["[\"sub.str\", \"sub.str\"]"], stage);
    }
    if (stage.indexName === "num_1_sub.$**_1") {
        idxUsedCnt++;

        // The CWI used to answer a $or query should be expanded to a generic CWI with "$_path"
        // field being the wildcard field.
        const expectedKeyPattern = {"num": 1, "$_path": 1};
        assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        assert.eq(stage.indexBounds["num"], ["[1.0, inf.0]"], stage);
        // The CWI used to answer a $or query should be expanded to include all paths and all keys
        // for the wildcard field.
        assert.eq(stage.indexBounds["$_path"], ["[MinKey, MinKey]", "[\"\", {})"], stage);
    }
}
assert.eq(idxUsedCnt, 2, winningPlan);

collTwoCWI.dropIndexes();
assert.commandWorked(collTwoCWI.createIndexes([{num: 1, "sub.$**": 1}, {str: 1, "sub.$**": 1}]));

// Test a filter with nested $and under a $or.
explain = assert.commandWorked(
    collTwoCWI
        .find({$or: [{$and: [{num: 1}, {"sub.num": {$gt: 4}}]}, {str: '1', "sub.num": {$lt: 10}}]})
        .explain("executionStats"));
winningPlan = getWinningPlan(explain.queryPlanner);
planStages = getPlanStages(winningPlan, 'IXSCAN');

idxUsedCnt = 0;
for (const stage of planStages) {
    assert(stage.hasOwnProperty('indexName'), stage);
    if (stage.indexName === "num_1_sub.$**_1") {
        idxUsedCnt++;

        // If the IndexScan stage has a filter on field 'sub.num', then this CWI's key pattern
        // cannot be overwritten.
        if (stage.hasOwnProperty("filter") && stage["filter"].hasOwnProperty("sub.num")) {
            const expectedKeyPattern = {"num": 1, "$_path": 1, "sub.num": 1};
            assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        } else {
            const expectedKeyPattern = {"num": 1, "$_path": 1};
            assert.eq(stage.keyPattern, expectedKeyPattern, stage);
            assert.eq(stage.indexBounds["$_path"], ["[MinKey, MinKey]", "[\"\", {})"], stage);
        }
    }
    if (stage.indexName === "str_1_sub.$**_1") {
        idxUsedCnt++;

        // If the IndexScan stage has a filter on field 'sub.num', then this CWI's key pattern
        // cannot be overwritten.
        if (stage.hasOwnProperty("filter") && stage["filter"].hasOwnProperty("sub.num")) {
            const expectedKeyPattern = {"num": 1, "$_path": 1, "sub.num": 1};
            assert.eq(stage.keyPattern, expectedKeyPattern, stage);
        } else {
            const expectedKeyPattern = {"str": 1, "$_path": 1};
            assert.eq(stage.keyPattern, expectedKeyPattern, stage);
            assert.eq(stage.indexBounds["$_path"], ["[MinKey, MinKey]", "[\"\", {})"], stage);
        }
    }
}
assert.eq(idxUsedCnt, 2, winningPlan);
