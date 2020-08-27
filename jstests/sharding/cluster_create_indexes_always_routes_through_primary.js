// Ensure that a call to createIndexes in a sharded cluster will route to the primary, even when
// setSecondaryOk() is set to true.
(function() {
'use strict';

let st = new ShardingTest({shards: {rs0: {nodes: 2}}});
const testDBName = jsTestName();
const collName = 'coll';
const testDB = st.s.getDB(testDBName);

assert.commandWorked(testDB.adminCommand({enableSharding: testDBName}));
assert.commandWorked(
    testDB.adminCommand({shardCollection: testDB[collName].getFullName(), key: {x: 1}}));

st.s.setSecondaryOk();
assert.commandWorked(
    testDB.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]}));

st.stop();
})();
