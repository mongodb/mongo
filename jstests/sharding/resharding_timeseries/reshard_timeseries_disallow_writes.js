/**
 * Tests that writes are disallowed while in kCommitted for timeseries resharded collections.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: false});
reshardingTest.setup();

const ns = "test.foo";

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};

const donorShardNames = reshardingTest.donorShardNames;

const coll = reshardingTest.createShardedCollection({
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
const db = coll.getDB();

assert.commandWorked(coll.insert({data: 1, ts: new Date(), meta: {x: -1, y: -1, z: 0}}));
assert.commandWorked(
    coll.createIndexes([{indexToDropDuringResharding: 1}, {indexToDropAfterResharding: 1}]));

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
            let res = coll.runCommand({
                insert: coll.getName(),
                documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = getTimeseriesCollForRawOps(db, coll).runCommand({
                insert: getTimeseriesCollForRawOps(db, coll).getName(),
                documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}],
                maxTimeMS: 3000,
                ...getRawOperationSpec(db)
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting update");
            res = coll.runCommand({
                update: coll.getName(),
                updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = getTimeseriesCollForRawOps(db, coll).runCommand({
                update: getTimeseriesCollForRawOps(db, coll).getName(),
                updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}],
                maxTimeMS: 3000,
                ...getRawOperationSpec(db)
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting delete");
            res = coll.runCommand({
                delete: coll.getName(),
                deletes: [{q: {'meta.x': -1}, limit: 1}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            res = getTimeseriesCollForRawOps(db, coll).runCommand({
                delete: getTimeseriesCollForRawOps(db, coll).getName(),
                deletes: [{q: {'meta.x': -1}, limit: 1}],
                maxTimeMS: 3000,
                ...getRawOperationSpec(db)
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting createIndex");
            res = coll.runCommand({
                createIndexes: coll.getName(),
                indexes: [{key: {'meta.z': 1}, name: "metaz_0"}],
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting collMod");
            // The collMod is serialized with the resharding command, so we explicitly add an
            // timeout to the command so that it doesn't get blocked and timeout the test.
            res = coll.runCommand({collMod: coll.getName(), maxTimeMS: 3000});
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting drop index");
            res = coll.runCommand({
                dropIndexes: coll.getName(),
                index: {indexToDropDuringResharding: 1},
                maxTimeMS: 3000
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            assert.soon(() => {
                let ops = reshardingTest._st.s.getDB('admin')
                              .aggregate([
                                  {$currentOp: {}},
                                  {$match: {"command._shardsvrDropIndexes": coll.getName()}}
                              ])
                              .toArray();
                return ops.length == 0;
            });

            jsTestLog("Completed operations");
        },
        afterReshardingFn: () => {
            jsTestLog("Join possible ongoing collMod command");
            assert.commandWorked(coll.runCommand("collMod"));
        }
    });

jsTestLog("Verify that writes succeed after resharding operation has completed");

assert.commandWorked(coll.runCommand(
    {insert: coll.getName(), documents: [{data: 3, ts: new Date(), meta: {x: -2, y: -2}}]}));

assert.commandWorked(coll.runCommand({
    update: coll.getName(),
    updates: [{q: {'meta.x': -1}, u: {$set: {'meta.x': -15}}, multi: true}]
}));

assert.commandWorked(
    coll.runCommand({delete: coll.getName(), deletes: [{q: {'meta.x': -1}, limit: 1}]}));

assert.commandWorked(coll.runCommand(
    {createIndexes: coll.getName(), indexes: [{key: {'meta.z': 1}, name: "metaz_0"}]}));

assert.commandWorked(coll.runCommand({collMod: coll.getName()}));

assert.commandWorked(
    coll.runCommand({dropIndexes: coll.getName(), index: {indexToDropDuringResharding: 1}}));

reshardingTest.teardown();
