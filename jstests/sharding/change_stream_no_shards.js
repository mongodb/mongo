/**
 * Test that running a $changeStream aggregation on a cluster with no shards returns an empty result
 * set with a cursorID of zero.
 *
 * Requires no shards so there can't be a config shard.
 * @tags: [config_shard_incompatible]
 */
(function() {
const st = new ShardingTest({shards: 0});

const adminDB = st.s.getDB("admin");
const testDB = st.s.getDB("test");

// Test that attempting to open a stream on a single collection results in an empty, closed
// cursor response.
let csCmdRes = assert.commandWorked(
    testDB.runCommand({aggregate: "testing", pipeline: [{$changeStream: {}}], cursor: {}}));
assert.docEq([], csCmdRes.cursor.firstBatch);
assert.eq(csCmdRes.cursor.id, 0);

// Test that attempting to open a whole-db stream results in an empty, closed cursor response.
csCmdRes = assert.commandWorked(
    testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}));
assert.docEq([], csCmdRes.cursor.firstBatch);
assert.eq(csCmdRes.cursor.id, 0);

// Test that attempting to open a cluster-wide stream results in an empty, closed cursor
// response.
csCmdRes = assert.commandWorked(adminDB.runCommand(
    {aggregate: 1, pipeline: [{$changeStream: {allChangesForCluster: true}}], cursor: {}}));
assert.docEq([], csCmdRes.cursor.firstBatch);
assert.eq(csCmdRes.cursor.id, 0);

// Test that a regular, non-$changeStream aggregation also results in an empty cursor when no
// shards are present.
const nonCsCmdRes = assert.commandWorked(
    testDB.runCommand({aggregate: "testing", pipeline: [{$match: {}}], cursor: {}}));
assert.docEq([], nonCsCmdRes.cursor.firstBatch);
assert.eq(nonCsCmdRes.cursor.id, 0);

st.stop();
})();
