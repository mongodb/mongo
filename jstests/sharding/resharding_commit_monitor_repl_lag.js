/**
 * Tests that the _shardsvrReshardingOperationTime command returns the majority replication lag.
 */
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function waitUntilReshardingInitialized(recipientPrimary, ns) {
    assert.soon(() => {
        const currentOp = recipientPrimary.getDB("admin")
                              .aggregate([
                                  {$currentOp: {allUsers: true}},
                                  {$match: {type: "op", "originatingCommand.reshardCollection": ns}}
                              ])
                              .toArray();
        return currentOp.length > 0;
    });
}

const reshardingTest = new ReshardingTest({
    numDonors: 1,
    numRecipients: 1,
    // Make the fixture start 3-node shards.
    enableElections: true,
    // Disallow chaining to force both secondaries to sync from the primary. One of the test cases
    // below disables replication on one of the secondaries, with chaining that would effectively
    // disable replication on both secondaries, causing the test case to to fail since writeConcern
    // of w: 2 is unsatisfiable.
    chainingAllowed: false,
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

// Set up the collection to reshard.
const dbName0 = "testDb0";
const collName0 = "testColl0";
const ns0 = dbName0 + "." + collName0;
const coll0 = reshardingTest.createShardedCollection({
    ns: ns0,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[0]},
    ],
});
assert.commandWorked(coll0.insert([
    {_id: -1, oldKey: -10, newKey: 10},
    {_id: 1, oldKey: 10, newKey: -10},
]));

// Set up the collection to write to in order to introduce replication lag on the recipient shard.
const dbName1 = "testDb1";
const collName1 = "testColl1";
const ns1 = dbName1 + "." + collName1;
assert.commandWorked(reshardingTest._st.s.adminCommand(
    {enableSharding: dbName1, primaryShard: recipientShardNames[0]}));
const coll1 = reshardingTest._st.s.getCollection(ns1);
assert.commandWorked(coll1.insert([
    {_id: -1},
    {_id: 1},
]));

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
        ],
    },
    () => {
        const recipient0RS = reshardingTest.getReplSetForShard(recipientShardNames[0]);
        const recipient0Primary = recipient0RS.getPrimary();
        waitUntilReshardingInitialized(recipient0Primary, ns0);

        jsTest.log("Test the case where there is no replication lag");
        recipient0RS.awaitReplication();
        const res0 = assert.commandWorked(
            recipient0Primary.adminCommand({_shardsvrReshardingOperationTime: ns0}));
        assert(res0.hasOwnProperty("majorityReplicationLagMillis"), res0);
        assert(res0.majorityReplicationLagMillis, 0, res0);

        jsTest.log("Test the case where there is replication lag on only one secondary");
        stopServerReplication(recipient0RS.getSecondaries()[0]);
        const sleepMillis1 = 100;
        sleep(sleepMillis1);
        // Perform a write and and wait for it to replicate to the other secondary.
        assert.commandWorked(coll1.insert([{_id: -2}], {writeConcern: {w: 2}}));
        const res1 = assert.commandWorked(
            recipient0Primary.adminCommand({_shardsvrReshardingOperationTime: ns0}));
        assert(res1.hasOwnProperty("majorityReplicationLagMillis"), res1);
        assert(res1.majorityReplicationLagMillis, 0, res1);

        jsTest.log("Test the case where there is replication lag on both secondaries");
        stopServerReplication(recipient0RS.getSecondaries()[1]);
        const sleepMillis2 = 200;
        sleep(sleepMillis2);
        // Perform a write and and don't wait for it to replicate to secondaries since replication
        // has been paused on both secondaries.
        assert.commandWorked(coll1.insert([{_id: 2}], {writeConcern: {w: 1}}));
        const res2 = assert.commandWorked(
            recipient0Primary.adminCommand({_shardsvrReshardingOperationTime: ns0}));
        assert(res2.hasOwnProperty("majorityReplicationLagMillis"), res2);
        assert.gte(res2.majorityReplicationLagMillis, sleepMillis2, {res2});

        jsTest.log("Test the case where there is replication lag on only one secondary again");
        // Unpause replication on one of the secondaries. The majority replication lag should become
        // 0 eventually.
        restartServerReplication(recipient0RS.getSecondaries()[0]);
        assert.soon(() => {
            const res3 = assert.commandWorked(
                recipient0Primary.adminCommand({_shardsvrReshardingOperationTime: ns0}));
            assert(res3.hasOwnProperty("majorityReplicationLagMillis"), res2);
            return res3.majorityReplicationLagMillis == 0;
        });

        restartServerReplication(recipient0RS.getSecondaries()[1]);
    });

reshardingTest.teardown();
