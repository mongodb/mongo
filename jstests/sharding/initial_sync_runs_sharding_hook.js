/**
 * Tests that we will run the appropriate hook after initial sync completes.
 *
 * @tags: [requires_fcv_50]
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const st = new ShardingTest({config: 1, shards: {rs0: {nodes: 1}}});
const rs = st.rs0;

const dbName = "testDB";
const collName = "testColl";

const primary = rs.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1}, {b: 2}, {c: 3}));

jsTestLog("Adding the initial-syncing node to the replica set.");
const secondary = rs.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {logComponentVerbosity: tojson({'sharding': 2})},
    shardsvr: ""
});

rs.reInitiate();
rs.awaitSecondaryNodes();
rs.awaitReplication();

jsTestLog("Checking for message indicating sharding hook ran.");
checkLog.containsJson(secondary, 5604000);

jsTestLog("Done with test.");
st.stop();
})();
