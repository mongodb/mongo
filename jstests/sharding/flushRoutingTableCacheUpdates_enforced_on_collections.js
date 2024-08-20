// Ensure that a call to _flushRoutingTableCacheUpdates in a sharded cluster will return error if
// attempted on a database instead of a collection.
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({});
const testDBName = jsTestName();
const collName = 'coll';
const testDB = st.s.getDB(testDBName);

assert.commandWorked(testDB.adminCommand({enableSharding: testDBName}));
assert.commandWorked(
    testDB.adminCommand({shardCollection: testDB[collName].getFullName(), key: {x: 1}}));

// On a collection, the command works
assert.commandWorked(
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: testDB[collName].getFullName()}));

// But on a database, the command fails with error "IllegalOperation"
assert.commandFailedWithCode(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: testDBName}),
                             ErrorCodes.IllegalOperation);

// Test also variant _flushRoutingTableCacheUpdatesWithWriteConcern
assert.commandWorked(st.shard0.adminCommand({
    _flushRoutingTableCacheUpdatesWithWriteConcern: testDB[collName].getFullName(),
    writeConcern: {w: "majority"}
}));
assert.commandFailedWithCode(st.shard0.adminCommand({
    _flushRoutingTableCacheUpdatesWithWriteConcern: testDBName,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.IllegalOperation);

st.stop();
