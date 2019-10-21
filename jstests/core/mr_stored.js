// This test expects a function stored in the system.js collection to be available for a map/reduce,
// which may not be the case if it is implicitly sharded in a passthrough.
// @tags: [
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

// Use a unique database name to avoid conflicts with other tests that directly modify
// system.js.
const testDB = db.getSiblingDB("mr_stored");
const coll = testDB.test;
coll.drop();

assert.commandWorked(coll.insert({"partner": 1, "visits": 9}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 9}));
assert.commandWorked(coll.insert({"partner": 1, "visits": 11}));
assert.commandWorked(coll.insert({"partner": 1, "visits": 30}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 41}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 41}));

let map = function(obj) {
    emit(obj.partner, {stats: [obj.visits]});
};

let reduce = function(k, v) {
    var stats = [];
    var total = 0;
    for (var i = 0; i < v.length; i++) {
        for (var j in v[i].stats) {
            stats.push(v[i].stats[j]);
            total += v[i].stats[j];
        }
    }
    return {stats: stats, total: total};
};

// Test that map reduce works with stored javascript
assert.commandWorked(testDB.system.js.insert({_id: "mr_stored_map", value: map}));
assert.commandWorked(testDB.system.js.insert({_id: "mr_stored_reduce", value: reduce}));

const out = testDB.mr_stored_out;

assert.commandWorked(coll.mapReduce(
    function() {
        mr_stored_map(this);
    },
    function(k, v) {
        return mr_stored_reduce(k, v);
    },
    {out: "mr_stored_out", scope: {xx: 1}}));

let z = out.convertToSingleObject("value");
assert.eq(2, Object.keySet(z).length);
assert.eq([9, 11, 30], z["1"].stats);
assert.eq([9, 41, 41], z["2"].stats);

out.drop();

map = function(obj) {
    var x = "partner";
    var y = "visits";
    emit(obj[x], {stats: [obj[y]]});
};

assert.commandWorked(testDB.system.js.save({_id: "mr_stored_map", value: map}));

assert.commandWorked(coll.mapReduce(
    function() {
        mr_stored_map(this);
    },
    function(k, v) {
        return mr_stored_reduce(k, v);
    },
    {out: "mr_stored_out", scope: {xx: 1}}));

z = out.convertToSingleObject("value");
assert.eq(2, Object.keySet(z).length);
assert.eq([9, 11, 30], z["1"].stats);
assert.eq([9, 41, 41], z["2"].stats);

assert.commandWorked(testDB.system.js.remove({_id: "mr_stored_map"}));
assert.commandWorked(testDB.system.js.remove({_id: "mr_stored_reduce"}));

out.drop();
}());
