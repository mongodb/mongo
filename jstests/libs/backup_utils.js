import {Thread} from "jstests/libs/parallelTester.js";

export function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo.getDB("admin"));
    let metadata = getBackupCursorMetadata(backupCursor);
    copyBackupCursorFiles(
        backupCursor, /*namespacesToSkip=*/[], metadata.dbpath, destinationDirectory);
    backupCursor.close();
    return metadata;
}

export function openBackupCursor(db, backupOptions, aggregateOptions) {
    // Opening a backup cursor can race with taking a checkpoint, resulting in a transient
    // error. Retry until it succeeds.
    backupOptions = backupOptions || {};
    aggregateOptions = aggregateOptions || {};

    while (true) {
        try {
            return db.aggregate([{$backupCursor: backupOptions}], aggregateOptions);
        } catch (exc) {
            jsTestLog({"Failed to open a backup cursor, retrying.": exc});
        }
    }
}

export function extendBackupCursor(mongo, backupId, extendTo) {
    return mongo.getDB("admin").aggregate(
        [{$backupCursorExtend: {backupId: backupId, timestamp: extendTo}}],
        {maxTimeMS: 180 * 1000});
}

export function startHeartbeatThread(host, backupCursor, session, stopCounter) {
    let cursorId = tojson(backupCursor._cursorid);
    let lsid = tojson(session.getSessionId());

    let heartbeatBackupCursor = function(host, cursorId, lsid, stopCounter) {
        const conn = new Mongo(host);
        const db = conn.getDB("admin");
        while (stopCounter.getCount() > 0) {
            let res = assert.commandWorked(db.runCommand({
                getMore: eval("(" + cursorId + ")"),
                collection: "$cmd.aggregate",
                lsid: eval("(" + lsid + ")")
            }));
            sleep(10 * 1000);
        }
    };

    const heartbeater = new Thread(heartbeatBackupCursor, host, cursorId, lsid, stopCounter);
    heartbeater.start();
    return heartbeater;
}

export function getBackupCursorMetadata(backupCursor) {
    assert(backupCursor.hasNext());
    let doc = backupCursor.next();
    assert(doc.hasOwnProperty("metadata"));
    return doc["metadata"];
}

/**
 * Exhaust the backup cursor and copy all the listed files to the destination directory. If `async`
 * is true, this function will spawn a Thread doing the copy work and return the thread along
 * with the backup cursor metadata. The caller should `join` the thread when appropriate.
 */
export function copyBackupCursorFiles(
    backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    resetDbpath(destinationDirectory);
    let separator = _isWindows() ? '\\' : '/';
    // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
    mkdir(destinationDirectory + separator + "journal");

    let copyThread = copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback);
    return copyThread;
}

export function copyBackupCursorExtendFiles(
    cursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    let files = _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback);
    let copyThread;
    if (async) {
        copyThread = new Thread(_copyFiles, files, dbpath, destinationDirectory, _copyFileHelper);
        copyThread.start();
    } else {
        _copyFiles(files, dbpath, destinationDirectory, _copyFileHelper);
    }

    // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
    jsTestLog({
        msg: "Destination",
        destination: destinationDirectory,
        dbpath: ls(destinationDirectory),
        journal: ls(destinationDirectory + "/journal")
    });

    return copyThread;
}

export function _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback) {
    let files = [];
    while (cursor.hasNext()) {
        let doc = cursor.next();
        assert(doc.hasOwnProperty("filename"));

        if (namespacesToSkip.includes(doc.ns)) {
            jsTestLog("Skipping file during backup: " + tojson(doc));
            continue;
        }

        if (fileCopiedCallback) {
            fileCopiedCallback(doc);
        }

        let file = {filename: doc.filename};

        if (doc.hasOwnProperty("fileSize")) {
            file.fileSize = Number(doc.fileSize);
        }

        if (doc.hasOwnProperty("offset")) {
            assert(doc.hasOwnProperty("length"));
            file.offset = Number(doc.offset);
            file.length = Number(doc.length);
        }

        files.push(file);
    }
    return files;
}

export function _copyFiles(files, dbpath, destinationDirectory, copyFileHelper) {
    files.forEach((file) => {
        jsTestLog(copyFileHelper(file, dbpath, destinationDirectory));
    });
}

export function _copyFileHelper(file, sourceDbPath, destinationDirectory) {
    let absoluteFilePath = file.filename;

    // Ensure the dbpath ends with an OS appropriate slash.
    let separator = '/';
    if (_isWindows()) {
        separator = '\\';
        // Convert dbpath which may contain directoryperdb/wiredTigerDirectoryForIndexes
        // subdirectory in POSIX style.
        absoluteFilePath = absoluteFilePath.replace(/[\/]/g, separator);
    }
    let lastChar = sourceDbPath[sourceDbPath.length - 1];
    if (lastChar !== '/' && lastChar !== '\\') {
        sourceDbPath += separator;
    }

    // Ensure that the full path starts with the returned dbpath.
    assert.eq(0, absoluteFilePath.indexOf(sourceDbPath));

    // Grab the file path relative to the dbpath. Maintain that relation when copying
    // to the `hiddenDbpath`.
    let relativePath = absoluteFilePath.substr(sourceDbPath.length);
    let destination = destinationDirectory + separator + relativePath;
    const newFileDirectory = destination.substring(0, destination.lastIndexOf(separator));
    mkdir(newFileDirectory);

    let msg = "File copy";
    if (!pathExists(destination)) {
        // If the file hasn't had an initial backup yet, then a full file copy is needed.
        copyFile(absoluteFilePath, destination);
    } else if (file.fileSize == undefined || file.length == undefined ||
               file.fileSize == file.length) {
        // - $backupCursorExtend, which only returns journal files does not report a 'fileSize'.
        // - 'length' is only reported for incremental backups.
        // - When 'fileSize' == 'length', that's used as an indicator to do a full file copy. Mostly
        // used for internal tables.
        if (pathExists(destination)) {
            // Remove the old backup of the file. For journal files specifically, if a checkpoint
            // didn't take place between two incremental backups, then the backup cursor can specify
            // journal files we've already copied at an earlier time. We should remove these old
            // journal files so that we can copy them over again in the event that their contents
            // have changed over time.
            jsTestLog(`Removing existing file ${
                destination} in preparation of copying a newer version of it`);
            removeFile(destination);
        }

        copyFile(absoluteFilePath, destination);
    } else {
        assert(file.offset != undefined);
        assert(file.length != undefined);
        msg = "Range copy, offset: " + file.offset + ", length: " + file.length;
        _copyFileRange(
            absoluteFilePath, destination, NumberLong(file.offset), NumberLong(file.length));
    }

    return {
        fileSource: absoluteFilePath,
        relativePath: relativePath,
        fileDestination: destination,
        msg: msg
    };
}

/**
 * Helper function to ensure the namespace and UUID fields are correctly populated in files we are
 * backing up.
 */
export function checkBackup(backupCursor) {
    // Print the metadata document.
    assert(backupCursor.hasNext());
    jsTestLog(backupCursor.next());

    while (backupCursor.hasNext()) {
        let doc = backupCursor.next();

        jsTestLog("File for backup: " + tojson(doc));

        if (!doc.required) {
            assert.neq(doc.ns, "");
            assert.neq(doc.uuid, "");
        } else {
            let pathsep = _isWindows() ? '\\' : '/';
            let stem = doc.filename.substr(doc.filename.lastIndexOf(pathsep) + 1);
            // Denylisting internal files that don't need to have ns/uuid set. Denylisting known
            // patterns will help catch subtle API changes if new filename patterns are added that
            // don't generate ns/uuid.
            if (!stem.startsWith("size") && !stem.startsWith("Wired") && !stem.startsWith("_")) {
                assert.neq(doc.ns, "");
                assert.neq(doc.uuid, "");
            }
        }
    }
}

// Magic restore utility class

// This class implements helpers for testing the magic restore proces. It maintains the state of the
// backup cursor and handles writing objects to named pipes and running magic restore on a single
// node. It exposes some of this state so that individual tests can make specific assertions as
// needed.

export class MagicRestoreUtils {
    constructor({backupSource, pipeDir, insertHigherTermOplogEntry, backupDbPathSuffix}) {
        this.backupSource = backupSource;
        this.pipeDir = pipeDir;
        this.backupDbPath =
            pipeDir + "/backup" + (backupDbPathSuffix != undefined ? backupDbPathSuffix : "");

        // isPit is set when we receive the restoreConfiguration.
        this.isPit = false;
        this.insertHigherTermOplogEntry = insertHigherTermOplogEntry;
        // Default high term value.
        this.restoreToHigherTermThan = 100;

        // These fields are set during the restore process.
        this.backupCursor = undefined;
        this.backupId = undefined;
        this.checkpointTimestamp = undefined;
        this.pointInTimeTimestamp = undefined;
    }

    /**
     * Helper function that returns the checkpoint timestamp from the backup cursor. Used in tests
     * that need this timestamp to make assertions about data before and after the backup time.
     */
    getCheckpointTimestamp() {
        return this.checkpointTimestamp;
    }

    /**
     * Helper function that returns the dbpath for the backup. Used to start a regular node after
     * magic restore completes.
     */
    getBackupDbPath() {
        return this.backupDbPath;
    }

    /**
     * Takes a checkpoint and opens the backup cursor on the source. backupCursorOpts is an optional
     * parameter which will be passed to the openBackupCursor call if provided. This function
     * returns the backup cursor metadata object.
     */
    takeCheckpointAndOpenBackup(backupCursorOpts = {}) {
        // Take the initial checkpoint.
        assert.commandWorked(this.backupSource.adminCommand({fsync: 1}));

        resetDbpath(this.backupDbPath);
        // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
        mkdir(this.backupDbPath + "/journal");

        // Open a backup cursor on the checkpoint.
        this.backupCursor = openBackupCursor(this.backupSource.getDB("admin"), backupCursorOpts);
        // Print the backup metadata document.
        assert(this.backupCursor.hasNext());
        const {metadata} = this.backupCursor.next();
        jsTestLog("Backup cursor metadata document: " + tojson(metadata));
        this.backupId = metadata.backupId;
        this.checkpointTimestamp = metadata.checkpointTimestamp;
        return metadata;
    }

    /**
     * Copies data files from the source dbpath to the backup dbpath.
     */
    copyFiles() {
        while (this.backupCursor.hasNext()) {
            const doc = this.backupCursor.next();
            jsTestLog("Copying for backup: " + tojson(doc));
            _copyFileHelper({filename: doc.filename, fileSize: doc.fileSize},
                            this.backupSource.dbpath,
                            this.backupDbPath);
        }
    }

    /**
     * Copies data files from the source dbpath to the backup dbpath. Closes the backup cursor.
     */
    copyFilesAndCloseBackup() {
        this.copyFiles();
        this.backupCursor.close();
    }

    /**
     * Extends the backup cursor, copies the extend files and closes the backup cursor.
     */
    extendAndCloseBackup(mongo, maxCheckpointTs) {
        extendBackupCursor(mongo, this.backupId, maxCheckpointTs);
        copyBackupCursorExtendFiles(this.backupCursor,
                                    [] /*namespacesToSkip*/,
                                    this.backupSource.dbpath,
                                    this.backupDbPath,
                                    false /*async*/);
        this.backupCursor.close();
    }

    /**
     * Helper function that generates the magic restore named pipe path for testing. 'pipeDir'
     * is the directory in the filesystem in which we create the named pipe.
     */
    static _generateMagicRestorePipePath(pipeDir) {
        const pipeName = "magic_restore_named_pipe";
        // On Windows, the pipe path prefix is ignored. "//./pipe/" is the required path start of
        // all named pipes on Windows.
        const pipePath = _isWindows() ? "//./pipe/" + pipeName : `${pipeDir}/tmp/${pipeName}`;
        if (!_isWindows() && !fileExists(pipeDir + "/tmp/")) {
            assert(mkdir(pipeDir + "/tmp/").created);
        }
        return {pipeName, pipePath};
    }

    /**
     * Helper function that writes an array of JavaScript objects into a named pipe. 'objs' will be
     * serialized into BSON and written into the named pipe path generated by
     * '_generateMagicRestorePipePath'. The function is static as it is used in passthrough testing
     * as well.
     */
    static writeObjsToMagicRestorePipe(pipeDir, objs, persistPipe = false) {
        const {pipeName, pipePath} = MagicRestoreUtils._generateMagicRestorePipePath(pipeDir);
        _writeTestPipeObjects(pipeName, objs.length, objs, pipeDir + "/tmp/", persistPipe);
        // Creating the named pipe is async, so we should wait until the file exists.
        assert.soon(() => fileExists(pipePath));
    }

    /**
     * Helper function that starts a magic restore node on the 'backupDbPath'. Waits for the process
     * to exit cleanly. The function is static as it is used in passthrough testing as well.
     */
    static runMagicRestoreNode(pipeDir, backupDbPath, options = {}) {
        const {pipePath} = MagicRestoreUtils._generateMagicRestorePipePath(pipeDir);
        // Magic restore will exit the mongod process cleanly. 'runMongod' may acquire a connection
        // to mongod before it exits, and so we wait for the process to exit in the 'assert.soon'
        // below. If mongod exits before we acquire a connection, 'conn' will be null. In this case,
        // if mongod exits with non-zero exit code, the runner will throw a StopError.
        const conn = MongoRunner.runMongod({
            dbpath: backupDbPath,
            noCleanData: true,
            magicRestore: "",
            env: {namedPipeInput: pipePath},
            ...options
        });
        if (conn) {
            assert.soon(() => {
                const res = checkProgram(conn.pid);
                return !res.alive && res.exitCode == MongoRunner.EXIT_CLEAN;
            }, "Expected magic restore to exit mongod cleanly");
        }
    }

    /**
     * Avoids nested loops (some including the config servers and some without) in the test itself.
     */
    static getAllNodes(numShards, numNodes) {
        const result = [];
        for (let rsIndex = 0; rsIndex < numShards; rsIndex++) {
            for (let nodeIndex = 0; nodeIndex < numNodes; nodeIndex++) {
                result.push([rsIndex, nodeIndex]);
            }
        }
        return result;
    }

    /**
     * Helper function that lists databases on the given ShardingTest, calls dbHash for each and
     * returns those as a dbName-to-shardIdx-to-nodeIdx-to-dbHash dictionary.
     */
    static getDbHashes(st, numShards, numNodes, expectedDBCount) {
        const dbHashes = {};
        const res = assert.commandWorked(st.s.adminCommand({"listDatabases": 1, "nameOnly": true}));
        assert.eq(res["databases"].length, expectedDBCount);

        const allNodes = this.getAllNodes(numShards + 1, numNodes);
        for (let dbEntry of res["databases"]) {
            const dbName = dbEntry["name"];
            dbHashes[dbName] = {};
            for (const [rsIndex, nodeIndex] of allNodes) {
                const node = rsIndex < numShards ? st["rs" + rsIndex].nodes[nodeIndex]
                                                 : st.configRS.nodes[nodeIndex];
                const dbHash = node.getDB(dbName).runCommand({dbHash: 1});
                if (dbHashes[dbName][rsIndex] == undefined) {
                    dbHashes[dbName][rsIndex] = {};
                }
                dbHashes[dbName][rsIndex][nodeIndex] = dbHash;
            }
        }

        return dbHashes;
    }

    /**
     * Helper function that compares outputs of dbHash on the given replicaSets against the output
     * of a previous getDbHashes call. The config server replica set can be passed in replicaSets
     * too. Collection names in excludedCollections are excluded from the comparison.
     */
    static checkDbHashes(dbHashes, replicaSets, excludedCollections, numShards, numNodes) {
        for (const [rsIndex, nodeIndex] of this.getAllNodes(numShards + 1, numNodes)) {
            for (const dbName in dbHashes) {
                if (dbHashes[dbName][rsIndex] != undefined) {
                    let dbhash =
                        replicaSets[rsIndex].nodes[nodeIndex].getDB(dbName).runCommand({dbHash: 1});
                    for (let collectionName in dbhash["collections"]) {
                        if (excludedCollections.includes(collectionName)) {
                            continue;
                        }
                        assert.eq(
                            dbhash["collections"][collectionName],
                            dbHashes[dbName][rsIndex][nodeIndex]["collections"][collectionName],
                            `Comparing ${collectionName} in ${dbName} on replicaSets[${
                                rsIndex}], node ${nodeIndex} failed`);
                    }
                }
            }
        }
    }

    /**
     * Replaces the trailing "_0" from the backupPath of the first node of a replica set by "_$node"
     * so startSet uses the correct dbpath for each node in the replica set.
     */
    static parameterizeDbpath(backupPath) {
        return backupPath.slice(0, -1) + "$node";
    }

    /**
     * Retrieves all oplog entries that occurred after the checkpoint timestamp on the source node.
     * Returns an object with the timestamp of the last oplog entry, as well as the oplog
     * entry array
     */
    getEntriesAfterBackup(sourceNode) {
        let oplog = sourceNode.getDB("local").getCollection('oplog.rs');
        const entriesAfterBackup =
            oplog.find({ts: {$gt: this.checkpointTimestamp}}).sort({ts: 1}).toArray();
        return {
            lastOplogEntryTs: entriesAfterBackup[entriesAfterBackup.length - 1].ts,
            entriesAfterBackup
        };
    }

    /**
     * Performs a find on the oplog for the given name space and asserts that the expected number of
     * entries exists. Optionally takes an op type to filter.
     */
    assertOplogCountForNamespace(node, ns, expectedNumEntries, op) {
        let findObj = {ns: ns};
        if (op) {
            findObj.op = op;
        }
        const entries =
            node.getDB("local").getCollection('oplog.rs').find(findObj).sort({ts: -1}).toArray();
        assert.eq(entries.length, expectedNumEntries);
    }

    /**
     * Adds the 'restoreToHigherTermThan' field to the restore configuration if this instance is
     * testing the higher term no-op behavior.
     */
    appendRestoreToHigherTermThanIfNeeded(restoreConfiguration) {
        if (this.insertHigherTermOplogEntry) {
            restoreConfiguration.restoreToHigherTermThan = NumberLong(this.restoreToHigherTermThan);
        }
        return restoreConfiguration;
    }

    /**
     * Combines writing objects to the named pipe and running magic restore.
     */
    writeObjsAndRunMagicRestore(restoreConfiguration, entriesAfterBackup, options) {
        this.pointInTimeTimestamp = restoreConfiguration.pointInTimeTimestamp;
        if (this.pointInTimeTimestamp) {
            assert(entriesAfterBackup.length > 0);
            this.isPit = true;
        }
        MagicRestoreUtils.writeObjsToMagicRestorePipe(
            this.pipeDir, [restoreConfiguration, ...entriesAfterBackup]);
        MagicRestoreUtils.runMagicRestoreNode(this.pipeDir, this.backupDbPath, options);
    }

    /**
     * Asserts that two config arguments are equal. If we are testing higher term behavior in the
     * test, modifies the expected term on the source config.
     */
    assertConfigIsCorrect(srcConfig, dstConfig) {
        // If we passed in a value for the 'restoreToHigherTermThan' field in the restore config, a
        // no-op oplog entry was inserted in the oplog with that term value + 100. On startup, the
        // replica set node sets its term to this value. A new election occurred when the replica
        // set restarted, so we must also increment the term by 1 regardless of if we passed in a
        // higher term value.
        const expectedTerm = this.insertHigherTermOplogEntry ? this.restoreToHigherTermThan + 101
                                                             : srcConfig.term + 1;
        // Make a copy to not modify the object passed by the caller.
        srcConfig = Object.assign({}, srcConfig);
        srcConfig.term = expectedTerm;
        assert.eq(srcConfig, dstConfig);
    }

    /**
     * Asserts that the stable checkpoint timestamp on the restored node is as expected. For a
     * non-PIT restore, the timestamp should be equal to the checkpoint timestamp from the backup
     * cursor. For a PIT restore, it should be equal to the top of the oplog. For any restore that
     * inserts a no-op oplog entry with a higher term, the stable checkpoint timestamp should be
     * equal to the timestamp of that entry.
     */
    assertStableCheckpointIsCorrectAfterRestore(restoreNode, lastOplogEntryTs = undefined) {
        let lastStableCheckpointTs =
            this.isPit ? this.pointInTimeTimestamp : this.checkpointTimestamp;

        // For a PIT restore on a sharded cluster, the lastStableCheckpointTs of a given shard might
        // be behind the global pointInTimeTimestamp. This allows the caller to specify
        // lastOplogEntryTs instead.
        if (lastOplogEntryTs != undefined) {
            lastStableCheckpointTs = lastOplogEntryTs;
        }

        if (this.insertHigherTermOplogEntry) {
            const oplog = restoreNode.getDB("local").getCollection('oplog.rs');
            const incrementTermEntry =
                oplog.findOne({op: "n", "o.msg": "restore incrementing term"});
            assert(incrementTermEntry);
            assert.eq(incrementTermEntry.t, this.restoreToHigherTermThan + 100);
            // If we've inserted a no-op oplog entry with a higher term during magic restore, we'll
            // have updated the stable timestamp.
            lastStableCheckpointTs = incrementTermEntry.ts;
        }

        // Ensure that the last stable checkpoint is as expected. As the timestamp is greater than
        // 0, this means the magic restore took a stable checkpoint on shutdown.
        const {lastStableRecoveryTimestamp} =
            assert.commandWorked(restoreNode.adminCommand({replSetGetStatus: 1}));
        assert(timestampCmp(lastStableRecoveryTimestamp, lastStableCheckpointTs) == 0,
               `timestampCmp(${tojson(lastStableRecoveryTimestamp)}, ${
                   tojson(lastStableCheckpointTs)}) is ${
                   timestampCmp(lastStableRecoveryTimestamp, lastStableCheckpointTs)}, not 0`);
    }

    /**
     * Assert that the minvalid document has been set correctly in magic restore.
     */
    assertMinValidIsCorrect(restoreNode) {
        const minValid = restoreNode.getCollection('local.replset.minvalid').findOne();
        assert.eq(minValid,
                  {_id: ObjectId("000000000000000000000000"), t: -1, ts: Timestamp(0, 1)});
    }

    /**
     * Assert that a restored node cannot complete a snapshot read at a timestamp earlier than the
     * last stable checkpoint timestamp.
     */
    assertCannotDoSnapshotRead(restoreNode, expectedNumDocs) {
        const {lastStableRecoveryTimestamp} =
            assert.commandWorked(restoreNode.adminCommand({replSetGetStatus: 1}));
        // A restored node will not preserve any history. The oldest timestamp should be set to the
        // stable timestamp at the end of a non-PIT restore.
        let res = restoreNode.getDB("db").runCommand({
            find: "coll",
            readConcern: {
                level: "snapshot",
                atClusterTime: Timestamp(lastStableRecoveryTimestamp.getTime() - 1,
                                         lastStableRecoveryTimestamp.getInc())
            }
        });
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);

        // A snapshot read at the last stable timestamp should succeed.
        res = restoreNode.getDB("db").runCommand({
            find: "coll",
            readConcern: {level: "snapshot", atClusterTime: lastStableRecoveryTimestamp}
        });
        assert.commandWorked(res);
        assert.eq(res.cursor.firstBatch.length, expectedNumDocs);
    }

    /**
     * Get the UUID for a given collection.
     */
    getCollUuid(node, dbName, collName) {
        return node.getDB(dbName).getCollectionInfos({name: collName})[0].info.uuid;
    }

    /**
     * Checks the log file specified at 'logpath', and ensures that it contains logs with the
     * 'RESTORE' component. The function also ensures the first and last log lines are as expected.
     */
    checkRestoreSpecificLogs(logpath) {
        // When splitting the logs on new lines, the last element will be an empty string, so we
        // should filter it out.
        let logs = cat(logpath)
                       .split("\n")
                       .filter(line => line.trim() !== "")
                       .map(line => JSON.parse(line))
                       .filter(json => json.c === 'RESTORE');
        assert.eq(logs[0].msg, "Beginning magic restore");
        assert.eq(logs[logs.length - 1].msg, "Finished magic restore");
    }
}
