/**
 * Tests the timeseries resharding recipient shards handles config.transactions entries from the
 * source shards.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 3, numRecipients: 3, reshardInPlace: true});

reshardingTest.setup();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {'meta.x': 1},
    chunks: [
        {min: {'meta.x': MinKey}, max: {'meta.x': 0}, shard: donorShardNames[0]},
        {min: {'meta.x': 0}, max: {'meta.x': 100}, shard: donorShardNames[1]},
        {min: {'meta.x': 100}, max: {'meta.x': MaxKey}, shard: donorShardNames[2]},
    ],
    collOptions: {
        timeseries: timeseriesInfo,
    }
});

let lsidList = [];
lsidList.push(UUID());
lsidList.push(UUID());
lsidList.push(UUID());

let execRetryableInsert = function(lsid, docs) {
    return inputCollection.getDB('test').runCommand({
        insert: 'foo',
        documents: docs,
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(1),
    });
};

assert.commandWorked(execRetryableInsert(lsidList[0], [
    {data: 3, ts: new Date(), 'meta.x': -10, 'meta.y': 0},
    {data: 2, ts: new Date(), 'meta.x': 10, 'meta.y': -19}
]));
assert.commandWorked(execRetryableInsert(lsidList[1], [
    {data: -9, ts: new Date(), 'meta.x': 0, 'meta.y': 100},
    {data: 0, ts: new Date(), 'meta.x': 105, 'meta.y': -16}
]));
assert.commandWorked(
    execRetryableInsert(lsidList[2], [{data: 16, ts: new Date(), 'meta.x': 100, 'meta.y': -10}]));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {'meta.y': 1},
    newChunks: [
        {min: {'meta.y': MinKey}, max: {'meta.y': 0}, shard: recipientShardNames[1]},
        {min: {'meta.y': 0}, max: {'meta.y': 100}, shard: recipientShardNames[2]},
        {min: {'meta.y': 100}, max: {'meta.y': MaxKey}, shard: recipientShardNames[0]},
    ],
});

// If we don't refresh mongos, writes will be targetted based on the chunk distribution before
// resharding. Even though the shard versions don't match, it will not cause a stale config
// exception because the write was already executed on the shard being targetted, resulting in a
// no-op write, and thus, no shard version checking. This behavior is not wrong, but since we
// want to test the retry behavior after resharding, we force the mongos to refresh.
const mongos = inputCollection.getMongo();
assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));

assert.commandFailedWithCode(
    execRetryableInsert(lsidList[0], [{data: 2, ts: new Date(), 'meta.x': 10, 'meta.y': -19}]),
    ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(
    execRetryableInsert(lsidList[0], [{data: 3, ts: new Date(), 'meta.x': -10, 'meta.y': 0}]),
    ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(
    execRetryableInsert(lsidList[1],
                        [
                            {data: -9, ts: new Date(), 'meta.x': 0, 'meta.y': 100},
                            {data: 0, ts: new Date(), 'meta.x': 105, 'meta.y': -16}
                        ]),
    ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(
    execRetryableInsert(lsidList[2], [{data: 16, ts: new Date(), 'meta.x': 100, 'meta.y': -10}]),
    ErrorCodes.IncompleteTransactionHistory);

reshardingTest.teardown();
