// @tags: [
//   does_not_support_stepdowns,
//   uses_testing_only_commands,
// ]
//
// Does not support stepdowns because if a stepdown were to occur between running find() and
// calling killCursors on the cursor ID returned by find(), the killCursors might be sent to
// different node than the one which has the cursor. This would result in the node returning
// "CursorNotFound."
//
// Test the killCursors command.
(function() {
'use strict';

var cmdRes;
var cursor;
var cursorId;

var coll = db.jstest_killcursors;
coll.drop();

for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({_id: i}));
}

// killCursors command should fail if the collection name is not a string.
cmdRes = db.runCommand(
    {killCursors: {foo: "bad collection param"}, cursors: [NumberLong(123), NumberLong(456)]});
assert.commandFailedWithCode(cmdRes, ErrorCodes.FailedToParse);

// killCursors command should fail if the cursors parameter is not an array.
cmdRes =
    db.runCommand({killCursors: coll.getName(), cursors: {a: NumberLong(123), b: NumberLong(456)}});
assert.commandFailedWithCode(cmdRes, ErrorCodes.FailedToParse);

// killCursors command should fail if the cursors parameter is an empty array.
cmdRes = db.runCommand({killCursors: coll.getName(), cursors: []});
assert.commandFailedWithCode(cmdRes, ErrorCodes.BadValue);

// killCursors command should report cursors as not found if the collection does not exist.
cmdRes = db.runCommand(
    {killCursors: "non-existent-collection", cursors: [NumberLong(123), NumberLong(456)]});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursorsKilled, []);
assert.eq(cmdRes.cursorsNotFound, [NumberLong(123), NumberLong(456)]);
assert.eq(cmdRes.cursorsAlive, []);
assert.eq(cmdRes.cursorsUnknown, []);

// killCursors command should report non-existent cursors as "not found".
cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [NumberLong(123), NumberLong(456)]});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursorsKilled, []);
assert.eq(cmdRes.cursorsNotFound, [NumberLong(123), NumberLong(456)]);
assert.eq(cmdRes.cursorsAlive, []);
assert.eq(cmdRes.cursorsUnknown, []);

// Test a case where one cursors exists and is killed but the other does not exist.
cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
assert.commandWorked(cmdRes);
cursorId = cmdRes.cursor.id;
assert.neq(cursorId, NumberLong(0));

cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [NumberLong(123), cursorId]});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursorsKilled, [cursorId]);
assert.eq(cmdRes.cursorsNotFound, [NumberLong(123)]);
assert.eq(cmdRes.cursorsAlive, []);
assert.eq(cmdRes.cursorsUnknown, []);

// Test killing a noTimeout cursor.
cmdRes = db.runCommand({find: coll.getName(), batchSize: 2, noCursorTimeout: true});
assert.commandWorked(cmdRes);
cursorId = cmdRes.cursor.id;
assert.neq(cursorId, NumberLong(0));

cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [NumberLong(123), cursorId]});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursorsKilled, [cursorId]);
assert.eq(cmdRes.cursorsNotFound, [NumberLong(123)]);
assert.eq(cmdRes.cursorsAlive, []);
assert.eq(cmdRes.cursorsUnknown, []);
})();
