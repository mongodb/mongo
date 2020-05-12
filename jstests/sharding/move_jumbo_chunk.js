/**
 * Test that a jumbo chunk can be moved using both manually and by the balancer when the
 * 'forceJumbo' option is set to true.
 *
 * TODO (SERVER-46420): Fix test to allow it to work with the resumable range deleter enabled.
 * @tags: [__TEMPORARILY_DISABLED__]
 */

(function() {
'use strict';

let st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {chunkSize: 1},
});

let kDbName = "test";
assert.commandWorked(st.s.adminCommand({enablesharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
const largeString = 'X'.repeat(10000);

function shardCollectionCreateJumboChunk() {
    assert.commandWorked(st.s.adminCommand({shardcollection: kDbName + ".foo", key: {x: 1}}));

    // Create sufficient documents to create a jumbo chunk, and use the same shard key in all of
    // them so that the chunk cannot be split.
    let i = 0;
    let bulk = st.s.getDB(kDbName).foo.initializeUnorderedBulkOp();
    for (; i < 2000; i++) {
        bulk.insert({x: 0, big: largeString, i: i});
    }
    assert.commandWorked(bulk.execute());
}

function diff() {
    var x = st.chunkCounts("foo");
    printjson(x);
    return Math.max(x[st.shard0.shardName], x[st.shard1.shardName]) -
        Math.min(x[st.shard0.shardName], x[st.shard1.shardName]);
}

function migrateJumboChunk(failpointOn) {
    shardCollectionCreateJumboChunk();

    // Run a manual move chunk with 'forceJumbo: true'.
    assert.commandWorked(st.s.adminCommand({
        movechunk: kDbName + ".foo",
        find: {x: 0},
        to: st.shard1.name,
        forceJumbo: true,
        waitForDelete: true
    }));
    assert.eq(st.s.getDB("config").chunks.find({ns: kDbName + ".foo"}).toArray()[0].shard,
              st.shard1.shardName);
    assert.eq(st.shard1.getDB(kDbName).foo.find().itcount(), 2000);
    assert.eq(st.shard0.getDB(kDbName).foo.find().itcount(), 0);
    assert.eq(
        st.shard1.getDB(kDbName).foo.aggregate([{$project: {i: {$range: [0, 2000]}}}]).itcount(),
        2000);

    // Add more docs that can then be split into new chunks.
    let bulk = st.s.getDB(kDbName).foo.initializeUnorderedBulkOp();
    let i = 2000;

    for (; i < 2005; i++) {
        bulk.insert({x: i, big: largeString, i: i});
    }
    for (i = 2000; i < 2005; i++) {
        assert.commandWorked(st.s.adminCommand({split: kDbName + ".foo", middle: {x: i}}));
    }

    // Now set "forceJumbo: true" in config.settings.
    assert.commandWorked(st.s.getDB("config").settings.update(
        {_id: "balancer"}, {$set: {attemptToBalanceJumboChunks: true}}, true));

    if (failpointOn) {
        assert.commandWorked(st.shard0.adminCommand(
            {configureFailPoint: 'migrateThreadHangAtStep2', mode: "alwaysOn"}));
    }

    // Let the balancer run for three runs. If the 'failTooMuchMemoryUsed' failpoint is off, the
    // jumbo chunk should be moved correctly. If it is on, the migration should fail.
    st.startBalancer();
    let numRounds = 0;
    assert.soon(() => {
        st.awaitBalancerRound();
        st.printShardingStatus(true);
        numRounds++;
        return (numRounds === 3);
    }, 'Balancer failed to run for 3 rounds', 1000 * 60 * 10);
    st.stopBalancer();

    let jumboChunk = st.getDB('config').chunks.findOne(
        {ns: 'test.foo', min: {$lte: {x: 0}}, max: {$gt: {x: 0}}});

    if (!failpointOn) {
        assert.lte(diff(), 5);
        assert.eq(st.shard0.shardName,
                  jumboChunk.shard,
                  'jumbo chunk ' + tojson(jumboChunk) + ' was not moved');
    } else {
        assert.eq(diff(), 6);
        assert.eq(st.shard1.shardName,
                  jumboChunk.shard,
                  'jumbo chunk ' + tojson(jumboChunk) + ' was moved');
        assert.commandWorked(
            st.shard0.adminCommand({configureFailPoint: 'migrateThreadHangAtStep2', mode: "off"}));
    }
}

// Test the behavior of the moveChunk command and the balancer without any failpoints set. Both the
// manual moveChunk and balancer should correctly move the jumbo chunk.
migrateJumboChunk(false);
st.s.getDB(kDbName).foo.drop();

// Turn on the 'failTooMuchMemoryUsed' failpoint, which is meant to mock the transfer mods queue
// growing to large. This will cause a migration scheduled by the balancer to fail, but not a manual
// migration.
assert.commandWorked(
    st.shard0.adminCommand({configureFailPoint: "failTooMuchMemoryUsed", mode: "alwaysOn"}));
assert.commandWorked(
    st.shard1.adminCommand({configureFailPoint: "failTooMuchMemoryUsed", mode: "alwaysOn"}));

migrateJumboChunk(true);

assert.commandWorked(
    st.shard0.adminCommand({configureFailPoint: "failTooMuchMemoryUsed", mode: "off"}));
assert.commandWorked(
    st.shard1.adminCommand({configureFailPoint: "failTooMuchMemoryUsed", mode: "off"}));

// Test that the balancer will correctly move all chunks including a jumbo chunk off of a draining
// shard.
st.s.getDB(kDbName).foo.drop();
shardCollectionCreateJumboChunk();
st.startBalancer();

// Now remove the shard that the jumbo chunk is on and make sure the chunk moves back to the other
// shard.
let res = st.s.adminCommand({removeShard: st.shard1.shardName});
assert.commandWorked(res);
assert.soon(function() {
    res = st.s.adminCommand({removeShard: st.shard1.shardName});
    assert.commandWorked(res);
    return ("completed" == res.state);
}, "failed to remove shard");

let jumboChunk =
    st.getDB('config').chunks.findOne({ns: 'test.foo', min: {$lte: {x: 0}}, max: {$gt: {x: 0}}});
assert.eq(
    st.shard0.shardName, jumboChunk.shard, 'jumbo chunk ' + tojson(jumboChunk) + ' was not moved');

st.stop();
})();
