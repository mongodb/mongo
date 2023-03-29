/**
 * Tests dropDatabase resilience against write conflict exceptions.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

assert.commandWorked(db.createCollection(jsTestName()));

const fp = configureFailPoint(primary, "throwWriteConflictExceptionDuringDropDatabase");

const awaitDropDB = startParallelShell(function() {
    assert.commandWorked(db.getSiblingDB("test").dropDatabase());
}, primary.port);

// Wait till at least one WriteConflict exception is thrown during dropDatabase.
fp.wait();
fp.off();

awaitDropDB();

rst.stopSet();
})();
