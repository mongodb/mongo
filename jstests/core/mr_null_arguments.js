// Test that MapReduce only runs when certain arguments are explicit null. This is required because
// the Java Driver adds null as an argument to these fields if they are missing in the command from
// the user.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const coll = db.mr_null_test;
const outColl = db.mr_null_test_out;
assert.commandWorked(coll.insert({val: 1}));

function mapFunc() {
    emit(this.val, 1);
}
function reduceFunc(k, v) {
    return Array.sum(v);
}

// Test that finalize can be explicit null.
assert.commandWorked(db.runCommand({
    mapReduce: coll.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    finalize: null,
    out: {merge: outColl.getName()}
}));

// Test that query can be explicit null.
assert.commandWorked(db.runCommand({
    mapReduce: coll.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    query: null,
    out: {merge: outColl.getName()}
}));

// Test that sort can be explicit null.
assert.commandWorked(db.runCommand({
    mapReduce: coll.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    sort: null,
    out: {merge: outColl.getName()}
}));

// Test that scope can be explicit null.
assert.commandWorked(db.runCommand({
    mapReduce: coll.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    scope: null,
    out: {merge: outColl.getName()}
}));
})();
