/*
 * Ensures that when $out is doing a rename collection operation and a concurrent 'shardCollection'
 * command is invoked, the operations are serialized. This is a targeted test to reproduce the
 * scenario described in SERVER-76626. We block the rename operation behind a DDL lock and validate
 * that a concurrent 'shardCollection' command cannot make progress.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_71,
 *   featureFlagAggOutTimeseries
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const dbName = "test";
const timeFieldName = 'time';
const metaFieldName = 'tag';
const numDocs = 40;

/* Create new sharded collection on testDB */
let _collCounter = 0;
function setUpCollection(testDB) {
    const collName = 'coll_' + _collCounter++;

    // Create a time-series collection to be the source for $out.
    testDB.createCollection(collName,
                            {timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
    const docs = [];
    for (let i = 0; i < numDocs; ++i) {
        docs.push({
            [timeFieldName]: ISODate(),
            [metaFieldName]: (1 * numDocs) + i,
        });
    }
    assert.commandWorked(testDB[collName].insertMany(docs));
    return testDB[collName];
}

function runOut(dbName, sourceCollName, targetCollName, expectCommandWorked) {
    const testDB = db.getSiblingDB(dbName);
    const cmdRes = testDB.runCommand({
        aggregate: sourceCollName,
        pipeline: [{
            $out: {
                db: testDB.getName(),
                coll: targetCollName,
                timeseries: {timeField: "time", metaField: "tag"}
            }
        }],
        cursor: {}
    });
    if (expectCommandWorked) {
        assert.commandWorked(cmdRes);
    } else {
        assert.commandFailed(cmdRes);
    }
}

function checkMetadata(testDB) {
    const checkOptions = {'checkIndexes': 1};
    let inconsistencies = testDB.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(0, inconsistencies, inconsistencies);
}

function runOutAndShardCollectionConcurrently_shardCollectionMustFail(st, testDB, primaryShard) {
    // The target collection should exist to produce the metadata inconsistency scenario.
    const sourceColl = setUpCollection(testDB);
    const targetColl = setUpCollection(testDB);

    // Set a failpoint in the internalRenameCollection command after the sharding check.
    const fp = configureFailPoint(primaryShard, 'blockBeforeInternalRenameAndAfterTakingDDLLocks');

    // Run an $out aggregation pipeline in a parallel shell.
    let outShell = startParallelShell(funWithArgs(runOut,
                                                  testDB.getName(),
                                                  sourceColl.getName(),
                                                  targetColl.getName(),
                                                  true /*expectCommandWorked*/),
                                      st.s.port);

    // Wait for the aggregation pipeline to hit the failpoint.
    fp.wait();

    // Validate the temporary collection exists, meaning we are in the middle of the $out stage.
    let collNames = testDB.getCollectionNames();
    assert.eq(collNames.filter(col => col.startsWith('tmp.agg_out')).length, 1, collNames);

    // Assert sharding the target collection fails, since the rename command has a lock on the
    // view namespace.
    jsTestLog("attempting to shard the target collection.");
    assert.commandFailedWithCode(
        testDB.adminCommand({shardCollection: targetColl.getFullName(), key: {[metaFieldName]: 1}}),
        ErrorCodes.LockBusy);

    // Turn off the failpoint and resume the $out aggregation pipeline.
    jsTestLog("turning the failpoint off.");
    fp.off();
    outShell();
    // Assert the metadata is consistent.
    checkMetadata(testDB);

    // Assert sharding the target collection succeeds, since there is no lock on the view
    // namespace.
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: targetColl.getFullName(), key: {[metaFieldName]: 1}}));

    // Assert the metadata is consistent.
    checkMetadata(testDB);

    // Assert no temporary collection is left over.
    collNames = testDB.getCollectionNames();
    assert.eq(collNames.filter(col => col.startsWith('tmp.agg_out')).length, 0, collNames);

    sourceColl.drop();
    targetColl.drop();
}

function runOutAndShardCollectionConcurrently_OutMustFail(st, testDB, primaryShard) {
    // The target collection should exist to produce the metadata inconsistency scenario.
    const sourceColl = setUpCollection(testDB);
    const targetColl = setUpCollection(testDB);

    // Set a failpoint in the internalRenameCollection command after the sharding check.
    const fp = configureFailPoint(primaryShard, 'blockBeforeInternalRenameAndBeforeTakingDDLLocks');

    // Run an $out aggregation pipeline in a parallel shell.
    let outShell = startParallelShell(funWithArgs(runOut,
                                                  testDB.getName(),
                                                  sourceColl.getName(),
                                                  targetColl.getName(),
                                                  false /*expectCommandWorked*/),
                                      st.s.port);

    // Wait for the aggregation pipeline to hit the failpoint.
    fp.wait();

    // Validate the temporary collection exists, meaning we are in the middle of the $out stage.
    let collNames = testDB.getCollectionNames();
    assert.eq(collNames.filter(col => col.startsWith('tmp.agg_out')).length, 1, collNames);

    // Assert sharding the target collection fails, since the rename command has a lock on the
    // view namespace.
    jsTestLog("attempting to shard the target collection.");
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: targetColl.getFullName(), key: {[metaFieldName]: 1}}));

    // Turn off the failpoint and resume the $out aggregation pipeline.
    jsTestLog("turning the failpoint off.");
    fp.off();
    outShell();

    // Assert the metadata is consistent.
    checkMetadata(testDB);

    // Assert to temp collection is left over.
    collNames = testDB.getCollectionNames();
    assert.eq(collNames.filter(col => col.startsWith('tmp.agg_out')).length, 0, collNames);

    sourceColl.drop();
    targetColl.drop();
}

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB(dbName);
const primaryShard = st.shard0;

// Reduce DDL lock timeout to half a second to speedup testing command that are expected to fail
// with LockBusy error
const fp = configureFailPoint(primaryShard, "overrideDDLLockTimeout", {'timeoutMillisecs': 500});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}));

// Running tests
runOutAndShardCollectionConcurrently_shardCollectionMustFail(st, testDB, primaryShard);
runOutAndShardCollectionConcurrently_OutMustFail(st, testDB, primaryShard);

fp.off();

st.stop();
