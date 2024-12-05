/**
 * Tests that writes are disallowed while in kCommitted for timeseries resharded collections.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: false});
reshardingTest.setup();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};

const donorShardNames = reshardingTest.donorShardNames;

const timeseriesCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {'meta.x': 1},
    chunks: [
        {min: {'meta.x': MinKey}, max: {'meta.x': 0}, shard: donorShardNames[0]},
        {min: {'meta.x': 0}, max: {'meta.x': MaxKey}, shard: donorShardNames[1]},
    ],
    collOptions: {
        timeseries: timeseriesInfo,
    }
});

assert.commandWorked(
    timeseriesCollection.insert({data: 1, ts: new Date(), meta: {x: -1, y: -1, z: 0}}));
assert.commandWorked(timeseriesCollection.createIndexes(
    [{indexToDropDuringResharding: 1}, {indexToDropAfterResharding: 1}]));

const bucketsCollName = "system.buckets.foo";
const bucketsColl = reshardingTest._st.s.getDB("test").getCollection('system.buckets.foo');

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {'meta.y': 1},
        newChunks: [
            {min: {'meta.y': MinKey}, max: {'meta.y': 0}, shard: recipientShardNames[0]},
            {min: {'meta.y': 0}, max: {'meta.y': MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {},
    {
        postCheckConsistencyFn: () => {
            jsTestLog("Attempting insert");
            let res = timeseriesCollection.runCommand({
                insert: collName,
                documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = bucketsColl.runCommand({
                insert: bucketsCollName,
                documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting update");
            res = timeseriesCollection.runCommand({
                update: collName,
                updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = bucketsColl.runCommand({
                update: bucketsCollName,
                updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting delete");
            res = timeseriesCollection.runCommand(
                {delete: collName, deletes: [{q: {'meta.x': -1}, limit: 1}], maxTimeMS: 3000});
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = bucketsColl.runCommand({
                delete: bucketsCollName,
                deletes: [{q: {'meta.x': -1}, limit: 1}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting createIndex");
            res = timeseriesCollection.runCommand({
                createIndexes: collName,
                indexes: [{key: {'meta.z': 1}, name: "metaz_0"}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting collMod");
            // The collMod is serialized with the resharding command, so we explicitly add an
            // timeout to the command so that it doesn't get blocked and timeout the test.
            res = timeseriesCollection.runCommand({collMod: collName, maxTimeMS: 3000});
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting drop index");
            res = timeseriesCollection.runCommand(
                {dropIndexes: collName, index: {indexToDropDuringResharding: 1}, maxTimeMS: 3000});
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            assert.soon(() => {
                let ops = reshardingTest._st.s.getDB('admin')
                              .aggregate([
                                  {$currentOp: {}},
                                  {$match: {"command._shardsvrDropIndexes": collName}}
                              ])
                              .toArray();
                return ops.length == 0;
            });

            jsTestLog("Completed operations");
        },
        afterReshardingFn: () => {
            jsTestLog("Join possible ongoing collMod command");
            assert.commandWorked(timeseriesCollection.runCommand("collMod"));
        }
    });

jsTestLog("Verify that writes succeed after resharding operation has completed");

assert.commandWorked(timeseriesCollection.runCommand(
    {insert: collName, documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}]}));

assert.commandWorked(timeseriesCollection.runCommand(
    {update: collName, updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}]}));

assert.commandWorked(
    timeseriesCollection.runCommand({delete: collName, deletes: [{q: {'meta.x': -1}, limit: 1}]}));

assert.commandWorked(timeseriesCollection.runCommand(
    {createIndexes: collName, indexes: [{key: {'meta.z': 1}, name: "metaz_0"}]}));

assert.commandWorked(timeseriesCollection.runCommand({collMod: collName}));

assert.commandWorked(timeseriesCollection.runCommand(
    {dropIndexes: collName, index: {indexToDropDuringResharding: 1}}));

reshardingTest.teardown();
