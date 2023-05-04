// @tags: [
//   # Step-down can cause mapReduce to fail.
//   does_not_support_stepdowns,
// ]
(function() {
"use strict";
const coll = db.bar;

assert.commandWorked(coll.insert({x: 1}));

const map = function() {
    emit(0, "mapped value");
};

const reduce = function(key, values) {
    return "reduced value";
};

const res = assert.commandWorked(
    db.runCommand({mapReduce: 'bar', map: map, reduce: reduce, out: {inline: 1}}));
assert.eq(res.results[0], {_id: 0, value: "reduced value"});
}());
