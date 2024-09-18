/**
 * Tests for serverStatus metrics.cursor.lifespan stats.
 */
(function() {
'use strict';
const coll = db[jsTest.name()];
coll.drop();

function getNumCursorsLessThan1Second() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan1Second;
}

function getNumCursorsLessThan5Seconds() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan5Seconds;
}

function getNumCursorsLessThan15Seconds() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan15Seconds;
}

function getNumCursorsLessThan30Seconds() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan30Seconds;
}

for (let i = 0; i < 40; i++) {
    coll.insert({a: i, b: "field b"});
}

const initialNumCursorsLt1s = getNumCursorsLessThan1Second();
const initialNumCursorsLt5s = getNumCursorsLessThan5Seconds();
const initialNumCursorsLt15s = getNumCursorsLessThan15Seconds();
const initialNumCursorsLt30s = getNumCursorsLessThan30Seconds();

// Since we aren't guaranteed perfect timings, the checks in this test have been relaxed to window
// sizes of 30s. For example, a cursor that is expected to die in under 5s may actually take longer
// than 5s to die on rare occasions due to slow computers, etc.; this is handled through a 30s grace
// period.
// Calculates the number of cursors that have lived less than 30s (< 1s, < 5s, etc.) since this test
// was started.
function cursorsDeadSinceStartLt30Seconds() {
    return (getNumCursorsLessThan1Second() + getNumCursorsLessThan5Seconds() +
            getNumCursorsLessThan15Seconds() + getNumCursorsLessThan30Seconds()) -
        (initialNumCursorsLt1s + initialNumCursorsLt5s + initialNumCursorsLt15s +
         initialNumCursorsLt30s);
}

let cursorId = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 2})).cursor.id;
db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
assert.eq(cursorsDeadSinceStartLt30Seconds(), 1);

// Create and kill 3 more cursors with lifespan < 30s.
for (let i = 0; i < 3; i++) {
    cursorId = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 2})).cursor.id;
    db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
}

assert.eq(cursorsDeadSinceStartLt30Seconds(), 4);
}());