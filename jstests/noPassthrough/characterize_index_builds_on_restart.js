/**
 * Characterizes the actions (rebuilds or drops the index) taken upon unfinished indexes when
 * restarting mongod from (standalone -> standalone) and (replica set member -> standalone).
 * Additionally, the primary will use the 'disableIndexSpecNamespaceGeneration' server test
 * parameter to prevent index specs from having the 'ns' field to test the absence of the 'ns' field
 * on a replica set with unfinished indexes during restart.
 *
 * @tags: [requires_replication, requires_persistence, requires_majority_read_concern]
 */
(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    const dbName = "testDb";
    const collName = "testColl";

    const firstIndex = "firstIndex";
    const secondIndex = "secondIndex";
    const thirdIndex = "thirdIndex";
    const fourthIndex = "fourthIndex";

    const indexesToBuild = [
        {key: {i: 1}, name: firstIndex, background: true},
        {key: {j: 1}, name: secondIndex, background: true},
        {key: {i: 1, j: 1}, name: thirdIndex, background: true},
        {key: {i: -1, j: 1, k: -1}, name: fourthIndex, background: true},
    ];

    function startStandalone() {
        let mongod = MongoRunner.runMongod({cleanData: true});
        let db = mongod.getDB(dbName);
        db.dropDatabase();
        return mongod;
    }

    function restartStandalone(old) {
        jsTest.log("Restarting mongod");
        MongoRunner.stopMongod(old);
        return MongoRunner.runMongod({restart: true, dbpath: old.dbpath, cleanData: false});
    }

    function shutdownStandalone(mongod) {
        MongoRunner.stopMongod(mongod);
    }

    function startReplSet() {
        let replSet = new ReplSetTest({name: "indexBuilds", nodes: 2, nodeOptions: {syncdelay: 1}});
        let nodes = replSet.nodeList();

        // We need an arbiter to ensure that the primary doesn't step down when we restart the
        // secondary
        replSet.startSet({startClean: true});
        replSet.initiate(
            {_id: "indexBuilds", members: [{_id: 0, host: nodes[0]}, {_id: 1, host: nodes[1]}]});

        replSet.getPrimary().getDB(dbName).dropDatabase();
        return replSet;
    }

    function stopReplSet(replSet) {
        replSet.stopSet();
    }

    function addTestDocuments(db) {
        let size = 100;
        jsTest.log("Creating " + size + " test documents.");
        var bulk = db.getCollection(collName).initializeUnorderedBulkOp();
        for (var i = 0; i < size; ++i) {
            bulk.insert({i: i, j: i * i, k: 1});
        }
        assert.writeOK(bulk.execute());
    }

    function startIndexBuildOnSecondaryAndLeaveUnfinished(primaryDB, writeConcern, secondaryDB) {
        jsTest.log("Starting an index build on the secondary and leaving it unfinished.");

        assert.commandWorked(secondaryDB.adminCommand(
            {configureFailPoint: "leaveIndexBuildUnfinishedForShutdown", mode: "alwaysOn"}));

        // Do not generate the 'ns' field for index specs on the primary to test the absence of the
        // field on restart.
        assert.commandWorked(
            primaryDB.adminCommand({setParameter: 1, disableIndexSpecNamespaceGeneration: 1}));

        try {
            let res = assert.commandWorked(primaryDB.runCommand({
                createIndexes: collName,
                indexes: indexesToBuild,
                writeConcern: {w: writeConcern}
            }));

            // Wait till all four index builds hang.
            checkLog.containsWithCount(
                secondaryDB,
                "Index build interrupted due to \'leaveIndexBuildUnfinishedForShutdown\' " +
                    "failpoint. Mimicing shutdown error code.",
                4);

            // Wait until the secondary has a recovery timestamp beyond the index oplog entry. On
            // restart, replication recovery will not replay the createIndex oplog entries.
            jsTest.log("Waiting for unfinished index build to be in checkpoint.");
            assert.soon(() => {
                let replSetStatus = assert.commandWorked(
                    secondaryDB.getSiblingDB("admin").runCommand({replSetGetStatus: 1}));
                if (replSetStatus.lastStableRecoveryTimestamp >= res.operationTime)
                    return true;
            });
        } finally {
            assert.commandWorked(secondaryDB.adminCommand(
                {configureFailPoint: "leaveIndexBuildUnfinishedForShutdown", mode: "off"}));
        }
    }

    function checkForIndexRebuild(mongod, indexName, shouldExist) {
        let adminDB = mongod.getDB("admin");
        let collDB = mongod.getDB(dbName);
        let logs = adminDB.runCommand({getLog: "global"});

        let rebuildIndexLogEntry = false;
        let dropIndexLogEntry = false;

        /** The log should contain the following lines if it rebuilds or drops the index:
         *     Rebuilding index. Collection: `collNss` Index: `indexName`
         *     Dropping unfinished index. Collection: `collNss` Index: `indexName`
         */
        let rebuildIndexLine =
            "Rebuilding index. Collection: " + dbName + "." + collName + " Index: " + indexName;
        let dropIndexLine = "Dropping unfinished index. Collection: " + dbName + "." + collName +
            " Index: " + indexName;
        for (let line = 0; line < logs.log.length; line++) {
            if (logs.log[line].includes(rebuildIndexLine))
                rebuildIndexLogEntry = true;
            else if (logs.log[line].includes(dropIndexLine))
                dropIndexLogEntry = true;
        }

        // Can't be either missing both entries or have both entries for the given index name.
        assert.neq(rebuildIndexLogEntry, dropIndexLogEntry);

        // Ensure the index either exists or doesn't exist in the collection depending on the result
        // of the log.
        let collIndexes = collDB.getCollection(collName).getIndexes();

        let foundIndexEntry = false;
        for (let index = 0; index < collIndexes.length; index++) {
            assert.eq(true, collIndexes[index].hasOwnProperty('ns'));
            if (collIndexes[index].name == indexName) {
                foundIndexEntry = true;
                break;
            }
        }

        // If the log claims it rebuilt an unfinished index, the index must exist.
        assert.eq(rebuildIndexLogEntry, foundIndexEntry);

        // If the log claims it dropped an unfinished index, the index must not exist.
        assert.eq(dropIndexLogEntry, !foundIndexEntry);

        // Ensure our characterization matches the outcome of the index build.
        assert.eq(foundIndexEntry, (shouldExist ? true : false));

        if (foundIndexEntry)
            jsTest.log("Rebuilt unfinished index. Collection: " + dbName + "." + collName +
                       " Index: " + indexName);
        else
            jsTest.log("Dropped unfinished index. Collection: " + dbName + "." + collName +
                       " Index: " + indexName);
    }

    function standaloneToStandaloneTest() {
        let mongod = startStandalone();
        let collDB = mongod.getDB(dbName);

        addTestDocuments(collDB);

        jsTest.log("Starting an index build on a standalone and leaving it unfinished.");
        assert.commandWorked(collDB.adminCommand(
            {configureFailPoint: "leaveIndexBuildUnfinishedForShutdown", mode: "alwaysOn"}));
        try {
            assert.commandFailedWithCode(
                collDB.runCommand({createIndexes: collName, indexes: indexesToBuild}),
                ErrorCodes.InterruptedAtShutdown);
        } finally {
            assert.commandWorked(collDB.adminCommand(
                {configureFailPoint: "leaveIndexBuildUnfinishedForShutdown", mode: "off"}));
        }

        mongod = restartStandalone(mongod);

        checkForIndexRebuild(mongod, firstIndex, /*shouldExist=*/false);
        checkForIndexRebuild(mongod, secondIndex, /*shouldExist=*/false);
        checkForIndexRebuild(mongod, thirdIndex, /*shouldExist=*/false);
        checkForIndexRebuild(mongod, fourthIndex, /*shouldExist=*/false);

        shutdownStandalone(mongod);
    }

    function secondaryToStandaloneTest() {
        let replSet = startReplSet();
        let primary = replSet.getPrimary();
        let secondary = replSet.getSecondary();

        let primaryDB = primary.getDB(dbName);
        let secondaryDB = secondary.getDB(dbName);

        addTestDocuments(primaryDB);

        // Make sure the documents get replicated on the secondary.
        replSet.awaitReplication();

        startIndexBuildOnSecondaryAndLeaveUnfinished(primaryDB, /*writeConcern=*/2, secondaryDB);

        replSet.stopSet(/*signal=*/null, /*forRestart=*/true);

        let mongod = restartStandalone(secondary);

        checkForIndexRebuild(mongod, firstIndex, /*shouldExist=*/true);
        checkForIndexRebuild(mongod, secondIndex, /*shouldExist=*/true);
        checkForIndexRebuild(mongod, thirdIndex, /*shouldExist=*/true);
        checkForIndexRebuild(mongod, fourthIndex, /*shouldExist=*/true);

        shutdownStandalone(mongod);

        mongod = restartStandalone(primary);
        let specs = mongod.getDB(dbName).getCollection(collName).getIndexes();
        assert.eq(specs.length, 5);
        for (let index = 0; index < specs.length; index++) {
            assert.eq(true, specs[index].hasOwnProperty('ns'));
        }

        shutdownStandalone(mongod);
    }

    /* Begin tests */
    jsTest.log("Restarting nodes as standalone with unfinished indexes.");

    // Standalone restarts as standalone
    jsTest.log("Restarting standalone mongod.");
    standaloneToStandaloneTest();

    // Replica set node restarts as standalone
    jsTest.log("Restarting replica set node mongod.");
    secondaryToStandaloneTest();
})();
