/**
 * Explicit shell session should prohibit w: 0 writes.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const session = conn.startSession();
const sessionColl = session.getDatabase("test").getCollection("foo");
const err = assert.throws(() => {
    sessionColl.insert({x: 1}, {writeConcern: {w: 0}});
});

assert.includes(
    err.toString(), "Unacknowledged writes are prohibited with sessions", "wrong error message");

session.endSession();
MongoRunner.stopMongod(conn);
})();
