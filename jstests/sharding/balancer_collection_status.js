/**
 * Test the balancerCollectionStatus command and its possible outputs
 */

(function() {
'use strict';

const chunkSizeMB = 1;
let st = new ShardingTest({
    shards: 3,
    other: {
        // Set global max chunk size to 1MB
        chunkSize: chunkSizeMB
    }
});

function runBalancer() {
    st.startBalancer();

    // Let the balancer run until balanced.
    st.printShardingStatus(true);
    st.awaitBalance('col', 'db');
    st.printShardingStatus(true);

    st.stopBalancer();
}

// only fully quilified namespaces are allowed on the command
assert.commandFailedWithCode(st.s0.adminCommand({balancerCollectionStatus: 'db'}),
                             ErrorCodes.InvalidNamespace);

// only sharded databases are allowed
assert.commandFailedWithCode(st.s0.adminCommand({balancerCollectionStatus: 'db.col'}),
                             ErrorCodes.NamespaceNotSharded);

// setup the collection for the test
assert.commandWorked(st.s0.adminCommand({enableSharding: 'db'}));
assert.commandWorked(st.s0.adminCommand({shardCollection: 'db.col', key: {key: 1}}));

// only sharded collections are allowed
assert.commandWorked(st.s0.getDB('db').runCommand({create: "col2"}));
assert.commandFailedWithCode(st.s0.adminCommand({balancerCollectionStatus: 'db.col2'}),
                             ErrorCodes.NamespaceNotSharded);

let result = assert.commandWorked(st.s0.adminCommand({balancerCollectionStatus: 'db.col'}));

// new collections must be balanced
assert.eq(result.balancerCompliant, true);

// get shardIds
const shards = st.s0.getDB('config').shards.find().toArray();

const bigString = 'X'.repeat(1024 * 1024);  // 1MB
for (var i = 0; i < 30; i += 10) {
    assert.commandWorked(st.s0.getDB('db').getCollection('col').insert({key: i, s: bigString}));
}
// manually split and place the 3 chunks on the same shard
assert.commandWorked(st.s0.adminCommand({split: 'db.col', middle: {key: 10}}));
assert.commandWorked(st.s0.adminCommand({split: 'db.col', middle: {key: 20}}));
assert.commandWorked(st.s0.adminCommand({moveChunk: 'db.col', find: {key: 0}, to: shards[0]._id}));
assert.commandWorked(st.s0.adminCommand({moveChunk: 'db.col', find: {key: 10}, to: shards[0]._id}));
assert.commandWorked(st.s0.adminCommand({moveChunk: 'db.col', find: {key: 20}, to: shards[0]._id}));

// check the current status
result = assert.commandWorked(st.s0.adminCommand({balancerCollectionStatus: 'db.col'}));

// chunksImbalanced expected
assert.eq(result.balancerCompliant, false);
assert.eq(result.firstComplianceViolation, 'chunksImbalance');

// run balancer until balanced
runBalancer();

// manually move a chunk to a shard before creating zones (this will help
// testing the zone violation)
assert.commandWorked(st.s0.adminCommand({moveChunk: 'db.col', find: {key: 10}, to: shards[2]._id}));

// create zones on first two shards only
assert.commandWorked(st.s0.adminCommand({addShardToZone: shards[0]._id, zone: 'zone0'}));
assert.commandWorked(st.s0.adminCommand(
    {updateZoneKeyRange: 'db.col', min: {key: MinKey}, max: {key: 10}, zone: 'zone0'}));

assert.commandWorked(st.s0.adminCommand({addShardToZone: shards[1]._id, zone: 'zone1'}));
assert.commandWorked(st.s0.adminCommand(
    {updateZoneKeyRange: 'db.col', min: {key: 10}, max: {key: 20}, zone: 'zone1'}));

result = assert.commandWorked(st.s0.adminCommand({balancerCollectionStatus: 'db.col'}));

// having a chunk on a different zone will cause a zone violation
assert.eq(result.balancerCompliant, false);
assert.eq(result.firstComplianceViolation, 'zoneViolation');

// run balancer until balanced
runBalancer();
assert.eq(result.chunkSize, chunkSizeMB);

st.stop();
})();
