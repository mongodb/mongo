/**
 * Tests the dbCheck command's extra index keys check behavior when the collection
 * or index is not found.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    awaitDbCheckCompletion,
    checkHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck,
} from "jstests/replsets/libs/dbcheck_utils.js";

(function () {
    "use strict";

    // This test injects inconsistencies between replica set members; do not fail because of expected
    // dbHash differences.
    TestData.skipCheckDBHashes = true;

    const dbName = "dbCheckExtraIndexKeys";
    const collName = "dbCheckExtraIndexKeysColl";

    const replSet = new ReplSetTest({
        name: jsTestName(),
        nodes: 2,
        nodeOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 3}), dbCheckHealthLogEveryNBatches: 1},
        },
    });
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const secondary = replSet.getSecondary();
    const primaryHealthLog = primary.getDB("local").system.healthlog;
    const secondaryHealthLog = secondary.getDB("local").system.healthlog;
    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);
    assert.commandWorked(primaryDB.createCollection(collName));
    const defaultNumDocs = 1000;
    const defaultMaxDocsPerBatch = 100;
    const writeConcern = {
        w: "majority",
    };

    const debugBuild = primaryDB.adminCommand("buildInfo").debug;

    function collNotFoundBeforeDbCheck(docSuffix, collOpts) {
        jsTestLog(
            `Testing that an collection that doesn't exist before dbcheck will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);

        const hangBeforeExtraIndexKeysCheck = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysCheck");

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeExtraIndexKeysCheck.wait();

        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        replSet.awaitReplication();

        hangBeforeExtraIndexKeysCheck.off();
        awaitDbCheckCompletion(replSet, primaryDB);
        checkHealthLog(primaryHealthLog, logQueries.collNotFoundWarningQuery, 1);
        // If index not found before db check, we won't create any oplog entry.
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        // No other info or error logs.
        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function indexNotFoundBeforeDbCheck(docSuffix, collOpts) {
        jsTestLog(
            `Testing that an index that doesn't exist before dbcheck will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

        checkHealthLog(primaryHealthLog, logQueries.indexNotFoundWarningQuery, 1);
        // If index not found before db check, we won't create any oplog entry.
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        // No other info or error logs.
        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function collNotFoundDuringReverseLookup(docSuffix, collOpts) {
        jsTestLog(
            `Testing that a collection that doesn't exist during reverse lookup will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangBeforeReverseLookupCatalogSnapshot = configureFailPoint(
            primaryDB,
            "hangBeforeReverseLookupCatalogSnapshot",
        );

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeReverseLookupCatalogSnapshot.wait();

        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        replSet.awaitReplication();

        hangBeforeReverseLookupCatalogSnapshot.off();

        awaitDbCheckCompletion(replSet, primaryDB);
        jsTestLog("checking primary health log");
        checkHealthLog(primaryHealthLog, logQueries.collNotFoundWarningQuery, 1);
        // If index not found during reverse lookup, we won't create any oplog entry for that batch.
        jsTestLog("checking secondary health log");
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function indexNotFoundDuringReverseLookup(docSuffix, collOpts) {
        jsTestLog(
            `Testing that an index that doesn't exist during reverse lookup will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangBeforeReverseLookupCatalogSnapshot = configureFailPoint(
            primaryDB,
            "hangBeforeReverseLookupCatalogSnapshot",
        );

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeReverseLookupCatalogSnapshot.wait();

        assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

        hangBeforeReverseLookupCatalogSnapshot.off();

        awaitDbCheckCompletion(replSet, primaryDB);
        jsTestLog("checking primary health log");
        checkHealthLog(primaryHealthLog, logQueries.indexNotFoundWarningQuery, 1);
        // If index not found during reverse lookup, we won't create any oplog entry for that batch.
        jsTestLog("checking secondary health log");
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function collNotFoundDuringHashing(docSuffix, collOpts) {
        jsTestLog(
            `Testing that a collection that doesn't exist during hashing will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangBeforeExtraIndexKeysHashing = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeExtraIndexKeysHashing.wait();

        assert.commandWorked(primaryDB.runCommand({drop: collName}));
        replSet.awaitReplication();

        hangBeforeExtraIndexKeysHashing.off();

        awaitDbCheckCompletion(replSet, primaryDB);

        jsTestLog("checking primary health log");
        checkHealthLog(primaryHealthLog, logQueries.collNotFoundWarningQuery, 1);
        // If index not found during hashing, we won't create any oplog entry for that batch.
        jsTestLog("checking secondary health log");
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function indexNotFoundDuringHashing(docSuffix, collOpts) {
        jsTestLog(
            `Testing that an index that doesn't exist during hashing will generate a health log entry.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangBeforeExtraIndexKeysHashing = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: defaultMaxDocsPerBatch,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeExtraIndexKeysHashing.wait();

        assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

        hangBeforeExtraIndexKeysHashing.off();

        awaitDbCheckCompletion(replSet, primaryDB);
        jsTestLog("checking primary health log");
        checkHealthLog(primaryHealthLog, logQueries.indexNotFoundWarningQuery, 1);
        // If index not found during hashing, we won't create any oplog entry for that batch.
        jsTestLog("checking secondary health log");
        checkHealthLog(secondaryHealthLog, logQueries.warningQuery, 0);

        checkHealthLog(primaryHealthLog, logQueries.infoOrErrorQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.infoOrErrorQuery, 0);
    }

    function keysChangedBeforeHashing(collOpts) {
        jsTestLog(
            `Testing that if keys within batch boundaries change in between reverse lookup and hashing we won't error.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, 10, null /*docSuffix*/, collOpts);
        const primaryColl = primaryDB.getCollection(collName);
        primaryColl.deleteOne({a: 3});

        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangBeforeExtraIndexKeysHashing = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

        // First batch should 0, 1, 2, 4, 5. Batch boundaries will be [0, 5].
        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: 5,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangBeforeExtraIndexKeysHashing.wait();

        // Actual batch will be 1, 2, 3, 4.
        primaryColl.deleteOne({a: 0});
        primaryColl.insertOne({a: 3});
        primaryColl.deleteOne({a: 5});

        hangBeforeExtraIndexKeysHashing.off();

        awaitDbCheckCompletion(replSet, primaryDB);

        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

        jsTestLog("Checking for correct number of batches on primary");
        checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 2);
        jsTestLog("Checking for correct number of batches on secondary");
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 2);
    }

    function allIndexKeysNotFoundDuringReverseLookup(nDocs, docSuffix, collOpts) {
        clearRawMongoProgramOutput();
        jsTestLog(
            `Testing that if all the index keys are deleted during reverse lookup we still log a batch.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix, collOpts);
        const primaryColl = primaryDB.getCollection(collName);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangAfterReverseLookupCatalogSnapshot = configureFailPoint(
            primaryDB,
            "hangAfterReverseLookupCatalogSnapshot",
        );

        // Batch size 1.
        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: 1,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangAfterReverseLookupCatalogSnapshot.wait();

        jsTestLog("Removing all docs");
        assert.commandWorked(primaryColl.deleteMany({}));
        replSet.awaitReplication();
        assert.eq(primaryColl.find({}).count(), 0);

        hangAfterReverseLookupCatalogSnapshot.off();

        awaitDbCheckCompletion(replSet, primaryDB);

        checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 2);
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 2);

        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

        if (debugBuild) {
            assert(
                rawMongoProgramOutput("could not find any keys in index").match(/7844803/),
                "expected 'could not find any keys in index' log",
            );
        }
    }

    function keyNotFoundDuringReverseLookup(nDocs, collOpts) {
        clearRawMongoProgramOutput();
        jsTestLog(
            `Testing that if a key is deleted during reverse lookup we continue with db check.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, nDocs, null /*docSuffix*/, collOpts);
        const primaryColl = primaryDB.getCollection(collName);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const hangAfterReverseLookupCatalogSnapshot = configureFailPoint(
            primaryDB,
            "hangAfterReverseLookupCatalogSnapshot",
        );

        // Batch size 1.
        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: 1,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        hangAfterReverseLookupCatalogSnapshot.wait();
        jsTestLog("Removing one doc");
        assert.commandWorked(primaryColl.deleteOne({"a": 1}));
        replSet.awaitReplication();
        assert.eq(primaryColl.find({}).count(), nDocs - 1);

        // TODO SERVER-80257: Replace using this failpoint with test commands instead.
        const skipUpdatingIndexDocumentPrimary = configureFailPoint(primaryDB, "skipUpdatingIndexDocument", {
            indexName: "a_1",
        });
        const skipUpdatingIndexDocumentSecondary = configureFailPoint(secondaryDB, "skipUpdatingIndexDocument", {
            indexName: "a_1",
        });

        jsTestLog("Updating docs to remove index key field");
        assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));
        replSet.awaitReplication();
        assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);

        hangAfterReverseLookupCatalogSnapshot.off();

        awaitDbCheckCompletion(replSet, primaryDB);

        jsTestLog("checking primary health log");
        // First doc (a: 0) was valid, second doc was not found (a:1) but we continue with dbcheck and
        // find inconsistencies in the rest of the docs.
        checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nDocs - 2);
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs - 2);
        jsTestLog("checking secondary health log");
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

        jsTestLog("checking primary for correct num of health logs");
        checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, nDocs - 1);

        jsTestLog("checking secondary for correct num of health logs");
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, nDocs - 1);

        skipUpdatingIndexDocumentPrimary.off();
        skipUpdatingIndexDocumentSecondary.off();
    }

    function runIndexBuild(dbName, collName, indexSpec) {
        jsTest.log("Index build request starting...");
        db.getSiblingDB(dbName).runCommand({createIndexes: collName, indexes: [indexSpec]});
    }

    function dbCheckDuringIndexBuild(docSuffix, collOpts) {
        jsTestLog(
            `Testing that dbcheck will generate a health log entry when index build has not completed yet.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);

        let joinIndexBuild;
        const indexBuildFailPoint = configureFailPoint(primaryDB, "hangAfterSettingUpIndexBuild");

        try {
            joinIndexBuild = startParallelShell(
                funWithArgs(runIndexBuild, dbName, collName, {key: {a: 1}, name: "a_1"}),
                primary.port,
            );
            indexBuildFailPoint.wait();

            runDbCheck(replSet, primaryDB, collName, {
                validateMode: "extraIndexKeysCheck",
                secondaryIndex: "a_1",
                maxDocsPerBatch: 20,
                batchWriteConcern: writeConcern,
            });
            checkHealthLog(primaryHealthLog, {"operation": "dbCheckStop"}, 1);
        } finally {
            indexBuildFailPoint.off();
        }

        joinIndexBuild();

        // Check that the primary logged a warning health log entry because the index did not exist when
        // dbcheck started.
        checkHealthLog(primaryHealthLog, logQueries.indexNotFoundWarningQuery, 1);
        // Check that there are no other warning/error health logs on the primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1);

        // Check that there are no warning/error health logs on the secondary.
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    function indexDropAfterFirstBatch(docSuffix, collOpts) {
        jsTestLog(
            `Testing that dbcheck will generate a health log entry when index is dropped in the middle of dbcheck.
        collOpts: ${collOpts}`,
        );

        resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix, collOpts);
        assert.commandWorked(
            primaryDB.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
            }),
        );
        replSet.awaitReplication();

        const primaryHangAfterHashing = configureFailPoint(primaryDB, "primaryHangAfterExtraIndexKeysHashing");
        const secondaryHangAfterHashing = configureFailPoint(secondaryDB, "secondaryHangAfterExtraIndexKeysHashing");

        let dbCheckParameters = {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: 20,
            batchWriteConcern: writeConcern,
        };
        runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

        primaryHangAfterHashing.wait();
        secondaryHangAfterHashing.wait();

        assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

        primaryHangAfterHashing.off();
        secondaryHangAfterHashing.off();
        awaitDbCheckCompletion(replSet, primaryDB);

        // Check that the primary logged an info batch for the first batch before the index was dropped.
        checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 1);
        // Check that the primary logged a warning entry because of the index drop.
        checkHealthLog(primaryHealthLog, logQueries.indexNotFoundWarningQuery, 1);
        // Check that there are no other errors/warnings on the primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1);

        // Check that the secondary has an info batch for the first batch.
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 1);
        // Check that there are no errors/warnings on the primary.
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    [{}, {clusteredIndex: {key: {_id: 1}, unique: true}}].forEach((collOpts) => {
        // Test with integer index entries (1, 2, 3, etc.), single character string entries ("1",
        // "2", "3", etc.), and long string entries ("1aaaaaaaaaa")
        [null, "", "aaaaaaaaaa"].forEach((docSuffix) => {
            indexNotFoundBeforeDbCheck(docSuffix, collOpts);
            indexNotFoundDuringHashing(docSuffix, collOpts);
            indexNotFoundDuringReverseLookup(docSuffix, collOpts);
            collNotFoundBeforeDbCheck(docSuffix, collOpts);
            collNotFoundDuringHashing(docSuffix, collOpts);
            collNotFoundDuringReverseLookup(docSuffix, collOpts);
            allIndexKeysNotFoundDuringReverseLookup(10, docSuffix, collOpts);
            dbCheckDuringIndexBuild(docSuffix, collOpts);
            indexDropAfterFirstBatch(docSuffix, collOpts);
        });

        keysChangedBeforeHashing(collOpts);
        keyNotFoundDuringReverseLookup(10, collOpts);
    });

    replSet.stopSet(undefined /* signal */, false /* forRestart */, {skipCheckDBHashes: true, skipValidation: true});
})();
