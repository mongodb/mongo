/**
 * Tests that time-series collection that require extended range support are properly recognized
 * during startup recovery.
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({name: jsTest.name(), nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const dbName = "testDB";
const primaryDB = primary.getDB(dbName);

assert.commandWorked(primaryDB.createCollection("standard", {timeseries: {timeField: "time"}}));
assert.commandWorked(primaryDB.createCollection("extended", {timeseries: {timeField: "time"}}));
assert.commandWorked(primaryDB.standard.insert({time: ISODate("1980-01-01T00:00:00.000Z")}));
assert.commandWorked(primaryDB.extended.insert({time: ISODate("2040-01-01T00:00:00.000Z")}));

// Make sure the collections got flagged properly during the initial write.
checkLog.checkContainsWithCountJson(primary, 6679401, {"ns": "test.standard"}, 0, "WARNING", true);
checkLog.checkContainsWithCountJson(primary, 6679401, {"ns": "test.extended"}, 1, "WARNING", true);

rst.stop(primary);
rst.start(primary);
// Wait for the restarted node to complete startup recovery and start accepting user requests.
// Note: no new primary will be elected because of the high election timeout set on the replica set.
assert.soonNoExcept(function() {
    const nodeState = assert.commandWorked(primary.adminCommand("replSetGetStatus")).myState;
    return nodeState == ReplSetTest.State.SECONDARY;
});

// Make sure the collections get flagged properly again after startup.
checkLog.checkContainsWithCountJson(primary, 6679401, {"ns": "test.standard"}, 0, "WARNING", true);
checkLog.checkContainsWithCountJson(primary, 6679401, {"ns": "test.extended"}, 2, "WARNING", true);

rst.stopSet();
})();