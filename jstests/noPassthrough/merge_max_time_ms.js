/**
 * Test that an aggregation with a $merge stage obeys the maxTimeMS.
 * @tags: [requires_sharding, requires_replication]
 */
(function() {
    load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode().
    load("jstests/libs/fixture_helpers.js");              // For isMongos().
    load("jstests/libs/profiler.js");  // For profilerHasSingleMatchingEntryOrThrow.

    const kDBName = "test";
    const kSourceCollName = "merge_max_time_ms_source";
    const kDestCollName = "merge_max_time_ms_dest";
    const nDocs = 10;

    /**
     * Helper for populating the collection.
     */
    function insertDocs(coll) {
        for (let i = 0; i < nDocs; i++) {
            assert.commandWorked(coll.insert({_id: i}));
        }
    }

    /**
     * Wait until the server sets its CurOp "msg" to the failpoint name, indicating that it's
     * hanging.
     */
    function waitUntilServerHangsOnFailPoint(conn, fpName) {
        // Be sure that the server is hanging on the failpoint.
        assert.soon(function() {
            const filter = {"msg": fpName};
            const ops = conn.getDB("admin")
                            .aggregate([{$currentOp: {allUsers: true}}, {$match: filter}])
                            .toArray();
            return ops.length == 1;
        });
    }

    /**
     * Given a $merge parameters mongod connection, run a $out aggregation against 'conn' which
     * hangs on the given failpoint and ensure that the $out maxTimeMS expires.
     */
    function forceAggregationToHangAndCheckMaxTimeMsExpires(
        whenMatched, whenNotMatched, conn, failPointName) {
        // Use a short maxTimeMS so that the test completes in a reasonable amount of time. We will
        // use the 'maxTimeNeverTimeOut' failpoint to ensure that the operation does not
        // prematurely time out.
        const maxTimeMS = 1000 * 2;

        // Enable a failPoint so that the write will hang.
        let failpointCommand = {
            configureFailPoint: failPointName,
            mode: "alwaysOn",
            data: {nss: kDBName + "." + kDestCollName}
        };

        assert.commandWorked(conn.getDB("admin").runCommand(failpointCommand));

        // Make sure we don't run out of time before the failpoint is hit.
        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));

        // Build the parallel shell function.
        let shellStr = `const sourceColl = db['${kSourceCollName}'];`;
        shellStr += `const destColl = db['${kDestCollName}'];`;
        shellStr += `const maxTimeMS = ${maxTimeMS};`;
        shellStr += `const whenMatched = ${tojson(whenMatched)};`;
        shellStr += `const whenNotMatched = '${whenNotMatched}';`;
        const runAggregate = function() {
            const pipeline = [{
                $merge: {
                    into: destColl.getName(),
                    whenMatched: whenMatched,
                    whenNotMatched: whenNotMatched
                }
            }];
            const err = assert.throws(() => sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS}));
            assert.eq(err.code, ErrorCodes.MaxTimeMSExpired, "expected aggregation to fail");
        };
        shellStr += `(${runAggregate.toString()})();`;
        const awaitShell = startParallelShell(shellStr, conn.port);

        waitUntilServerHangsOnFailPoint(conn, failPointName);

        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}));

        // The aggregation running in the parallel shell will hang on the failpoint, burning
        // its time. Wait until the maxTimeMS has definitely expired.
        sleep(maxTimeMS + 2000);

        // Now drop the failpoint, allowing the aggregation to proceed. It should hit an
        // interrupt check and terminate immediately.
        assert.commandWorked(
            conn.getDB("admin").runCommand({configureFailPoint: failPointName, mode: "off"}));

        // Wait for the parallel shell to finish.
        assert.eq(awaitShell(), 0);
    }

    function runUnshardedTest(whenMatched, whenNotMatched, conn) {
        jsTestLog("Running unsharded test in whenMatched: " + whenMatched + " whenNotMatched: " +
                  whenNotMatched);
        // The target collection will always be empty so we do not test the setting that will cause
        // only failure.
        if (whenNotMatched == "fail") {
            return;
        }

        const sourceColl = conn.getDB(kDBName)[kSourceCollName];
        const destColl = conn.getDB(kDBName)[kDestCollName];
        assert.commandWorked(destColl.remove({}));

        // Be sure we're able to read from a cursor with a maxTimeMS set on it.
        (function() {
            // Use a long maxTimeMS, since we expect the operation to finish.
            const maxTimeMS = 1000 * 600;
            const pipeline = [{
                $merge: {
                    into: destColl.getName(),
                    whenMatched: whenMatched,
                    whenNotMatched: whenNotMatched
                }
            }];
            assert.doesNotThrow(() => sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS}));
        })();

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being written. The failpoint changes
        // depending on the mode. If 'whenMatched' is set to "fail" then the implementation will end
        // up issuing insert commands instead of updates.
        const kFailPointName =
            whenMatched == "fail" ? "hangDuringBatchInsert" : "hangDuringBatchUpdate";
        forceAggregationToHangAndCheckMaxTimeMsExpires(
            whenMatched, whenNotMatched, conn, kFailPointName);

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being built.
        forceAggregationToHangAndCheckMaxTimeMsExpires(
            whenMatched, whenNotMatched, conn, "hangWhileBuildingDocumentSourceMergeBatch");
    }

    // Run on a standalone.
    (function() {
        const conn = MongoRunner.runMongod({});
        assert.neq(null, conn, 'mongod was unable to start up');
        insertDocs(conn.getDB(kDBName)[kSourceCollName]);
        withEachMergeMode(
            (mode) => runUnshardedTest(mode.whenMatchedMode, mode.whenNotMatchedMode, conn));
        MongoRunner.stopMongod(conn);
    })();

    // Runs a $merge against 'mongosConn' and verifies that the maxTimeMS value is included in the
    // command sent to mongod. Since the actual timeout can unreliably happen in mongos before even
    // reaching the shard, we instead set a very large timeout and verify that the command sent to
    // mongod includes the maxTimeMS.
    function runShardedTest(whenMatched, whenNotMatched, mongosConn, mongodConn, comment) {
        jsTestLog("Running sharded test in whenMatched: " + whenMatched + " whenNotMatched: " +
                  whenNotMatched);
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
        assert.commandWorked(mongosConn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));

        const cursor = sourceColl.aggregate([{
                                               $merge: {
                                                   into: destColl.getName(),
                                                   whenMatched: whenMatched,
                                                   whenNotMatched: whenNotMatched
                                               }
                                            }],
                                            {maxTimeMS: maxTimeMS, comment: comment});
        assert(!cursor.hasNext());

        // Filter the profiler entries on the existence of $merge, since aggregations through mongos
        // will include an extra aggregation with an empty pipeline to establish cursors on the
        // shards.
        assert.soon(function() {
            return mongodConn.getDB(kDBName)
                       .system.profile
                       .find({
                           "command.aggregate": kSourceCollName,
                           "command.pipeline.$merge": {"$exists": true},
                           "command.comment": comment,
                           "command.maxTimeMS": maxTimeMS,
                       })
                       .itcount() == 1;
        });

        assert.commandWorked(mongosConn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}));
    }

    // Run on a sharded cluster.
    (function() {
        const st = new ShardingTest({shards: 2});

        // Ensure shard 0 is the primary shard. This is so that the $merge stage is guaranteed to
        // run on it.
        assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: kDBName}));
        st.ensurePrimaryShard(kDBName, st.shard0.name);

        // Set up the source collection to be sharded in a way such that each node will have some
        // documents for the remainder of the test.
        // shard 0: [MinKey, 5]
        // shard 1: [5, MaxKey]
        st.shardColl(kSourceCollName,
                     {_id: 1},  // key
                     {_id: 5},  // split
                     {_id: 6},  // move
                     kDBName);
        insertDocs(st.s.getDB(kDBName)[kSourceCollName]);

        // Start the profiler on each shard so that we can examine the $out's maxTimeMS.
        assert.commandWorked(st.shard0.getDB(kDBName).setProfilingLevel(2));
        assert.commandWorked(st.shard1.getDB(kDBName).setProfilingLevel(2));

        // // Run the test with 'destColl' unsharded.
        withEachMergeMode((mode) => runShardedTest(mode.whenMatchedMode,
                                                   mode.whenNotMatchedMode,
                                                   st.s,
                                                   st.shard0,
                                                   mode + "_unshardedDest"));

        // Run the test with 'destColl' sharded. This means that writes will be sent to both
        // shards, and if either one hangs, the MaxTimeMS will expire.
        // Shard the destination collection.
        st.shardColl(kDestCollName,
                     {_id: 1},  // key
                     {_id: 5},  // split
                     {_id: 6},  // move
                     kDBName);

        jsTestLog("Running test forcing shard " + st.shard0.name + " to hang");
        withEachMergeMode((mode) => runShardedTest(mode.whenMatchedMode,
                                                   mode.whenNotMatchedMode,
                                                   st.s,
                                                   st.shard0,
                                                   mode + "_shardedDest_" + st.shard0.name));

        jsTestLog("Running test forcing shard " + st.shard1.name + " to hang");
        withEachMergeMode((mode) => runShardedTest(mode.whenMatchedMode,
                                                   mode.whenNotMatchedMode,
                                                   st.s,
                                                   st.shard1,
                                                   mode + "_shardedDest_" + st.shard1.name));

        st.stop();
    })();
})();
