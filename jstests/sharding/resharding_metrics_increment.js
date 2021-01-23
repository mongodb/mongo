/**
 * Tests that resharding metrics section in server status
 * responds to statistical increments in an expected way.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const kNamespace = "reshardingDb.coll";

// All of the keys in `expected` must be present in `dict`.
// Furthermore, if the the `expected` key is mapped to a value other than `undefined`,
// then the corresponding value in `dict` must be equal to that value.
function verifyDict(dict, expected) {
    for (var key in expected) {
        assert(dict.hasOwnProperty(key), `Missing ${key} in ${tojson(dict)}`);
        const expectedValue = expected[key];
        if (expected[key] === undefined) {
            jsTest.log(`${key}: ${tojson(dict[key])}`);
            continue;
        } else {
            assert.eq(dict[key],
                      expected[key],
                      `Expected the value for ${key} to be ${expectedValue}: ${tojson(dict)}`);
        }
    }
}

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const inputCollection = reshardingTest.createShardedCollection({
    ns: kNamespace,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

assert.commandWorked(inputCollection.insert([
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]));

function awaitEstablishmentOfFetchTimestamp(inputCollection) {
    const mongos = inputCollection.getMongo();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
            nss: inputCollection.getFullName()
        });
        return coordinatorDoc !== null && coordinatorDoc.fetchTimestamp !== undefined;
    });
}

// Start the resharding operation, and wait for it to establish a fetch
// timestamp, which indicates the beginning of the apply phase. Then perform a
// few late inserts to verify that those show up as both "fetched" and
// "applied" in the metrics.
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        jsTest.log(`tempNs: ${tempNs}`);
        awaitEstablishmentOfFetchTimestamp(inputCollection);
        for (var i = 0; i < 10; ++i)
            assert.commandWorked(
                inputCollection.insert({_id: `late ${i}`, oldKey: 10, newKey: 10}));
    });

const mongos = inputCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

// There's one terminating "nop" oplog entry from each donor marking the
// boundary between the cloning phase and the applying phase. So there's a
// baseline of 2 fetches on each recipient (one "nop" for each donor).
// Additionally, recipientShard[1] gets the 10 late inserts above, so expect 12
// total fetches on that shard, and obviously the 10 corresponding
// oplogEntry applies for those late inserts.
[{shardName: recipientShardNames[0], fetched: 2, applied: 0},
 {shardName: recipientShardNames[1], fetched: 12, applied: 10},
].forEach(e => {
    const mongo = new Mongo(topology.shards[e.shardName].primary);
    const doc = mongo.getDB('admin').serverStatus({});

    var sub = doc;
    ['shardingStatistics', 'resharding'].forEach(k => {
        assert(sub.hasOwnProperty(k), sub);
        sub = sub[k];
    });

    jsTest.log(`Resharding stats for ${mongo}: ${tojson(sub)}`);

    verifyDict(sub, {
        "oplogEntriesFetched": e.fetched,
        "oplogEntriesApplied": e.applied,
    });
});

reshardingTest.teardown();
})();
