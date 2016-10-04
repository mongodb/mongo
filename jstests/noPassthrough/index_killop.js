/**
 * Confirms that both foreground and background index builds can be aborted using killop.
 */
(function() {
    "use strict";

    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.dropDatabase());
    testDB.test.insertOne({a: 1});

    // Returns the op id for the running index build, or -1 if there is no current index build.
    function getIndexBuildOpId() {
        const result = testDB.currentOp();
        assert.commandWorked(result);
        let indexBuildOpId = -1;

        result.inprog.forEach(function(op) {
            // Identify the index build as the createIndex command
            // It is assumed that no other clients are concurrently
            // accessing the 'test' database.
            if ((op.op == 'query' || op.op == 'command') && 'createIndexes' in op.query) {
                indexBuildOpId = op.opid;
            }
        });
        return indexBuildOpId;
    }

    // Test that building an index with 'options' can be aborted using killop.
    function testAbortIndexBuild(options) {
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

        const createIdx = startParallelShell(
            "let coll = db.getSiblingDB('test').test;" +
                "assert.commandWorked(coll.createIndex({ a: 1 }, " + tojson(options) + "));",
            conn.port);

        // When the index build starts, find its op id.
        let opId;
        assert.soon(function() {
            return (opId = getIndexBuildOpId()) != -1;
        }, "Index build operation not found after starting via parallelShell");

        // Kill the index build.
        assert.commandWorked(testDB.killOp(opId));

        assert.commandWorked(
            testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));

        // Wait for the index build to stop.
        assert.soon(function() {
            return getIndexBuildOpId() == -1;
        });

        const exitCode = createIdx({checkExitSuccess: false});
        assert.neq(
            0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

        // Check that no new index has been created.  This verifies that the index build was aborted
        // rather than successfully completed.
        assert.eq([{_id: 1}], testDB.test.getIndexKeys());
    }

    testAbortIndexBuild({background: true});
    testAbortIndexBuild({background: false});
})();
