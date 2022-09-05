/**
 * Ensures that the drop database oplog entry gets sent to the secondaries prior to dropping the
 * in memory state of the database in the primary during the 'dropDatabase' command.
 * Dropping the in memory state of the database before sending the drop database oplog entry to the
 * secondaries first can result in a situation where the secondaries crash during replication.
 * The secondaries would crash given this scenario:
 *     - At time 10: Primary drops database "A"'s in memory state.
 *     - At time 15: Primary creates database "a" and sends the create database oplog entries to the
 *                   secondaries.
 *     - At time 20: Secondaries apply create database oplog entry and crash because database "A"
 *                   still exists in-memory for them.
 *     - At time 25: Primary sends database "A"'s drop oplog entry to secondaries (already crashed).
 *
 * @tags: [requires_replication]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const dbNameUpper = "A";
const dbNameLower = "a";

const primary = rst.getPrimary();

let upperDB = primary.getDB(dbNameUpper);
assert.commandWorked(upperDB.createCollection("test"));

let failPoint = configureFailPoint(upperDB, 'dropDatabaseHangBeforeInMemoryDrop');
let awaitDropUpper = startParallelShell(() => {
    db.getSiblingDB("A").dropDatabase();
}, primary.port);

failPoint.wait();
let lowerDB = primary.getDB(dbNameLower);

// The oplog entry to the secondaries to drop database "A" was sent, but the primary has not yet
// dropped "A" as it's hanging on the 'dropDatabaseHangBeforeInMemoryDrop' fail point.
assert.commandFailedWithCode(lowerDB.createCollection("test"), ErrorCodes.DatabaseDifferCase);

rst.awaitReplication();
failPoint.off();

checkLog.containsJson(primary, 20336, {"db": dbNameUpper});
assert.commandWorked(lowerDB.createCollection("test"));

awaitDropUpper();

rst.stopSet();
})();
