/**
 * Tests that we will run the appropriate hook after startup recovery completes.
 *
 * @tags: [requires_fcv_50, requires_persistence]
 */

(function() {
"use strict";

load('jstests/libs/fail_point_util.js');

const st = new ShardingTest({config: 1, shards: {rs0: {nodes: 1}}});
const rs = st.rs0;

const dbName = "testDB";
const collName = "testColl";

const primary = rs.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

jsTestLog("Adding a write to pin the stable timestamp at.");
const ts =
    assert.commandWorked(testDB.runCommand({insert: testColl.getName(), documents: [{a: 1}]}))
        .operationTime;
configureFailPoint(primary, 'holdStableTimestampAtSpecificTimestamp', {timestamp: ts});

jsTestLog("Adding more data.");
assert.commandWorked(testColl.insert([{b: 2}, {c: 3}]));

jsTestLog("Restarting node. It should go into startup recovery.");
rs.restart(primary, {setParameter: {logComponentVerbosity: tojson({'sharding': 2})}});

jsTestLog("Checking for expected message.");
checkLog.containsJson(primary, 5604000);

jsTestLog("Done with test.");
st.stop();
})();
