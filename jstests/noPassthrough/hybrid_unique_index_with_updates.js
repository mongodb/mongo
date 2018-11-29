/**
 * Tests that write operations are accepted and result in correct indexing behavior for each phase
 * of hybrid unique index builds. This test inserts a duplicate document at different phases of an
 * index build to confirm that the resulting behavior is failure.
 *
 * @tags: [requires_document_locking]
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    let conn = MongoRunner.runMongod();
    let testDB = conn.getDB('test');

    // Run 'func' while failpoint is enabled.
    let doDuringFailpoint = function(failPointName, logMessage, func, i) {
        clearRawMongoProgramOutput();
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: failPointName, mode: "alwaysOn", data: {"i": i}}));

        assert.soon(() => rawMongoProgramOutput().indexOf(logMessage) >= 0);

        func();

        assert.commandWorked(testDB.adminCommand({configureFailPoint: failPointName, mode: "off"}));
    };

    const docsToInsert = 1000;
    let setUp = function(coll) {
        coll.drop();

        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < docsToInsert; i++) {
            bulk.insert({i: i});
        }
        assert.commandWorked(bulk.execute());
    };

    let buildIndexInBackground = function(expectDuplicateKeyError) {
        if (expectDuplicateKeyError) {
            return startParallelShell(function() {
                assert.commandFailedWithCode(
                    db.hybrid.createIndex({i: 1}, {background: true, unique: true}),
                    ErrorCodes.DuplicateKey);
            }, conn.port);
        }
        return startParallelShell(function() {
            assert.commandWorked(db.hybrid.createIndex({i: 1}, {background: true, unique: true}));
        }, conn.port);
    };

    /**
     * Run a background index build on a unique index under different configurations. Introduce
     * duplicate keys on the index that may cause it to fail or succeed, depending on the following
     * optional parmeters:
     * {
     *   // Which operation used to introduce a duplicate key.
     *   operation {string}: "insert", "update"
     *
     *   // Whether or not resolve the duplicate key before completing the build.
     *   resolve {bool}
     *
     *   // Which phase of the index build to introduce the duplicate key.
     *   phase {number}: 0-4
     * }
     */
    let runTest = function(config) {
        jsTestLog("running test with config: " + tojson(config));

        setUp(testDB.hybrid);

        // Expect the build to fail with a duplicate key error if we insert a duplicate key and
        // don't resolve it.
        let expectDuplicate = config.resolve === false;
        let awaitBuild = buildIndexInBackground(expectDuplicate);

        // Introduce a duplicate key, either from an insert or update. Optionally, follow-up with an
        // operation that will resolve the duplicate by removing it or updating it.
        const dup = {i: 0};
        let doOperation = function() {
            if ("insert" == config.operation) {
                assert.commandWorked(testDB.hybrid.insert(dup));
                if (config.resolve) {
                    assert.commandWorked(testDB.hybrid.deleteOne(dup));
                }
            } else if ("update" == config.operation) {
                assert.commandWorked(testDB.hybrid.update(dup, {i: 1}));
                if (config.resolve) {
                    assert.commandWorked(testDB.hybrid.update({i: 1}, dup));
                }
            }
        };

        const stopKey = 0;
        switch (config.phase) {
            // Don't hang the build.
            case undefined:
                break;
            // Hang before scanning the first document.
            case 0:
                doDuringFailpoint("hangBeforeIndexBuildOf",
                                  "Hanging before index build of i=" + stopKey,
                                  doOperation,
                                  stopKey);
                break;
            // Hang after scanning the first document.
            case 1:
                doDuringFailpoint("hangAfterIndexBuildOf",
                                  "Hanging after index build of i=" + stopKey,
                                  doOperation,
                                  stopKey);
                break;
            // Hang before the first drain and after dumping the keys from the external sorter into
            // the index.
            case 2:
                doDuringFailpoint("hangAfterIndexBuildDumpsInsertsFromBulk",
                                  "Hanging after dumping inserts from bulk builder",
                                  doOperation);
                break;
            // Hang before the second drain.
            case 3:
                doDuringFailpoint("hangAfterIndexBuildFirstDrain",
                                  "Hanging after index build first drain",
                                  doOperation);
                break;
            // Hang before the final drain and commit.
            case 4:
                doDuringFailpoint("hangAfterIndexBuildSecondDrain",
                                  "Hanging after index build second drain",
                                  doOperation);
                break;
            default:
                assert(false, "Invalid phase: " + config.phase);
        }

        awaitBuild();

        let expectedDocs = docsToInsert;
        expectedDocs += (config.operation == "insert" && config.resolve === false) ? 1 : 0;

        assert.eq(expectedDocs, testDB.hybrid.count());
        assert.eq(expectedDocs, testDB.hybrid.find().itcount());
        assert.commandWorked(testDB.hybrid.validate({full: true}));
    };

    runTest({});

    for (let i = 0; i <= 4; i++) {
        runTest({operation: "insert", resolve: true, phase: i});
        runTest({operation: "insert", resolve: false, phase: i});
        runTest({operation: "update", resolve: true, phase: i});
        runTest({operation: "update", resolve: false, phase: i});
    }

    MongoRunner.stopMongod(conn);
})();
