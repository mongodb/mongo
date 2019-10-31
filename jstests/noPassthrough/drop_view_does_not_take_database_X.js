/**
 * Test that drop view only takes database IX lock.
 *
 * @tags: [requires_db_locking]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod({});
const db = conn.getDB("test");

assert.commandWorked(db.runCommand({insert: "a", documents: [{x: 1}]}));
assert.commandWorked(db.createView("view", "a", []));

const failPoint = configureFailPoint(db, "hangDuringDropCollection");

// This only holds a database IX lock.
const awaitDrop =
    startParallelShell(() => assert(db.getSiblingDB("test")["view"].drop()), conn.port);
failPoint.wait();

// This takes a database IX lock and should not be blocked.
assert.commandWorked(db.runCommand({insert: "a", documents: [{y: 1}]}));

failPoint.off();

awaitDrop();
MongoRunner.stopMongod(conn);
})();
