/**
 * Test that an aggregation with a $out stage obeys the maxTimeMS.
 * @tags: [requires_sharding, requires_replication]
 */
(function() {
    load("jstests/libs/fixture_helpers.js");  // For isMongos().
    load("jstests/libs/profiler.js");         // For profilerHasSingleMatchingEntryOrThrow.

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
     * Given a mongod connection, run a $out aggregation against 'conn' which hangs on the given
     * failpoint and ensure that the $out maxTimeMS expires.
     */
    function forceAggregationToHangAndCheckMaxTimeMsExpires(conn, failPointName) {
        // Use a short maxTimeMS so that the test completes in a reasonable amount of time. We will
        // use the 'maxTimeNeverTimeOut' failpoint to ensure that the operation does not prematurely
        // time out.
        const maxTimeMS = 1000 * 2;

        // Enable a failPoint so that the write will hang.
        let failpointCommand = {
            configureFailPoint: failPointName,
            mode: "alwaysOn",
        };

        assert.commandWorked(conn.getDB("admin").runCommand(failpointCommand));

        // Make sure we don't run out of time before the failpoint is hit.
        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));

        // Build the parallel shell function.
        let shellStr = `const sourceColl = db['${kSourceCollName}'];`;
        shellStr += `const destColl = db['${kDestCollName}'];`;
        shellStr += `const maxTimeMS = ${maxTimeMS};`;
        const runAggregate = function() {
            const pipeline = [{$out: destColl.getName()}];
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

    function runUnshardedTest(conn) {
        jsTestLog("Running unsharded test");

        const sourceColl = conn.getDB(kDBName)[kSourceCollName];
        const destColl = conn.getDB(kDBName)[kDestCollName];
        assert.commandWorked(destColl.remove({}));

        // Be sure we're able to read from a cursor with a maxTimeMS set on it.
        (function() {
            // Use a long maxTimeMS, since we expect the operation to finish.
            const maxTimeMS = 1000 * 600;
            const pipeline = [{$out: destColl.getName()}];
            const cursor = sourceColl.aggregate(pipeline, {maxTimeMS: maxTimeMS});
            assert(!cursor.hasNext());
            assert.eq(destColl.countDocuments({_id: {$exists: true}}), nDocs);
        })();

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being written.
        const kFailPointName = "hangDuringBatchInsert";
        forceAggregationToHangAndCheckMaxTimeMsExpires(conn, kFailPointName);

        assert.commandWorked(destColl.remove({}));

        // Force the aggregation to hang while the batch is being built.
        forceAggregationToHangAndCheckMaxTimeMsExpires(conn,
                                                       "hangWhileBuildingDocumentSourceOutBatch");
    }

    // Run on a standalone.
    (function() {
        const conn = MongoRunner.runMongod({});
        assert.neq(null, conn, 'mongod was unable to start up');
        insertDocs(conn.getDB(kDBName)[kSourceCollName]);
        runUnshardedTest(conn);
        MongoRunner.stopMongod(conn);
    })();
})();
