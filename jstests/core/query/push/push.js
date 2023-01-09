/**
 * Tests array append/removal operations using $push.
 *
 * The $push operator also supports modifiers such as $each, $slice, $sort, and $position
 * that are described in the documentation:
 *     https://docs.mongodb.com/manual/reference/operator/update/push/#modifiers
 *
 * Tests for these modifiers may be found in:
 * - jstests/core/push_sort.js
 * - src/mongod/db/update/push_node_test.cpp
 */
(function() {
'use strict';

const t = db.push;
t.drop();

assert.commandWorked(t.insert({_id: 2, a: [1]}));
assert.commandWorked(t.update({_id: 2}, {$push: {a: 2}}));
assert.sameMembers([1, 2], t.findOne().a, "Array should contain 2 elements after appending 2.");
assert.commandWorked(t.update({_id: 2}, {$push: {a: 3}}));
assert.sameMembers([1, 2, 3], t.findOne().a, "Array should contain 3 elements after appending 3.");

assert.commandWorked(t.update({_id: 2}, {$pop: {a: 1}}));
assert.sameMembers(
    [1, 2], t.findOne().a, "Array should contain 2 elements after removing the last element.");
assert.commandWorked(t.update({_id: 2}, {$pop: {a: -1}}));
assert.sameMembers(
    [2], t.findOne().a, "Array should contain 1 element after removing the first element.");

assert.commandWorked(t.update({_id: 2}, {$pop: {a: -1}}));
assert.sameMembers([], t.findOne().a, "Array should be empty after removing the first element");

assert.commandWorked(t.update({_id: 2}, {$pop: {a: 1}}));
assert.sameMembers(
    [], t.findOne().a, "Empty array should remain empty after trying to remove the last element.");

assert.commandWorked(t.update({_id: 2}, {$pop: {b: -1}}));
assert.sameMembers(
    [],
    t.findOne().a,
    "Popping (first element of) non-existent field should not affect array in field 'a'.");

assert.commandWorked(t.update({_id: 2}, {$pop: {b: 1}}));
assert.sameMembers(
    [],
    t.findOne().a,
    "Popping (last element of) non-existent field should not affect array in field 'a'.");
})();
