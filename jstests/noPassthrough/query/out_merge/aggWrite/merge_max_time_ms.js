/**
 * Test that an aggregation with a $merge stage obeys the maxTimeMS.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {withEachMergeMode} from "jstests/aggregation/extras/merge_helpers.js";
import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDBName = "test";
const kSourceCollName = "merge_max_time_ms_source";
const kDestCollName = "merge_max_time_ms_dest";
const nDocs = 10;

/**
 * Helper for populating the collection.
 */
function insertDocs(coll) {
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i}, {writeConcern: {w: "majority"}}));
    }
}

/**
 * Given a $merge parameters mongod connection, run a $merge aggregation against 'conn'. Set the
 * provided failpoint on the node specified by 'failPointConn' in order to hang during the
 * aggregate. Ensure that the $merge maxTimeMS expires on the node specified by 'maxTimeMsConn'.
 */
function forceAggregationToHangAndCheckMaxTimeMsExpires(
    whenMatched,
    whenNotMatched,
    failPointName,
    conn,
    failPointConn,
    maxTimeMsConn,
) {
    // Use a short maxTimeMS so that the test completes in a reasonable amount of time. We will
    // use the 'maxTimeNeverTimeOut' failpoint to ensure that the operation does not
    // prematurely time out.
    const maxTimeMS = 1000 * 2;

    // Enable a failPoint so that the write will hang. 'shouldCheckForInterrupt' is set to true
    // so that maxTimeMS expiration can occur while the $merge operation's thread is hanging on
    // this failpoiint.
    const failpointCommand = {
        configureFailPoint: failPointName,
        mode: "alwaysOn",
        data: {nss: kDBName + "." + kDestCollName, shouldCheckForInterrupt: true},
    };

    assert.commandWorked(failPointConn.getDB("admin").runCommand(failpointCommand));

    // Make sure we don't run out of time on either of the involved nodes before the failpoint is
    // hit.
    assert.commandWorked(conn.getDB("admin").runCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));
    assert.commandWorked(
        maxTimeMsConn.getDB("admin").runCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}),
    );

    // Build the parallel shell function.
    let shellStr = `const testDB = db.getSiblingDB('${kDBName}');`;
    shellStr += `const sourceColl = testDB['${kSourceCollName}'];`;
    shellStr += `const destColl = testDB['${kDestCollName}'];`;
    shellStr += `const maxTimeMS = ${maxTimeMS};`;
    shellStr += `const whenMatched = ${tojson(whenMatched)};`;
    shellStr += `const whenNotMatched = '${whenNotMatched}';`;
    /* eslint-disable */
    const runAggregate = function () {
        const pipeline = [
            {
                $merge: {into: destColl.getName(), whenMatched: whenMatched, whenNotMatched: whenNotMatched},
            },
        ];
        const err = assert.throws(() =>
            sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS, $readPreference: {mode: "secondary"}}),
        );
        assert.eq(err.code, ErrorCodes.MaxTimeMSExpired, "expected aggregation to fail");
    };
    /* eslint-enable */
    shellStr += `(${runAggregate.toString()})();`;
    const awaitShell = startParallelShell(shellStr, conn.port);

    waitForCurOpByFailPointNoNS(failPointConn.getDB("admin"), failPointName, {}, {allUsers: true});

    assert.commandWorked(
        maxTimeMsConn.getDB("admin").runCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}),
    );

    // The aggregation running in the parallel shell will hang on the failpoint, burning
    // its time. Wait until the maxTimeMS has definitely expired.
    sleep(maxTimeMS + 2000);

    // Now drop the failpoint, allowing the aggregation to proceed. It should hit an
    // interrupt check and terminate immediately.
    assert.commandWorked(failPointConn.getDB("admin").runCommand({configureFailPoint: failPointName, mode: "off"}));

    // Wait for the parallel shell to finish.
    assert.eq(awaitShell(), 0);
}

/**
 * Run a $merge aggregate against the node specified by 'conn' with primary 'primaryConn' (these may
 * be the same node). Verify that maxTimeMS properly times out the aggregate on the node specified
 * by 'maxTimeMsConn' both while hanging on the insert/update on 'primaryConn' and while hanging on
 * the batch being built on 'conn'.
 */
function runUnshardedTest(whenMatched, whenNotMatched, conn, primaryConn, maxTimeMsConn) {
    jsTestLog("Running unsharded test in whenMatched: " + whenMatched + " whenNotMatched: " + whenNotMatched);
    // The target collection will always be empty so we do not test the setting that will cause
    // only failure.
    if (whenNotMatched == "fail") {
        return;
    }

    const sourceColl = conn.getDB(kDBName)[kSourceCollName];
    const destColl = primaryConn.getDB(kDBName)[kDestCollName];
    assert.commandWorked(destColl.remove({}));

    // Be sure we're able to read from a cursor with a maxTimeMS set on it.
    (function () {
        // Use a long maxTimeMS, since we expect the operation to finish.
        const maxTimeMS = 1000 * 600;
        const pipeline = [
            {
                $merge: {into: destColl.getName(), whenMatched: whenMatched, whenNotMatched: whenNotMatched},
            },
        ];
        assert.doesNotThrow(() => sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS}));
    })();

    assert.commandWorked(destColl.remove({}));

    // Force the aggregation to hang while the batch is being written. The failpoint changes
    // depending on the mode. If 'whenMatched' is set to "fail" then the implementation will end
    // up issuing insert commands instead of updates.
    const kFailPointName = whenMatched === "fail" ? "hangDuringBatchInsert" : "hangDuringBatchUpdate";
    forceAggregationToHangAndCheckMaxTimeMsExpires(
        whenMatched,
        whenNotMatched,
        kFailPointName,
        conn,
        primaryConn,
        maxTimeMsConn,
    );

    assert.commandWorked(destColl.remove({}));

    // Force the aggregation to hang while the batch is being built.
    forceAggregationToHangAndCheckMaxTimeMsExpires(
        whenMatched,
        whenNotMatched,
        "hangWhileBuildingDocumentSourceMergeBatch",
        conn,
        conn,
        conn,
    );
}

// Run on a standalone.
(function () {
    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    insertDocs(conn.getDB(kDBName)[kSourceCollName]);
    withEachMergeMode((mode) => runUnshardedTest(mode.whenMatchedMode, mode.whenNotMatchedMode, conn, conn, conn));
    MongoRunner.stopMongod(conn);
})();

// Run on the primary and the secondary of a replica set.
(function () {
    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();
    const primary = replTest.getPrimary();
    const secondary = replTest.getSecondary();
    insertDocs(primary.getDB(kDBName)[kSourceCollName]);
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Run the $merge on the primary and test that the maxTimeMS times out on the primary.
        runUnshardedTest(whenMatchedMode, whenNotMatchedMode, primary, primary, primary);
        // Run the $merge on the secondary and test that the maxTimeMS times out on the primary.
        runUnshardedTest(whenMatchedMode, whenNotMatchedMode, secondary, primary, primary);
        // Run the $merge on the secondary and test that the maxTimeMS times out on the secondary.
        runUnshardedTest(whenMatchedMode, whenNotMatchedMode, secondary, primary, secondary);
    });
    replTest.stopSet();
})();

// Runs a $merge against 'mongosConn' and verifies that the maxTimeMS value is included in the
// command sent to mongod. Since the actual timeout can unreliably happen in mongos before even
// reaching the shard, we instead set a very large timeout and verify that the command sent to
// mongod includes the maxTimeMS.
function runShardedTest(whenMatched, whenNotMatched, mongosConn, mongodConn, comment) {
    jsTestLog("Running sharded test in whenMatched: " + whenMatched + " whenNotMatched: " + whenNotMatched);
    // The target collection will always be empty so we do not test the setting that will cause
    // only failure.
    if (whenNotMatched == "fail") {
        return;
    }

    // Set a large timeout since we expect the command to finish.
    const maxTimeMS = 1000 * 20;

    const sourceColl = mongosConn.getDB(kDBName)[kSourceCollName];
    const destColl = mongosConn.getDB(kDBName)[kDestCollName];
    assert.commandWorked(destColl.remove({}));

    // Make sure we don't timeout in mongos before even reaching the shards.
    assert.commandWorked(
        mongosConn.getDB("admin").runCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}),
    );

    const cursor = sourceColl.aggregate(
        [
            {
                $merge: {into: destColl.getName(), whenMatched: whenMatched, whenNotMatched: whenNotMatched},
            },
        ],
        {maxTimeMS: maxTimeMS, comment: comment},
    );
    assert(!cursor.hasNext());

    // Filter the profiler entries on the existence of $merge, since aggregations through mongos
    // will include an extra aggregation with an empty pipeline to establish cursors on the
    // shards.
    assert.soon(function () {
        return (
            mongodConn
                .getDB(kDBName)
                .system.profile.find({
                    "command.aggregate": kSourceCollName,
                    "command.pipeline.$merge": {"$exists": true},
                    "command.comment": comment,
                    "command.maxTimeMS": maxTimeMS,
                })
                .itcount() == 1
        );
    });

    assert.commandWorked(
        mongosConn.getDB("admin").runCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}),
    );
}

// Run on a sharded cluster.
(function () {
    const st = new ShardingTest({shards: 2});

    // Ensure shard 0 is the primary shard. This is so that the $merge stage is guaranteed to
    // run on it.
    assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: kDBName, primaryShard: st.shard0.name}));

    // Set up the source collection to be sharded in a way such that each node will have some
    // documents for the remainder of the test.
    // shard 0: [MinKey, 5]
    // shard 1: [5, MaxKey]
    st.shardColl(
        kSourceCollName,
        {_id: 1}, // key
        {_id: 5}, // split
        {_id: 6}, // move
        kDBName,
    );
    insertDocs(st.s.getDB(kDBName)[kSourceCollName]);

    // Start the profiler on each shard so that we can examine the $out's maxTimeMS.
    assert.commandWorked(st.shard0.getDB(kDBName).setProfilingLevel(2));
    assert.commandWorked(st.shard1.getDB(kDBName).setProfilingLevel(2));

    // // Run the test with 'destColl' unsharded.
    withEachMergeMode((mode) =>
        runShardedTest(mode.whenMatchedMode, mode.whenNotMatchedMode, st.s, st.shard0, tojson(mode) + "_unshardedDest"),
    );

    // Run the test with 'destColl' sharded. This means that writes will be sent to both
    // shards, and if either one hangs, the MaxTimeMS will expire.
    // Shard the destination collection.
    st.shardColl(
        kDestCollName,
        {_id: 1}, // key
        {_id: 5}, // split
        {_id: 6}, // move
        kDBName,
    );

    jsTestLog("Running test forcing shard " + st.shard0.name + " to hang");
    withEachMergeMode((mode) =>
        runShardedTest(
            mode.whenMatchedMode,
            mode.whenNotMatchedMode,
            st.s,
            st.shard0,
            tojson(mode) + "_shardedDest_" + st.shard0.name,
        ),
    );

    jsTestLog("Running test forcing shard " + st.shard1.name + " to hang");
    withEachMergeMode((mode) =>
        runShardedTest(
            mode.whenMatchedMode,
            mode.whenNotMatchedMode,
            st.s,
            st.shard1,
            tojson(mode) + "_shardedDest_" + st.shard1.name,
        ),
    );

    st.stop();
})();
