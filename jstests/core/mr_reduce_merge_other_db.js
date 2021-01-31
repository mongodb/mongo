// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const testName = "mr_reduce_merge_other_db";
const testDB = db.getSiblingDB(testName);
const coll = testDB.mr_replace;
coll.drop();

assert.commandWorked(coll.insert([{a: [1, 2]}, {a: [2, 3]}, {a: [3, 4]}]));

const outCollStr = "mr_replace_col_" + testName;
const outDbStr = "mr_db_" + testName;
const outDb = db.getMongo().getDB(outDbStr);
const outColl = outDb[outCollStr];

const mapFn = function() {
    for (i = 0; i < this.a.length; i++)
        emit(this.a[i], 1);
};
const reduceFn = function(k, vs) {
    return Array.sum(vs);
};

(function testMerge() {
    outColl.drop();
    assert.commandWorked(
        outColl.insert([{_id: 1, value: "something else"}, {_id: 5, value: "existing"}]));
    let res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: {merge: outCollStr, db: outDbStr}}));
    const expected = [
        {_id: 1, value: 1},
        {_id: 2, value: 2},
        {_id: 3, value: 2},
        {_id: 4, value: 1},
        {_id: 5, value: "existing"}
    ];
    let actual = outColl.find().sort({_id: 1}).toArray();
    assert.eq(expected, actual);
})();

(function testReduce() {
    outColl.drop();
    assert.commandWorked(outColl.insert([{_id: 1, value: 100}, {_id: 5, value: 0}]));
    let res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: {reduce: outCollStr, db: outDbStr}}));
    const expected = [
        {_id: 1, value: 101},
        {_id: 2, value: 2},
        {_id: 3, value: 2},
        {_id: 4, value: 1},
        {_id: 5, value: 0}
    ];
    let actual = outColl.find().sort({_id: 1}).toArray();
    assert.eq(expected, actual);
})();
}());
