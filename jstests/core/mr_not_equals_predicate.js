// Tests that a mapReduce command can succeed when given a not equals predicate. This test was
// designed to reproduce SERVER-45177.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
// ]
(function() {
"use strict";
const coll = db.mr_ne_query;
assert.commandWorked(coll.insert([{key: 'one', val: 1}, {key: 'two', val: 2}]));
assert.doesNotThrow(() => coll.find({key: {$ne: 'one'}}));
const mapper = function() {
    emit(this._id, this.val);
};
const reducer = function(k, v) {
    return Array.sum(v);
};
assert.doesNotThrow(() => coll.mapReduce(mapper, reducer, {out: {inline: 1}, query: {}}));
assert.doesNotThrow(
    () => coll.mapReduce(mapper, reducer, {out: {inline: 1}, query: {key: {$ne: 'one'}}}));
}());
