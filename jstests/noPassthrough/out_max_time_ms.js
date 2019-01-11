/**
 * Test that an aggregation with a $out stage obeys the maxTimeMS.
 * @tags: [requires_sharding, requires_replication]
 */
(function() {
    load("jstests/aggregation/extras/out_helpers.js");  // For withEachOutMode().
    load("jstests/libs/fixture_helpers.js");            // For isMongos().

    const kDBName = "test";
    const kSourceCollName = "out_max_time_ms_source";
    const kDestCollName = "out_max_time_ms_dest";
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
     * Given a $out mode and two connections, run a $out aggregation against 'connToQuery', force
     * 'connToHang' to hang on the given failpoint, and ensure that the $out maxTimeMS expires.
     */
    function forceAggregationToHangAndCheckMaxTimeMsExpires(
        mode, connToQuery, connToHang, failPointName) {
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

        // For mode "replaceCollection", the namespace of the writes will be to a temp namespace so
        // remove the restriction on nss.
        if (mode == "replaceCollection")
            delete failpointCommand.data;

        assert.commandWorked(connToHang.getDB("admin").runCommand(failpointCommand));

        // Make sure we don't run out of time before the failpoint is hit.
        assert.commandWorked(connToHang.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));

        // Build the parallel shell function.
        let shellStr = `const sourceColl = db['${kSourceCollName}'];`;
        shellStr += `const destColl = db['${kDestCollName}'];`;
        shellStr += `const maxTimeMS = ${maxTimeMS};`;
        shellStr += `const mode = '${mode}';`;
        const runAggregate = function() {
            const pipeline = [{$out: {to: destColl.getName(), mode: mode}}];
            const err = assert.throws(() => sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS}));
            assert.eq(err.code, ErrorCodes.MaxTimeMSExpired, "expected aggregation to fail");
        };
        shellStr += `(${runAggregate.toString()})();`;
        const awaitShell = startParallelShell(shellStr, connToQuery.port);

        waitUntilServerHangsOnFailPoint(connToHang, failPointName);

        assert.commandWorked(connToHang.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}));

        // The aggregation running in the parallel shell will hang on the failpoint, burning
        // its time. Wait until the maxTimeMS has definitely expired.
        sleep(maxTimeMS + 2000);

        // Now drop the failpoint, allowing the aggregation to proceed. It should hit an
        // interrupt check and terminate immediately.
        assert.commandWorked(
            connToHang.getDB("admin").runCommand({configureFailPoint: failPointName, mode: "off"}));

        // Wait for the parallel shell to finish.
        assert.eq(awaitShell(), 0);
    }

    function runTest(mode, connToQuery, connToHang, setupCollections) {
        jsTestLog("Running test in mode " + mode);
        if (mode == "replaceCollection" && FixtureHelpers.isMongos(connToQuery.getDB("admin"))) {
            return;
        }

        const sourceColl = connToQuery.getDB(kDBName)[kSourceCollName];
        const destColl = connToQuery.getDB(kDBName)[kDestCollName];
        assert.commandWorked(destColl.remove({}));

        // Be sure we're able to read from a cursor with a maxTimeMS set on it.
        (function() {
            // Use a long maxTimeMS, since we expect the operation to finish.
            const maxTimeMS = 1000 * 600;
            const pipeline = [{$out: {to: destColl.getName(), mode: mode}}];
            const cursor = sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS});
            assert(!cursor.hasNext());
            assert.eq(destColl.countDocuments({_id: {$exists: true}}), nDocs);
        })();

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being written.
        const kFailPointName =
            mode == "replaceDocuments" ? "hangDuringBatchUpdate" : "hangDuringBatchInsert";
        forceAggregationToHangAndCheckMaxTimeMsExpires(
            mode, connToQuery, connToHang, kFailPointName);

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being built.
        forceAggregationToHangAndCheckMaxTimeMsExpires(
            mode, connToQuery, connToHang, "hangWhileBuildingDocumentSourceOutBatch");
    }

    // Run on a standalone.
    (function() {
        const conn = MongoRunner.runMongod({});
        assert.neq(null, conn, 'mongod was unable to start up');
        insertDocs(conn.getDB(kDBName)[kSourceCollName]);
        withEachOutMode((mode) => runTest(mode, conn, conn));
        MongoRunner.stopMongod(conn);
    })();

    // Run on a sharded cluster.
    (function() {
        const st = new ShardingTest({shards: 2});

        // Ensure shard 0 is the primary shard. This is so that the $out stage is guaranteed to
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

        // Run the test with 'destColl' unsharded.
        withEachOutMode((mode) => runTest(mode,
                                          st.s,      // connection to query
                                          st.shard0  // connection to force a hang
                                          ));

        // Run the test with 'destColl' sharded. This means that writes will be sent to both
        // shards, and if either one hangs, the MaxTimeMS will expire.
        // Shard the destination collection.
        st.shardColl(kDestCollName,
                     {_id: 1},  // key
                     {_id: 5},  // split
                     {_id: 6},  // move
                     kDBName);

        function runTestMakingShardHang(shard) {
            jsTestLog("Running test forcing shard " + shard.name + " to hang");
            withEachOutMode((mode) => runTest(mode,
                                              st.s,  // connection to query
                                              shard  // connection to force a hang
                                              ));
        }

        runTestMakingShardHang(st.shard0);
        runTestMakingShardHang(st.shard1);

        st.stop();
    })();
})();
