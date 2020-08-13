// Tests that the map function can access state of the 'this' object using both the '.x' and ["x"]
// syntax.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   sbe_incompatible,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For resultsEq.

const coll = db.mr_use_this_object;
coll.drop();
const outputColl = db.mr_use_this_object_out;
outputColl.drop();

assert.commandWorked(coll.insert([
    {partner: 1, visits: 9},
    {partner: 2, visits: 9},
    {partner: 1, visits: 11},
    {partner: 1, visits: 30},
    {partner: 2, visits: 41},
    {partner: 2, visits: 41}
]));

let mapper = function() {
    emit(this.partner, {stats: [this.visits]});
};

const reducer = function(k, v) {
    let stats = [];
    let total = 0;
    for (let i = 0; i < v.length; i++) {
        for (let j in v[i].stats) {
            stats.push(v[i].stats[j]);
            total += v[i].stats[j];
        }
    }
    return {stats: stats, total: total};
};

assert.commandWorked(coll.mapReduce(mapper, reducer, {out: {merge: outputColl.getName()}}));

let resultAsObj = outputColl.convertToSingleObject("value");
assert.eq(2,
          Object.keySet(resultAsObj).length,
          `Expected 2 keys ("1" and "2") in object ${tojson(resultAsObj)}`);
// Use resultsEq() to avoid any assumptions about order.
assert(resultsEq([9, 11, 30], resultAsObj["1"].stats));
assert(resultsEq([9, 41, 41], resultAsObj["2"].stats));

assert(outputColl.drop());

mapper = function() {
    let x = "partner";
    let y = "visits";
    emit(this[x], {stats: [this[y]]});
};

assert.commandWorked(coll.mapReduce(mapper, reducer, {out: {merge: outputColl.getName()}}));

resultAsObj = outputColl.convertToSingleObject("value");
assert.eq(2,
          Object.keySet(resultAsObj).length,
          `Expected 2 keys ("1" and "2") in object ${tojson(resultAsObj)}`);
// Use resultsEq() to avoid any assumptions about order.
assert(resultsEq([9, 11, 30], resultAsObj["1"].stats));
assert(resultsEq([9, 41, 41], resultAsObj["2"].stats));
}());
