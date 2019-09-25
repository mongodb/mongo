// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For resultsEq.

const t = db.mr5;
t.drop();

assert.commandWorked(t.insert({"partner": 1, "visits": 9}));
assert.commandWorked(t.insert({"partner": 2, "visits": 9}));
assert.commandWorked(t.insert({"partner": 1, "visits": 11}));
assert.commandWorked(t.insert({"partner": 1, "visits": 30}));
assert.commandWorked(t.insert({"partner": 2, "visits": 41}));
assert.commandWorked(t.insert({"partner": 2, "visits": 41}));

let mapper = function() {
    emit(this.partner, {stats: [this.visits]});
};

const reducer = function(k, v) {
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

assert.commandWorked(t.mapReduce(mapper, reducer, {out: "mr5_out", scope: {xx: 1}}));

let resultAsObj = db.mr5_out.convertToSingleObject("value");
assert.eq(2,
          Object.keySet(resultAsObj).length,
          `Expected 2 keys ("1" and "2") in object ${tojson(resultAsObj)}`);
// Use resultsEq() to avoid any assumptions about order.
assert(resultsEq([9, 11, 30], resultAsObj["1"].stats));
assert(resultsEq([9, 41, 41], resultAsObj["2"].stats));

db.mr5_out.drop();

mapper = function() {
    var x = "partner";
    var y = "visits";
    emit(this[x], {stats: [this[y]]});
};

assert.commandWorked(t.mapReduce(mapper, reducer, {out: "mr5_out", scope: {xx: 1}}));

resultAsObj = db.mr5_out.convertToSingleObject("value");
assert.eq(2,
          Object.keySet(resultAsObj).length,
          `Expected 2 keys ("1" and "2") in object ${tojson(resultAsObj)}`);
// Use resultsEq() to avoid any assumptions about order.
assert(resultsEq([9, 11, 30], resultAsObj["1"].stats));
assert(resultsEq([9, 41, 41], resultAsObj["2"].stats));

db.mr5_out.drop();
}());
