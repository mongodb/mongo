// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const coll = db.mr_replace;
coll.drop();

assert.commandWorked(coll.insert([{a: [1, 2]}, {a: [2, 3]}, {a: [3, 4]}]));

const outCollStr = "mr_replace_col";
const outDbStr = "mr_db";
const outDb = db.getMongo().getDB(outDbStr);
const outColl = outDb[outCollStr];

const mapFn = function() {
    for (i = 0; i < this.a.length; i++)
        emit(this.a[i], 1);
};
const reduceFn = function(k, vs) {
    return Array.sum(vs);
};

// TODO SERVER-42511 we should fix up and call the functions in this file once we flip on the new
// implementation.
function testReplace() {
    let res = assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: {replace: outCollStr, db: outDbStr}}));
    const expected =
        [{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}];
    let actual = outColl.find().sort({_id: 1}).toArray();
    assert.eq(expected, actual);

    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);

    // Run the whole thing again and make sure it does the same thing.
    assert.commandWorked(outColl.insert({_id: 5, value: 1}));
    assert.commandWorked(
        coll.mapReduce(mapFn, reduceFn, {out: {replace: outCollStr, db: outDbStr}}));
    actual = outColl.find().sort({_id: 1}).toArray();
    assert.eq(expected, actual);
}

function testMerge() {
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
}

function testReduce() {
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
}
}());
