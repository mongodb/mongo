/**
 * Test sorting with dotted field paths.
 *
 * This test expects some statements to error, which will cause a transaction (if one is open)
 * to abort entirely. Thus, we add the "does_not_support_transactions" tag to prevent this test
 * from being run in various the multi-statement passthrough testsuites.
 *
 * @tags: [
 *   does_not_support_transactions,
 * ]
 */
(function() {
"use strict";

const coll = db.sort_dotted_paths_parallel_arrays;
coll.drop();

// Verify that sort({a:1,b:1}) fails with a "parallel arrays" error when there is at least one
// document where both a and b are arrays.
assert.commandWorked(
    coll.insert([{_id: 1, a: [], b: 1}, {_id: 2, a: 1, b: []}, {_id: 3, a: [], b: []}]));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {_id: 1, a: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, _id: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, b: 1, _id: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);

// Verify that sort({"a.b":1,"a.c":1}) fails with a "parallel arrays" error when there is at least
// one document where both a.b and a.c are arrays.
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 1, a: {b: [1, 2], c: [3, 4]}}]));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {"a.b": 1, "a.c": 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);

// Verify that sort({"a.b":1,"c.d":1}) fails with a "parallel arrays" error when there is at least
// onw document where both a.b and c.d are arrays.
assert(coll.drop());
assert.commandWorked(coll.insert({a: {b: [1, 2]}, c: {d: [3, 4]}}));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {"a.b": 1, "c.d": 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
})();
