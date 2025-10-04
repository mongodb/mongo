import {getPlanStage, getWinningPlanFromExplain, isExpress, isIxscan} from "jstests/libs/query/analyze_plan.js";

/**
 * Get the URI of the wt collection file given the collection name.
 */
export let getUriForColl = function (coll) {
    assert(coll.exists()); // Collection must exist
    return coll.stats().wiredTiger.uri.split("table:")[1];
};

/**
 * Get the URI of the wt index file given the collection name and the index name.
 */
export let getUriForIndex = function (coll, indexName) {
    assert(coll.exists()); // Collection must exist
    const ret = assert.commandWorked(coll.getDB().runCommand({collStats: coll.getName()}));
    return ret.indexDetails[indexName].uri.split("table:")[1];
};

/**
 * 'Corrupt' the file by replacing it with an empty file.
 */
export let corruptFile = function (file) {
    removeFile(file);
    writeFile(file, "");
};

/**
 * Starts a mongod on the provided data path without clearing data. Accepts 'options' as parameters
 * to runMongod.
 */
export let startMongodOnExistingPath = function (dbpath, options) {
    let args = {dbpath: dbpath, noCleanData: true};
    for (let attr in options) {
        if (options.hasOwnProperty(attr)) args[attr] = options[attr];
    }
    return MongoRunner.runMongod(args);
};

export let assertQueryUsesIndex = function (coll, query, indexName) {
    let res = coll.find(query).explain();
    assert.commandWorked(res);

    let stage;
    if (isIxscan(coll.getDB(), res)) {
        stage = getPlanStage(getWinningPlanFromExplain(res), "IXSCAN");
    } else {
        assert(isExpress(coll.getDB(), res), tojson(res));
        stage = getPlanStage(getWinningPlanFromExplain(res), "EXPRESS_IXSCAN");
    }
    assert.eq(stage.indexName, indexName, "Expecting index scan on " + indexName + ": " + tojson(res));
};

/**
 * Assert that running MongoDB with --repair on the provided dbpath exits cleanly.
 */
export let assertRepairSucceeds = function (dbpath, port, opts) {
    let args = ["mongod", "--repair", "--port", port, "--dbpath", dbpath, "--bind_ip_all"];
    for (let a in opts) {
        if (opts.hasOwnProperty(a)) args.push("--" + a);

        if (opts[a].length > 0) {
            args.push(opts[a]);
        }
    }
    jsTestLog("Repairing the node");
    assert.eq(0, runMongoProgram.apply(this, args));
};

export let assertRepairFailsWithFailpoint = function (dbpath, port, failpoint) {
    const param = "failpoint." + failpoint + "={'mode': 'alwaysOn'}";
    jsTestLog("The node should fail to complete repair with --setParameter " + param);

    assert.eq(
        MongoRunner.EXIT_ABRUPT,
        runMongoProgram("mongod", "--repair", "--port", port, "--dbpath", dbpath, "--setParameter", param),
    );
};

/**
 * Asserts that running MongoDB with --repair on the provided dbpath fails.
 */
export let assertRepairFails = function (dbpath, port) {
    jsTestLog("The node should complete repairing the node but fails.");

    assert.neq(0, runMongoProgram("mongod", "--repair", "--port", port, "--dbpath", dbpath));
};

/**
 * Assert that starting MongoDB with --replSet on an existing data path exits with a specific
 * error.
 */
export let assertErrorOnStartupWhenStartingAsReplSet = function (dbpath, port, rsName) {
    jsTestLog("The repaired node should fail to start up with the --replSet option");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        replSet: rsName,
        noCleanData: true,
        waitForConnect: false,
    });
    assert.soon(function () {
        return rawMongoProgramOutput("Fatal assertion").search(/50923/) >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path exits with a specific
 * error because the previous repair failed.
 */
export let assertErrorOnStartupAfterIncompleteRepair = function (dbpath, port) {
    jsTestLog("The node should fail to start up because a previous repair did not complete");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod({dbpath: dbpath, port: port, noCleanData: true, waitForConnect: false});
    assert.soon(function () {
        return rawMongoProgramOutput("Fatal assertion").search(/50922/) >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB with --replSet will fail when going through initial sync with
 * existing data on the node.
 */
export let assertErrorOnStartupWhenInitialSyncingWithData = function (replSet, originalNode) {
    jsTestLog("The node with data should fail to complete initial sync");

    clearRawMongoProgramOutput();
    let node = null;
    // Sometimes replsettest.Start cannot connect to the node before it crashes and it will throw a
    // StopError. Wrap this call in a try block to avoid throwing on that error. This is fine
    // because we still make sure the correct fatal log message gets written.
    try {
        node = replSet.start(originalNode, {
            dbpath: originalNode.dbpath,
            port: originalNode.port,
            restart: true,
            waitForConnect: true,
            setParameter: {"failpoint.skipClearInitialSyncState": "{'mode':'alwaysOn'}"},
        });
    } catch (e) {
        jsTestLog("Ignoring exception from replsettest.start: " + tojson(e));
    } finally {
        assert.soon(function () {
            return rawMongoProgramOutput("Fatal assertion").search(/9184100/) >= 0;
        });
        if (node) {
            MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
        }
    }
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path succeeds. Uses a provided
 * testFunc to run any caller-provided checks on the started node.
 */
export let assertStartAndStopStandaloneOnExistingDbpath = function (dbpath, port, testFunc) {
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
export let assertStartInReplSet = function (replSet, originalNode, cleanData, expectResync, testFunc) {
    jsTestLog("The node should rejoin the replica set. Clean data: " + cleanData + ". Expect resync: " + expectResync);
    // Skip clearing initial sync progress after a successful initial sync attempt so that we
    // can check initialSyncStatus fields after initial sync is complete.
    let node = replSet.start(originalNode, {
        dbpath: originalNode.dbpath,
        port: originalNode.port,
        restart: !cleanData,
        setParameter: {"failpoint.skipClearInitialSyncState": "{'mode':'alwaysOn'}"},
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

    assert.commandWorked(node.adminCommand({configureFailPoint: "skipClearInitialSyncState", mode: "off"}));

    testFunc(node);
    return node;
};

/**
 * Assert that mongo crashes on startup when files are missing or corrupt.
 */
export let assertErrorOnStartupWhenFilesAreCorruptOrMissing = function (dbpath, dbName, collName, deleteOrCorruptFunc) {
    // Start a MongoDB instance, create the collection file.
    const mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    const testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance and get the abrupt exit code (14).
    assert.eq(MongoRunner.EXIT_ABRUPT, runMongoProgram("mongod", "--port", mongod.port, "--dbpath", dbpath));
};

/**
 * Assert mongo crashes when files are missing or corrupt.
 */
export let assertErrorOnRequestWhenFilesAreCorruptOrMissing = function (
    dbpath,
    dbName,
    collName,
    deleteOrCorruptFunc,
    requestFunc,
) {
    // Start a MongoDB instance, create the collection file.
    let mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    let testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance.
    mongod = MongoRunner.runMongod({dbpath: dbpath, port: mongod.port, noCleanData: true});

    // This request crashes the server.
    testColl = mongod.getDB(dbName)[collName];
    requestFunc(testColl);

    MongoRunner.stopMongod(mongod, 9, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Runs the WiredTiger tool with the provided arguments.
 */
export let runWiredTigerTool = function (...args) {
    const cmd = ["wt"].concat(args);
    assert.eq(run.apply(undefined, cmd), 0, "error executing: " + cmd.join(" "));
};

/**
 * Stops the given mongod, runs the truncate command on the given uri using the WiredTiger tool, and
 * starts mongod again on the same path.
 */
export let truncateUriAndRestartMongod = function (uri, conn, mongodOptions) {
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    runWiredTigerTool("-h", conn.dbpath, "truncate", uri);
    return startMongodOnExistingPath(conn.dbpath, mongodOptions);
};

/**
 * Stops the given mongod and runs the alter command to modify the index table's metadata.
 */
export let alterIndexFormatVersion = function (uri, conn, formatVersion) {
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    runWiredTigerTool(
        "-h",
        conn.dbpath,
        "alter",
        "table:" + uri,
        "app_metadata=(formatVersion=" + formatVersion + "),exclusive_refreshed=false",
    );
};

/**
 * Stops the given mongod, dumps the table with the uri, modifies the content, and loads it back to
 * the table.
 */
export let count = 0;

export let rewriteTable = function (uri, conn, modifyData) {
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    const separator = _isWindows() ? "\\" : "/";
    const tempDumpFile = conn.dbpath + separator + "temp_dump";
    const newTableFile = conn.dbpath + separator + "new_table_file" + count++;
    runWiredTigerTool(
        "-h",
        conn.dbpath,
        "-r",
        "-C",
        "log=(compressor=snappy,path=journal)",
        "dump",
        "-x",
        "-f",
        tempDumpFile,
        "table:" + uri,
    );
    let dumpLines = cat(tempDumpFile).split("\n");
    modifyData(dumpLines);
    writeFile(newTableFile, dumpLines.join("\n"));
    runWiredTigerTool("-h", conn.dbpath, "load", "-f", newTableFile, "-r", uri);
};

// In WiredTiger table dumps, the first seven lines are the header and key that we don't want to
// modify. We will skip them and start from the line containing the first value.
export const wtHeaderLines = 7;

/**
 * Inserts the documents with duplicate field names into the MongoDB server.
 */
export let insertDocDuplicateFieldName = function (coll, uri, conn, numDocs) {
    for (let i = 0; i < numDocs; ++i) {
        coll.insert({a: "aaaaaaa", b: "bbbbbbb"});
    }
    // The format of the BSON documents will be {_id: ObjectId(), a: "aaaaaaa", a: "bbbbbbb"}.
    let makeDuplicateFieldNames = function (lines) {
        // The offset of the document's field name 'b' in the hex string dumped by wt tool.
        const offsetToFieldB = 75;
        // Each record takes two lines with a key and a value. We will only modify the values.
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            // Switch the field name 'b' to 'a' to create a duplicate field name.
            lines[i] = lines[i].substring(0, offsetToFieldB) + "1" + lines[i].substring(offsetToFieldB + 1);
        }
    };
    rewriteTable(uri, conn, makeDuplicateFieldNames);
};

export let insertDocSymbolField = function (coll, uri, conn, numDocs) {
    for (let i = 0; i < numDocs; ++i) {
        coll.insert({a: "aaaaaaa"});
    }
    let makeSymbolField = function (lines) {
        // The offset of the type of field 'a' in the hex string dumped by wt tool.
        const offsetToFieldAType = 43;
        // Each record takes two lines with a key and a value. We will only modify the values.
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            // Switch the field type from string to symbol.
            lines[i] = lines[i].substring(0, offsetToFieldAType) + "e" + lines[i].substring(offsetToFieldAType + 1);
        }
    };
    rewriteTable(uri, conn, makeSymbolField);
};

/**
 * Inserts array document with non-sequential indexes into the MongoDB server.
 */
export let insertNonSequentialArrayIndexes = function (coll, uri, conn, numDocs) {
    for (let i = 0; i < numDocs; ++i) {
        coll.insert({arr: [1, 2, [1, [1, 2], 2], 3]});
    }
    let makeNonSequentialIndexes = function (lines) {
        // The offset of the 0th index of the innermost array in the hex string dumped by wt tool.
        const offsetToNestedIndex0 = 179;
        // Each record takes two lines with a key and a value. We will only modify the values.
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            lines[i] = lines[i].substring(0, offsetToNestedIndex0) + "4" + lines[i].substring(offsetToNestedIndex0 + 1);
        }
    };
    rewriteTable(uri, conn, makeNonSequentialIndexes);
};

/**
 * Inserts documents with invalid regex options into the MongoDB server.
 */
export let insertInvalidRegex = function (coll, mongod, nDocuments) {
    const regex = "a*.conn";
    const options = "gimsuy";

    // First, insert valid expressions which will not be rejected by the JS interpreter.
    for (let i = 0; i < nDocuments; i++) {
        coll.insert({a: RegExp(regex, options)});
    }

    // Inserts 4 types of invalid expressions.
    let swapOptions = function (lines) {
        const toInsert = ["imlsux", "imzsux", "xuslmi", "amlsux"];
        const offsetToOptionStr = 64;
        const toHexStr = function (str) {
            return str
                .split("")
                .map((a) => {
                    return a.charCodeAt(0).toString(16);
                })
                .join("");
        };

        let modifiedOptions;
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            modifiedOptions = toHexStr(toInsert[((i - wtHeaderLines) / 2) % toInsert.length]);
            lines[i] =
                lines[i].substring(0, offsetToOptionStr) +
                modifiedOptions +
                lines[i].substring(offsetToOptionStr + modifiedOptions.length);
        }
    };
    rewriteTable(getUriForColl(coll), mongod, swapOptions);
};

/**
 * Inserts document with invalid UTF-8 string into the MongoDB server.
 */
export let insertInvalidUTF8 = function (coll, uri, conn, numDocs) {
    for (let i = 0; i < numDocs; ++i) {
        coll.insert({validString: "\x70"});
    }
    let makeInvalidUTF8 = function (lines) {
        // The offset of the first byte of the string, flips \x70 to \x80 (10000000) - invalid
        // because single byte UTF-8 cannot have a leading 1.
        const offsetToString = 76;
        // Each record takes two lines with a key and a value. We will only modify the values.
        for (let i = wtHeaderLines; i < lines.length; i += 2) {
            lines[i] = lines[i].substring(0, offsetToString) + "8" + lines[i].substring(offsetToString + 1);
        }
    };
    rewriteTable(uri, conn, makeInvalidUTF8);
};

/**
 * Loads the contents of the _mdb_catalog.wt file, and invokes the 'modifyCatalogEntry' callback for
 * each entry, which should modify the passed entry (in-place). The modified contents are then
 * written back to the _mdb_catalog.wt file.
 */
export let rewriteCatalogTable = function (conn, modifyCatalogEntry) {
    const uri = "_mdb_catalog";
    const fullURI = "table:" + uri;

    const separator = _isWindows() ? "\\" : "/";
    const tempDumpFile = conn.dbpath + separator + "temp_dump";
    const newTableFile = conn.dbpath + separator + "new_table_file" + count++;
    runWiredTigerTool(
        "-h",
        conn.dbpath,
        "-r",
        "-C",
        "log=(compressor=snappy,path=journal)",
        "dump",
        "-x",
        "-f",
        tempDumpFile,
        fullURI,
    );

    let lines = cat(tempDumpFile).split("\n");

    // Each record takes two lines with a key and a value. We will only modify the values.
    for (let i = wtHeaderLines; i < lines.length; i += 2) {
        let entry = hexToBSON(lines[i]);
        modifyCatalogEntry(entry);
        lines[i] = dumpBSONAsHex(entry);
    }

    writeFile(newTableFile, lines.join("\n"));

    runWiredTigerTool("-h", conn.dbpath, "alter", fullURI, "write_timestamp_usage=never");
    runWiredTigerTool("-h", conn.dbpath, "load", "-f", newTableFile, "-r", uri);
    runWiredTigerTool("-h", conn.dbpath, "alter", fullURI, "write_timestamp_usage=none");
};

/**
 * Extracts KV record lines from a WiredTiger dump output.
 */
export function wtExtractRecordsFromDump(lines) {
    const start = lines.findIndex((l) => l.trim() === "Data");
    if (start < 0) return [];
    return lines.slice(start + 1).filter((l) => l.trim() !== "");
}

/**
 * Dumps a WiredTiger table in the specified format.
 */
export function dumpWtTable(ident, dbpath, dumpType = "hex") {
    const DUMP_FLAGS = Object.freeze({
        "hex": ["-x"],
        "json": ["-j"],
        "pretty": ["-p"],
        "pretty-hex": ["-p", "-x"],
        "plain": [],
    });
    const flags = DUMP_FLAGS[dumpType];
    if (!flags) {
        throw new Error(`Invalid dumpType: ${dumpType}. Use one of: ${Object.keys(DUMP_FLAGS).join(", ")}`);
    }

    const sep = _isWindows() ? "\\" : "/";
    const tmp = dbpath + sep + "temp_dump.hex";
    runWiredTigerTool("-h", dbpath, "-r", "dump", ...flags, "-f", tmp, "table:" + ident);
    return cat(tmp).split("\n");
}

/**
 * Creates a new WiredTiger table with the specified ident and config string.
 */
export function createWtTable(dbpath, ident, cfg) {
    runWiredTigerTool("-h", dbpath, "create", "-c", cfg, "table:" + ident);
}
