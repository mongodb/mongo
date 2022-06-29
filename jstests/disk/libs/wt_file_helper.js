load("jstests/libs/analyze_plan.js");

/**
 * Get the URI of the wt collection file given the collection name.
 */
let getUriForColl = function(coll) {
    assert(coll.exists());  // Collection must exist
    return coll.stats().wiredTiger.uri.split("table:")[1];
};

/**
 * Get the URI of the wt index file given the collection name and the index name.
 */
let getUriForIndex = function(coll, indexName) {
    assert(coll.exists());  // Collection must exist
    const ret = assert.commandWorked(coll.getDB().runCommand({collStats: coll.getName()}));
    return ret.indexDetails[indexName].uri.split("table:")[1];
};

/**
 * 'Corrupt' the file by replacing it with an empty file.
 */
let corruptFile = function(file) {
    removeFile(file);
    writeFile(file, "");
};

/**
 * Starts a mongod on the provided data path without clearing data. Accepts 'options' as parameters
 * to runMongod.
 */
let startMongodOnExistingPath = function(dbpath, options) {
    let args = {dbpath: dbpath, noCleanData: true};
    for (let attr in options) {
        if (options.hasOwnProperty(attr))
            args[attr] = options[attr];
    }
    return MongoRunner.runMongod(args);
};

let assertQueryUsesIndex = function(coll, query, indexName) {
    let res = coll.find(query).explain();
    assert.commandWorked(res);

    let inputStage = getWinningPlan(res.queryPlanner).inputStage;
    assert.eq(inputStage.stage, "IXSCAN");
    assert.eq(inputStage.indexName, indexName);
};

/**
 * Assert that running MongoDB with --repair on the provided dbpath exits cleanly.
 */
let assertRepairSucceeds = function(dbpath, port, opts) {
    let args = ["mongod", "--repair", "--port", port, "--dbpath", dbpath, "--bind_ip_all"];
    for (let a in opts) {
        if (opts.hasOwnProperty(a))
            args.push("--" + a);

        if (opts[a].length > 0) {
            args.push(opts[a]);
        }
    }
    jsTestLog("Repairing the node");
    assert.eq(0, runMongoProgram.apply(this, args));
};

let assertRepairFailsWithFailpoint = function(dbpath, port, failpoint) {
    const param = "failpoint." + failpoint + "={'mode': 'alwaysOn'}";
    jsTestLog("The node should fail to complete repair with --setParameter " + param);

    assert.eq(
        MongoRunner.EXIT_ABRUPT,
        runMongoProgram(
            "mongod", "--repair", "--port", port, "--dbpath", dbpath, "--setParameter", param));
};

/**
 * Asserts that running MongoDB with --repair on the provided dbpath fails.
 */
let assertRepairFails = function(dbpath, port) {
    jsTestLog("The node should complete repairing the node but fails.");

    assert.neq(0, runMongoProgram("mongod", "--repair", "--port", port, "--dbpath", dbpath));
};

/**
 * Assert that starting MongoDB with --replSet on an existing data path exits with a specific
 * error.
 */
let assertErrorOnStartupWhenStartingAsReplSet = function(dbpath, port, rsName) {
    jsTestLog("The repaired node should fail to start up with the --replSet option");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod(
        {dbpath: dbpath, port: port, replSet: rsName, noCleanData: true, waitForConnect: false});
    assert.soon(function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*50923/) >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path exits with a specific
 * error because the previous repair failed.
 */
let assertErrorOnStartupAfterIncompleteRepair = function(dbpath, port) {
    jsTestLog("The node should fail to start up because a previous repair did not complete");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod(
        {dbpath: dbpath, port: port, noCleanData: true, waitForConnect: false});
    assert.soon(function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*50922/) >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path succeeds. Uses a provided
 * testFunc to run any caller-provided checks on the started node.
 */
let assertStartAndStopStandaloneOnExistingDbpath = function(dbpath, port, testFunc) {
    jsTestLog("The repaired node should start up and serve reads as a standalone");
    let node = MongoRunner.runMongod({dbpath: dbpath, port: port, noCleanData: true});
    assert(node);
    testFunc(node);
    MongoRunner.stopMongod(node);
};

/**
 * Assert that starting MongoDB with --replSet succeeds. Uses a provided testFunc to run any
 * caller-provided checks on the started node.
 *
 * Returns the started node.
 */
let assertStartInReplSet = function(replSet, originalNode, cleanData, expectResync, testFunc) {
    jsTestLog("The node should rejoin the replica set. Clean data: " + cleanData +
              ". Expect resync: " + expectResync);
    // Skip clearing initial sync progress after a successful initial sync attempt so that we
    // can check initialSyncStatus fields after initial sync is complete.
    let node = replSet.start(originalNode, {
        dbpath: originalNode.dbpath,
        port: originalNode.port,
        restart: !cleanData,
        setParameter: {"failpoint.skipClearInitialSyncState": "{'mode':'alwaysOn'}"}
    });

    replSet.awaitSecondaryNodes();

    // Ensure that an initial sync attempt was made and succeeded if the data directory was cleaned.
    let res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
    if (expectResync) {
        assert.eq(1, res.initialSyncStatus.initialSyncAttempts.length);
        assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);
    } else {
        assert.eq(undefined, res.initialSyncStatus);
    }

    assert.commandWorked(
        node.adminCommand({configureFailPoint: 'skipClearInitialSyncState', mode: 'off'}));

    testFunc(node);
    return node;
};

/**
 * Assert certain error messages are thrown on startup when files are missing or corrupt.
 */
let assertErrorOnStartupWhenFilesAreCorruptOrMissing = function(
    dbpath, dbName, collName, deleteOrCorruptFunc, errmsgRegExp) {
    // Start a MongoDB instance, create the collection file.
    const mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    const testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance and get an expected error message.
    clearRawMongoProgramOutput();
    assert.eq(MongoRunner.EXIT_ABRUPT,
              runMongoProgram("mongod", "--port", mongod.port, "--dbpath", dbpath));
    assert.gte(rawMongoProgramOutput().search(errmsgRegExp), 0);
};

/**
 * Assert certain error messages are thrown on a specific request when files are missing or corrupt.
 */
let assertErrorOnRequestWhenFilesAreCorruptOrMissing = function(
    dbpath, dbName, collName, deleteOrCorruptFunc, requestFunc, errmsgRegExp) {
    // Start a MongoDB instance, create the collection file.
    mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance.
    clearRawMongoProgramOutput();
    mongod = MongoRunner.runMongod({dbpath: dbpath, port: mongod.port, noCleanData: true});

    // This request crashes the server.
    testColl = mongod.getDB(dbName)[collName];
    requestFunc(testColl);

    // Get an expected error message.
    const rawLogs = rawMongoProgramOutput();
    const matchedIndex = rawLogs.search(errmsgRegExp);
    if (matchedIndex < 0) {
        jsTestLog("String pattern not found in rawMongoProgramOutput(): " + rawLogs);
    }

    assert.gte(matchedIndex, 0);
    MongoRunner.stopMongod(mongod, 9, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Runs the WiredTiger tool with the provided arguments.
 */
let runWiredTigerTool = function(...args) {
    const cmd = ['wt'].concat(args);
    // TODO (SERVER-67632): Check the return code on Windows variants again.
    if (_isWindows()) {
        run.apply(undefined, cmd);
    } else {
        assert.eq(run.apply(undefined, cmd), 0, "error executing: " + cmd.join(' '));
    }
};

/**
 * Stops the given mongod, runs the truncate command on the given uri using the WiredTiger tool, and
 * starts mongod again on the same path.
 */
let truncateUriAndRestartMongod = function(uri, conn, mongodOptions) {
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    runWiredTigerTool("-h", conn.dbpath, "truncate", uri);
    return startMongodOnExistingPath(conn.dbpath, mongodOptions);
};

/**
 * Stops the given mongod, dumps the table with the uri, modifies the content, and loads it back to
 * the table.
 */
let rewriteTable = function(uri, conn, modifyData) {
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    const separator = _isWindows() ? '\\' : '/';
    const tempDumpFile = conn.dbpath + separator + "temp_dump";
    const newTableFile = conn.dbpath + separator + "new_table_file";
    runWiredTigerTool("-h",
                      conn.dbpath,
                      "-r",
                      "-C",
                      "log=(compressor=snappy,path=journal)",
                      "dump",
                      "-x",
                      "-f",
                      tempDumpFile,
                      "table:" + uri);
    let dumpLines = cat(tempDumpFile).split("\n");
    modifyData(dumpLines);
    writeFile(newTableFile, dumpLines.join("\n"));
    runWiredTigerTool("-h", conn.dbpath, "load", "-f", newTableFile, "-r", uri);
};

// In WiredTiger table dumps, the first seven lines are the header and key that we don't want to
// modify. We will skip them and start from the line containing the first value.
const wtHeaderLines = 7;

/**
 * Inserts the documents with duplicate field names into the MongoDB server.
 */
let insertDocDuplicateFieldName = function(coll, uri, conn, numDocs) {
    for (let i = 0; i < numDocs; ++i) {
        coll.insert({a: "aaaaaaa", b: "bbbbbbb"});
    }
    // The format of the BSON documents will be {_id: ObjectId(), a: "aaaaaaa", a: "bbbbbbb"}.
    let makeDuplicateFieldNames = function(lines) {
        // The offset of the document's field name 'b' in the hex string dumped by wt tool.
        const offsetToFieldB = 75;
        // Each record takes two lines with a key and a value. We will only modify the values.
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            // Switch the field name 'b' to 'a' to create a duplicate field name.
            lines[i] = lines[i].substring(0, offsetToFieldB) + "1" +
                lines[i].substring(offsetToFieldB + 1);
        }
    };
    rewriteTable(uri, conn, makeDuplicateFieldNames);
};