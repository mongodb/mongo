/**
 * Test that, in a conjunction of two separate multikey predicates, each predicate can match
 * a separate array element. This means we must avoid satisfying both predicates with one
 * multikey index scan.
 *
 * Reproduces SERVER-71524.
 */
(function() {
"use strict";

const coll = db.query_golden_multiple_traverse_single_scan;
coll.drop();

assert.commandWorked(coll.insert({a: [{x: 1}, {y: 1}]}));
assert.commandWorked(coll.insert(Array.from({length: 100}, () => ({}))));

assert.commandWorked(coll.createIndex({a: 1}));

// Both predicates match, by matching different elements of 'a'.
// But the index will contain separate entries for {x: 1} and {y: 1}.
// An incorrect plan would force each index entry to match both predicates,
// returning an empty result-set.
show(coll.find({'a.x': 1, 'a.y': 1}, {_id: 0}));
})();
