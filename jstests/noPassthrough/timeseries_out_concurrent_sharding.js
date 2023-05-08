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
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // for configureFailPoint.

const st = new ShardingTest({shards: 2});
const dbName = "test";
const testDB = st.s.getDB(dbName);
const targetCollName = "out_time";
const sourceCollName = "in";
const timeFieldName = 'time';
const metaFieldName = 'tag';
const numDocs = 40;

function setUpCollection(collName) {
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
}

function runOutQuery() {
    assert.doesNotThrow(() => testDB[sourceCollName].aggregate([
        {$set: {"time": new Date()}},
        {
            $out: {
                db: testDB.getName(),
                coll: targetCollName,
                timeseries: {timeField: timeFieldName, metaField: metaFieldName}
            }
        }
    ]));
}

function checkMetadata() {
    const checkOptions = {'checkIndexes': 1};
    let inconsistencies = testDB.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(0, inconsistencies, inconsistencies);
}

function runParallelShellTest() {
    // Set a failpoint in the internalRenameCollection command after the sharding check.
    const fp = configureFailPoint(st.shard0, 'blockBeforeInternalRenameIfOptionsAndIndexesMatch');

    // Run an $out aggregation pipeline in a parallel shell.
    let parallelFunction =
        `let cmdRes = db.runCommand({
        aggregate: "in",
        pipeline: [
            {
                $out: {
                    db: db.getName(),
                    coll: "out_time",
                    timeseries: {timeField: "time", metaField: "tag"}
                }
            }
        ], cursor: {}})
        assert(cmdRes.ok);`;
    let outShell = startParallelShell(parallelFunction, st.s.port);

    // Wait for the aggregation pipeline to hit the failpoint.
    fp.wait();

    // Validate the temporary collection exists, meaning we are in the middle of the $out stage.
    const collNames = testDB.getCollectionNames();
    assert.eq(collNames.filter(col => col.includes('tmp.agg_out')).length, 1, collNames);

    // Assert sharding the target collection fails, since the rename command has a lock on the
    // view namespace.
    jsTestLog("attempting to shard the target collection.");
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {shardCollection: testDB[targetCollName].getFullName(), key: {[metaFieldName]: 1}}),
        ErrorCodes.LockBusy);

    // Turn off the failpoint and resume the $out aggregation pipeline.
    jsTestLog("turning the failpoint off.");
    fp.off();
    outShell();
    // Assert the metadata is consistent.
    checkMetadata();

    // Assert sharding the target collection succeeds, since there is no lock on the view
    // namespace.
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: testDB[targetCollName].getFullName(), key: {[metaFieldName]: 1}}));

    // Assert the metadata is consistent.
    checkMetadata();
}

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// The target collection should exist to produce the metadata inconsistency scenario.
setUpCollection(sourceCollName);
setUpCollection(targetCollName);

runParallelShellTest();

st.stop();
}());
