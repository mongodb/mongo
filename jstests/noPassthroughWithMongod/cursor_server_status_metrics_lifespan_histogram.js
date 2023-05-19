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

function getNumCursorsLessThan1Minute() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan1Minute;
}

function getNumCursorsLessThan10Minutes() {
    return db.serverStatus().metrics.cursor.lifespan.lessThan10Minutes;
}

for (let i = 0; i < 40; i++) {
    coll.insert({a: i, b: "field b"});
}

const initialNumCursorsLt1s = getNumCursorsLessThan1Second();
const initialNumCursorsLt5s = getNumCursorsLessThan5Seconds();
const initialNumCursorsLt15s = getNumCursorsLessThan15Seconds();
const initialNumCursorsLt30s = getNumCursorsLessThan30Seconds();
const initialNumCursorsLt1m = getNumCursorsLessThan1Minute();
const initialNumCursorsLt10m = getNumCursorsLessThan10Minutes();

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

const cursorLt1Minute = coll.find().batchSize(2);
const cursorLt10Minutes = coll.aggregate([], {cursor: {batchSize: 2}});
cursorLt1Minute.next();
cursorLt10Minutes.next();

sleep(31000);  // Sleep for 31 s.
while (cursorLt1Minute.hasNext()) {
    cursorLt1Minute.next();
}
assert.eq(getNumCursorsLessThan1Minute() - initialNumCursorsLt1m, 1);

sleep(30000);  // Sleep another 30s, so the total should be greater than 1m and less than 10m.
while (cursorLt10Minutes.hasNext()) {
    cursorLt10Minutes.next();
}
assert.eq(getNumCursorsLessThan10Minutes() - initialNumCursorsLt10m, 1);
}());
