/**
 * Verify that TTL deletes on timeseries collections are performed on
 * - the resharding source collection and copied to the resharding temporary collection.
 * - the resharded collection after resharding is complete.
 * @tags: [
 *    featureFlagReshardingForTimeseries,
 *    requires_fcv_80,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {TTLUtil} from "jstests/libs/ttl_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, ttlMonitorSleepSecs: 1});
reshardingTest.setup();

const ns = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};
const expireAfterSeconds = 5;
// Default maximum range of time for a bucket.
const defaultBucketMaxRange = 3600;
const st = reshardingTest._st;
reshardingTest.createUnshardedCollection({
    ns: ns,
    collOptions: {
        timeseries: timeseriesInfo,
        expireAfterSeconds: expireAfterSeconds,
    }
});
const timeseriesCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {'meta.x': 1},
    chunks: [
        {min: {'meta.x': MinKey}, max: {'meta.x': 0}, shard: donorShardNames[0]},
        {min: {'meta.x': 0}, max: {'meta.x': MaxKey}, shard: donorShardNames[1]},
    ],
    collOptions: {timeseries: timeseriesInfo}
});

function assertNumOfDocs(expected) {
    assert.soon(() => {
        const docs = st.s0.getCollection(ns).find({}).toArray();
        if (expected !== docs.length) {
            jsTestLog("Didn't find expected number of documents, trying again. Found: " +
                      tojson(docs));
        }
        return expected === docs.length;
    }, "Expected " + expected + " docs in the collection.");
}

function insertDocsToBeDeleted() {
    // Insert two documents in the same bucket that both are older than the TTL expiry
    // and the maximum bucket range. Expect that the TTL monitor deletes these docs.
    const maxTime = new Date((new Date()).getTime() - (1000 * defaultBucketMaxRange));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(timeseriesCollection.insert([
        {data: 3, ts: minTime, meta: {x: -2, y: -2}},
        {data: 4, ts: maxTime, meta: {x: -2, y: -2}},
    ]));
}

// Insert initial documents.
assert.commandWorked(timeseriesCollection.insert([
    {data: 1, ts: new Date(), meta: {x: -1, y: 1}},
    {data: 2, ts: new Date(), meta: {x: 1, y: -1}},
]));
assertNumOfDocs(2);

const mongos = timeseriesCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const donor1 = new Mongo(topology.shards[donorShardNames[1]].primary);
const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);

const hangTTLMonitorFP = configureFailPoint(donor0, 'hangTTLMonitorBetweenPasses');
hangTTLMonitorFP.wait();
insertDocsToBeDeleted();
assertNumOfDocs(4);

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {'meta.y': 1},
        newChunks: [
            {min: {'meta.y': MinKey}, max: {'meta.y': 0}, shard: recipientShardNames[0]},
            {min: {'meta.y': 0}, max: {'meta.y': MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        hangTTLMonitorFP.off();

        // Refresh donor shards to avoid staleConfig errors.
        assert.commandWorked(donor0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(donor1.adminCommand({_flushRoutingTableCacheUpdates: ns}));

        // Wait for TTL to delete the docs on the source
        // collection.
        TTLUtil.waitForPass(donor0.getDB('reshardingDb'));
        assertNumOfDocs(2);
    });

// Verify that donor has delete oplog entry and recipient has a corresponding applyOps entry.
const bucketNss = "reshardingDb.system.buckets.coll";
const ttlDeleteEntryDonor =
    donor0.getCollection('local.oplog.rs').find({"op": "d", "ns": bucketNss}).toArray();
assert.eq(1, ttlDeleteEntryDonor.length, ttlDeleteEntryDonor);

const ttlDeleteApplyOpsEntryRecipient =
    recipient0.getCollection('local.oplog.rs').find({"o.applyOps.op": "d"}).toArray();
assert.eq(1, ttlDeleteApplyOpsEntryRecipient.length, ttlDeleteApplyOpsEntryRecipient);
assert.eq("c", ttlDeleteApplyOpsEntryRecipient[0].op, ttlDeleteApplyOpsEntryRecipient[0]);
const applyOps = ttlDeleteApplyOpsEntryRecipient[0].o.applyOps[0];
assert(applyOps.ns.includes("reshardingDb.system.buckets.resharding"));

// Ensure TTL deletes works on the resharded collection.
const pauseTTLMonitor =
    configureFailPoint(recipient0, 'hangTTLMonitorBetweenPasses', {}, "alwaysOn");
pauseTTLMonitor.wait();
insertDocsToBeDeleted();
assertNumOfDocs(4);
pauseTTLMonitor.off();
TTLUtil.waitForPass(recipient0.getDB("reshardingDb"));
assertNumOfDocs(2);

reshardingTest.teardown();
