/**
 * Tests that we will run the appropriate hook after initial sync completes.
 *
 * @tags: [requires_fcv_60]
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({nodes: 1, name: jsTestName()});
rst.startSet();
rst.initiate();

const dbName = "testDB";
const collName = "testColl";

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1}, {b: 2}, {c: 3}));

jsTestLog("Adding the initial-syncing node to the replica set.");
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {logComponentVerbosity: tojson({'sharding': 2})}
});

rst.reInitiate();
rst.awaitSecondaryNodes();
rst.awaitReplication();

jsTestLog("Checking for message indicating sharding hook ran.");
checkLog.containsJson(secondary, 6351912);

jsTestLog("Done with test.");
rst.stopSet();
})();
