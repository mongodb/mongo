/**
 * Tests that writes are disallowed while in kCommitted.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagMoveCollection,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createUnshardedCollection({ns: ns, primaryShardName: donorShardNames[0]});

assert.commandWorked(sourceCollection.insert({_id: 0, yak: 50}));
assert.commandWorked(
    sourceCollection.createIndexes([{indexToDropDuringResharding: 1}, {indexToDropAfterResharding: 1}]),
);

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withMoveCollectionInBackground(
    {
        toShard: recipientShardNames[0],
    },
    () => {},
    {
        postCheckConsistencyFn: () => {
            jsTestLog("Attempting insert");
            let res = sourceCollection.runCommand({insert: collName, documents: [{_id: 1}], maxTimeMS: 5000});
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting update");
            res = sourceCollection.runCommand({
                update: collName,
                updates: [{q: {_id: 0}, u: {$set: {yak: 15}}}],
                maxTimeMS: 5000,
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting delete");
            res = sourceCollection.runCommand({delete: collName, deletes: [{q: {_id: 0}, limit: 1}], maxTimeMS: 5000});
            assert(ErrorCodes.isExceededTimeLimitError(res.writeErrors[0].code));

            jsTestLog("Attempting createIndex");
            res = sourceCollection.runCommand({
                createIndexes: collName,
                indexes: [{key: {yak: 1}, name: "yak_0"}],
                maxTimeMS: 5000,
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting collMod");
            // The collMod is serialized with the resharding command, so we explicitly add an
            // timeout to the command so that it doesn't get blocked and timeout the test.
            res = sourceCollection.runCommand({collMod: sourceCollection.getName(), maxTimeMS: 5000});
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Attempting drop index");
            res = sourceCollection.runCommand({
                dropIndexes: collName,
                index: {indexToDropDuringResharding: 1},
                maxTimeMS: 5000,
            });
            assert(ErrorCodes.isExceededTimeLimitError(res.code));

            jsTestLog("Completed operations");
        },
        afterReshardingFn: () => {
            // The ShardingDDLCoordinator will automatically retry any retriable DDLs, and
            // ExceededTimeLimit is considered retriable. If a DDL fails with this code, it will be
            // queued to rerun once the resharding releases the locks. We need to join any
            // in-progress retries and accept IndexNotFound for dropIndexes since the index may have
            // already been dropped by the retry.

            jsTestLog("Join possible ongoing collMod and dropIndexes commands");
            assert.commandWorked(sourceCollection.runCommand("collMod"));

            // TODO SERVER-107420: Remove IndexNotFound from acceptable dropIndexes errors once 9.0
            // becomes LTS
            assert.commandWorkedOrFailedWithCode(
                sourceCollection.runCommand({
                    dropIndexes: sourceCollection.getName(),
                    index: {indexToDropDuringResharding: 1},
                }),
                ErrorCodes.IndexNotFound,
            );
        },
    },
);

jsTestLog("Verify that writes succeed after resharding operation has completed");

assert.commandWorked(sourceCollection.runCommand({insert: collName, documents: [{_id: 1}]}));

assert.commandWorked(sourceCollection.runCommand({update: collName, updates: [{q: {_id: 0}, u: {$set: {extra: 15}}}]}));

assert.commandWorked(sourceCollection.runCommand({delete: collName, deletes: [{q: {_id: 0}, limit: 1}]}));

assert.commandWorked(sourceCollection.runCommand({createIndexes: collName, indexes: [{key: {yak: 1}, name: "yak_0"}]}));

assert.commandWorked(sourceCollection.runCommand({collMod: sourceCollection.getName()}));

assert.commandWorked(sourceCollection.runCommand({dropIndexes: collName, index: {indexToDropAfterResharding: 1}}));

assert.commandWorked(sourceCollection.runCommand({drop: collName}));

reshardingTest.teardown();
