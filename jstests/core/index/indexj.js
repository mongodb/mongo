// SERVER-726
// This test makes assertions about how many keys are examined during query execution, which can
// change depending on whether/how many documents are filtered out by the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
//   no_selinux,
//   # Different assertions are made depending on whether SBE or classic is used. Implicitly
//   # creating an index can change which engine is used.
//   assumes_no_implicit_index_creation,
//   # This test assumes that either SBE or classic is fully enabled and that we're not running in
//   # a mixed version cluster.
//   requires_fcv_63,
// ]

import {getPlanStages} from "jstests/libs/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const t = db[jsTestName()];
t.drop();

class ExplainWithKeysExamined {
    constructor(explain, keysExamined) {
        this.explain = explain;
        this.keysExamined = keysExamined;
    }
}

function keysExamined(query, hint, sort) {
    if (!hint) {
        hint = {};
    }
    if (!sort) {
        sort = {};
    }
    const explain = t.find(query).sort(sort).hint(hint).explain("executionStats");
    return new ExplainWithKeysExamined(explain, explain.executionStats.totalKeysExamined);
}

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}).keysExamined, "A");

assert(t.drop());
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert({a: 4}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}).keysExamined, "B");

assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}).keysExamined, "D");

assert.commandWorked(t.insert({a: 4}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}).keysExamined, "C");

assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}).keysExamined, "D");

assert(t.drop());
assert.commandWorked(t.createIndex({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 2}));
assert.commandWorked(t.insert({a: 2, b: 1}));
assert.commandWorked(t.insert({a: 2, b: 2}));

// We make different assertions about the number of keys examined depending on whether we are using
// SBE or the classic engine. This is because the classic engine will use a multi-interval index
// scan whereas SBE will decompose the intervals into a set of single-interval bounds and will end
// up examining 0 keys.
const isSBEEnabled = checkSbeFullyEnabled(db);
let expectedKeys = isSBEEnabled ? 0 : 3;
let errMsg = function(actualNumKeys) {
    return "Chosen plan examined " + actualNumKeys + " keys";
};
let keysExaminedRet = keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

keysExaminedRet =
    keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

assert.commandWorked(t.insert({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 1}));
keysExaminedRet = keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

keysExaminedRet = keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

keysExaminedRet =
    keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

// We examine one less key in the classic engine because the bounds are slightly tighter.
if (!isSBEEnabled) {
    expectedKeys = 2;
}

keysExaminedRet = keysExamined({a: {$in: [1, 1.9]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

keysExaminedRet =
    keysExamined({a: {$in: [1.1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));
assert.commandWorked(t.insert({a: 1, b: 1.5}));

// We examine one extra key in both engines because we've inserted a document that falls within
// both sets of bounds being scanned.
expectedKeys = isSBEEnabled ? 1 : 4;
keysExaminedRet = keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1});
assert.eq(keysExaminedRet.keysExamined, expectedKeys, errMsg(keysExaminedRet.keysExamined));

if (isSBEEnabled) {
    const explain = t.find({a: {$gte: 1, $lt: 3}, b: {$gte: 1, $lt: 3}})
                        .hint({a: 1, b: 1})
                        .explain("executionStats");
    const stage = getPlanStages(explain.executionStats.executionStages, "ixscan_generic");
    assert.eq(1, stage.length, explain);
    assert.eq(5, stage[0].keyCheckSkipped, stage);
}
